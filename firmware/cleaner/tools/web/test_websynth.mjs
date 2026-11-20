// SPDX-License-Identifier: GPL-3.0-only
// test_websynth.mjs SYNTH.html LITE.html BANK.syx: the sound in the browser (make_page.py --synth) in
// Chromium (Playwright from the working directory's node_modules or the global ones), the pages opened from disk:
//   the page without it (make_page.py's default) has none of it;
//   the synth page: the switch starts the worklet, a key of the computer plays the page's own path (the dock's keys ->
//   WS.noteOn -> the worklet's engine), the output is not silent and at the key's pitch (A4 = 440 Hz: the init patch's
//   saw, its period by autocorrelation); the on-screen keyboard plays; the Editor's controls drive it without an FM-1
//   (the cutoff closed: the level falls); a bank file's ▶ (the Patches tab's preview) and a click in the patch manager's
//   local list load their patch into it (and into the editor, no FM-1 there); off again: the editor dimmed.
//   The player bar (1.18C47w2): hidden at load, shown while the sound is on, its "×" switches the sound off and hides
//   it; the on-screen keyboard only on request (the Editor's "Keyboard" (which also switches the sound on), the bar's,
//   "×", Esc), remembered across a reload; the patch manager's lists: a ▶ on each filled row (none on an empty one) that
//   switches the sound on, loads and sounds the patch, shows the bar but not the keyboard, and stops on a second press.
//   The Patches tab's file list (step 1), its ▶ browser first: with no device no MIDI access asked (requestMIDIAccess
//   spied on), the sound switched on, the bar shown, the single sounding, ■ stops; the bar's audition settings follow
//   the tab's; with a device connected and "Notes to" Browser nothing goes to the device.
//   And the engine's speed in this browser: the embedded module rendering a heavy patch (14 voices, unison, reverb with
//   the shimmer, chorus, distortion, osc 3) on the page's main thread, as real-time factor (printed, checked > 2).
import { createRequire } from "node:module";
import { pathToFileURL } from "node:url";
const { chromium } = createRequire(process.cwd() + "/")("playwright");

const [synthPage, litePage, bank] = process.argv.slice(2);
if (!synthPage || !litePage || !bank) { console.error("usage: test_websynth.mjs SYNTH.html LITE.html BANK.syx"); process.exit(2); }
let fails = 0;
const ok = (c, what) => { if (!c) { fails++; console.log("FAIL " + what); } return c; };

const browser = await chromium.launch();
try {
  // ---- the page without the synth ----
  {
    const p = await browser.newPage(), errs = [];
    p.on("pageerror", (e) => errs.push(String(e)));
    await p.goto(pathToFileURL(litePage).href + "#editor");
    const r = await p.evaluate(() => ({ dock: !!document.getElementById("ws-dock"), data: !!document.getElementById("wsdata"),
                                       ws: typeof window.infekt.WS, edoff: document.getElementById("e-body").className }));
    ok(!r.dock && !r.data && r.ws === "undefined", "lite: no web synth expected " + JSON.stringify(r));
    ok(r.edoff === "ed-off", "lite: the editor dimmed without an FM-1");
    ok(!errs.length, "lite: page errors " + errs.join(" | "));
    await p.close();
  }
  // ---- the synth page ----
  const p = await browser.newPage(), errs = [];
  p.on("pageerror", (e) => errs.push(String(e)));
  p.on("console", (m) => { if (m.type() === "error") errs.push(m.text()); });
  await p.goto(pathToFileURL(synthPage).href + "#editor");
  const vis = () => p.evaluate(() => ({ dock: !document.getElementById("ws-dock").hidden, kb: !document.getElementById("ws-kbw").hidden,
                                         on: window.infekt.WS.on }));
  let v = await vis();
  ok(!v.dock && !v.kb && !v.on, "at load: no player bar, no keyboard, the sound off " + JSON.stringify(v));
  ok(await p.isVisible("#ws-on") && await p.isVisible("#ws-ekb"), "the Editor's head: the sound switch and the Keyboard button");
  ok((await p.getAttribute("#e-body", "class")) === "ed-off", "the editor dimmed before the sound is on");
  await p.click("#ws-on");
  await p.waitForFunction(() => window.infekt.WS.on || /Fehler|error|kann/.test(document.getElementById("ws-msg").textContent),
                          null, { timeout: 20000 });
  const st = await p.evaluate(() => ({ on: window.infekt.WS.on, rate: window.infekt.WS.ctx && window.infekt.WS.ctx.sampleRate,
                                      msg: document.getElementById("ws-msg").textContent }));
  if (!ok(st.on, "the sound on: " + st.msg)) throw new Error("not started");
  ok((await p.getAttribute("#e-body", "class")) === "", "the editor live with the browser's sound");
  v = await vis();
  ok(v.dock && !v.kb, "the sound on: the player bar shown, the keyboard not " + JSON.stringify(v));
  // an analyser on the worklet's output (again after each start: a new node)
  const tap = () => p.evaluate(() => {
    const W = window.infekt.WS, an = W.ctx.createAnalyser();
    an.fftSize = 8192; W.node.connect(an); window.__an = an;
  });
  await tap();
  const grab = () => p.evaluate(() => {
    const an = window.__an, d = new Float32Array(an.fftSize);
    an.getFloatTimeDomainData(d);
    let e = 0;
    for (const x of d) e += x * x;
    const rms = Math.sqrt(e / d.length), fs = window.infekt.WS.ctx.sampleRate;
    // the period: the autocorrelation's first peak after its first minimum (60 .. 2000 Hz)
    let best = 0, lag = 0, dip = false;
    for (let k = Math.floor(fs / 2000); k < fs / 60; k++) {
      let c = 0;
      for (let i = 0; i + k < d.length; i++) c += d[i] * d[i + k];
      if (!dip) { if (c < 0) dip = true; continue; }
      if (c > best) { best = c; lag = k; } else if (best > 0 && c < best * 0.5) break;
    }
    return { rms, hz: lag ? fs / lag : 0 };
  });
  // A4 on the computer's keys: the octave up (C4), then H (the 10th semitone: A)
  await p.click("#ws-oup");
  ok((await p.textContent("#ws-oct")) === "C4", "the octave C4");
  await p.focus("body");
  await p.keyboard.down("KeyH");
  await p.waitForTimeout(600);
  const a4 = await grab();
  const meter = await p.textContent("#ws-meter");
  await p.keyboard.up("KeyH");
  ok(a4.rms > 0.01, `A4: not silent (RMS ${a4.rms.toFixed(4)})`);
  ok(Math.abs(a4.hz / 440 - 1) < 0.01, `A4: the pitch ${a4.hz.toFixed(1)} Hz (440 expected)`);
  ok(/^[1-9]\d* /.test(meter), "the voices shown while a note plays: " + meter);
  // the on-screen keyboard: opened by the bar's button; its C4 (a white key) held by the mouse
  await p.waitForTimeout(300);
  await p.click("#ws-kbt");
  v = await vis();
  ok(v.kb && (await p.getAttribute("#ws-kbt", "aria-expanded")) === "true", "the bar's Keyboard button opens it");
  const key = await p.$('#ws-kb > div[data-n="60"]');
  const bx = await key.boundingBox();
  await p.mouse.move(bx.x + bx.width / 2, bx.y + bx.height * 0.8);
  await p.mouse.down();
  await p.waitForTimeout(500);
  const c4 = await grab();
  const heldC4 = await p.evaluate(() => document.querySelector('#ws-kb > div[data-n="60"]').classList.contains("on"));
  ok(c4.rms > 0.01 && Math.abs(c4.hz / 261.63 - 1) < 0.01, `on-screen C4: RMS ${c4.rms.toFixed(4)}, ${c4.hz.toFixed(1)} Hz`);
  ok(heldC4, "the key lit while held");
  // the Editor: the cutoff closed while the note plays (the editor's own set: ed.setByte, as a knob)
  await p.evaluate(() => window.infekt.ed.setByte(40, 0));
  await p.waitForTimeout(400);
  const shut = await grab();
  ok(shut.rms < c4.rms * 0.3, `the cutoff closed from the editor: RMS ${c4.rms.toFixed(4)} -> ${shut.rms.toFixed(4)}`);
  await p.mouse.up();
  // the keyboard: "×" closes it, the Editor's button opens it again, Esc closes it; the computer's keys play without it
  await p.click("#ws-kbx");
  v = await vis();
  ok(!v.kb && v.dock && v.on, "its × closes the keyboard (the bar and the sound stay) " + JSON.stringify(v));
  await p.click("#ws-ekb");
  ok((await vis()).kb, "the Editor's Keyboard button opens it again");
  await p.keyboard.press("Escape");
  ok(!(await vis()).kb, "Esc closes it");
  await p.evaluate(() => window.infekt.ed.setByte(40, 127));
  await p.focus("body");
  await p.keyboard.down("KeyH");
  await p.waitForTimeout(500);
  const kh = await grab();
  await p.keyboard.up("KeyH");
  ok(kh.rms > 0.01, `the computer's keys play with the keyboard closed (RMS ${kh.rms.toFixed(4)})`);
  // the bar's ×: the sound off, the bar gone; the Editor's Keyboard button: the sound on again, the keyboard open;
  // its state remembered over a reload (the bar again only once the sound is on)
  await p.click("#ws-close");
  v = await vis();
  ok(!v.dock && !v.on, "the bar's × switches the sound off and hides the bar " + JSON.stringify(v));
  await p.click("#ws-ekb");
  await p.waitForFunction(() => window.infekt.WS.on, null, { timeout: 15000 });
  v = await vis();
  ok(v.dock && v.kb && v.on, "the Editor's Keyboard button with the sound off: the sound on, the bar and the keyboard shown " + JSON.stringify(v));
  await p.reload();
  await p.waitForTimeout(300);
  v = await vis();
  ok(!v.dock && !v.on && (await p.getAttribute("#ws-ekb", "aria-expanded")) === "true", "after a reload: no bar until the sound is on, the keyboard remembered open");
  await p.click("#ws-on");
  await p.waitForFunction(() => window.infekt.WS.on, null, { timeout: 15000 });
  v = await vis();
  ok(v.dock && v.kb, "the sound on: the bar with the keyboard as it was left " + JSON.stringify(v));
  await p.click("#ws-kbx");
  await tap();
  // the Patches tab: a bank file's preview (▶) plays in the browser, into the editor too (no FM-1)
  await p.evaluate(() => window.infekt.showTab("patches"));
  ok(await p.isVisible("#ws-dock"), "the dock on the Patches tab");
  await p.setInputFiles("#b-file", bank);
  await p.waitForSelector("#b-list button.play");
  await p.selectOption("#b-pvmode", "chord");
  await p.click("#b-list tbody tr:nth-child(3) button.play");
  await p.waitForTimeout(400);
  const pv = await grab();
  const pvName = await p.evaluate(() => [window.infekt.WS.name, document.getElementById("e-name").value]);
  ok(pvName[0] === "TEST 002" && pvName[1] === "TEST 002", "the preview's patch in the browser and the editor: " + pvName.join(" / "));
  ok(pv.rms > 0 || pvName[0] === "TEST 002", "the preview played");
  // the patch manager: its local list, a click auditions
  await p.click("#org-open");
  await p.setInputFiles("#org-lfile", bank);
  await p.waitForSelector("#org-llist .orow:not(.empty)");
  await p.click('#org-llist .orow[data-i="5"] .oname');
  await p.waitForTimeout(200);
  ok((await p.evaluate(() => window.infekt.WS.name)) === "TEST 005", "a click in the local list auditions TEST 005");
  // its ▶: on each filled row, none on an empty one (the bank's slots past its 32 patches); with the sound off:
  // switched on, the bar shown, the keyboard left closed, the patch loaded and sounding; ■ stops it
  const rows = await p.evaluate(() => {
    const L = window.infekt.org.LB.banks[window.infekt.org.LB.cur], rs = [...document.querySelectorAll("#org-llist .orow")];
    return { filled: rs.filter((r) => L.slots[+r.dataset.i]).length, btns: rs.filter((r) => r.querySelector(".wsplay")).length,
             emptyBtn: rs.filter((r) => !L.slots[+r.dataset.i] && r.querySelector(".wsplay")).length, empties: rs.filter((r) => r.classList.contains("empty")).length };
  });
  ok(rows.filled > 5 && rows.btns === rows.filled && rows.emptyBtn === 0 && rows.empties > 0, "a ▶ on each filled row, none on an empty one " + JSON.stringify(rows));
  await p.evaluate(() => window.infekt.showTab("editor"));
  await p.click("#ws-close");
  await p.evaluate(() => window.infekt.showTab("patches"));
  ok(!(await vis()).dock, "the sound off: no bar on the Patches tab");
  await p.evaluate(() => {               // (TEST 009 made audible: the init patch's sound under its name; the made-up
    const it = window.infekt.org.LB.banks[window.infekt.org.LB.cur].slots[9], D = JSON.parse(document.getElementById("eddata").textContent);   // bytes may be silent)
    for (let k = 0; k < 240; k++) it.data[k] = D.init[k];
  });
  await p.evaluate(() => { document.getElementById("b-pvmode").value = "chord"; document.getElementById("b-pvlen").value = "3"; });   // (the
  // preview's settings, hidden while the patch manager is open)
  await p.click('#org-llist .orow[data-i="9"] .wsplay');
  await p.waitForFunction(() => window.infekt.WS.on && window.infekt.WS.name === "TEST 009", null, { timeout: 15000 });
  await tap();
  await p.waitForTimeout(500);
  const pl = await grab();
  v = await vis();
  const btn = await p.evaluate(() => { const b = document.querySelector('#org-llist .orow[data-i="9"] .wsplay'); return [b.textContent, b.getAttribute("aria-pressed")]; });
  ok(v.on && v.dock && !v.kb, "▶: the sound on, the bar shown, the keyboard left closed " + JSON.stringify(v));
  ok(pl.rms > 0.005, `▶: TEST 009 sounds (RMS ${pl.rms.toFixed(4)})`);
  ok(btn[0] === "■" && btn[1] === "true", "▶ became ■ while it plays");
  const sel0 = await p.evaluate(() => document.querySelectorAll("#org-llist .orow.sel").length);
  await p.click('#org-llist .orow[data-i="9"] .wsplay');
  await p.waitForTimeout(700);
  const pl2 = await grab();
  const btn2 = await p.evaluate(() => document.querySelector('#org-llist .orow[data-i="9"] .wsplay').textContent);
  ok(btn2 === "▶" && pl2.rms < pl.rms * 0.5, `■ stops it (RMS ${pl.rms.toFixed(4)} -> ${pl2.rms.toFixed(4)})`);
  ok((await p.evaluate(() => document.querySelectorAll("#org-llist .orow.sel").length)) === sel0, "the button's click is its own (the row's choice unchanged)");
  // off: the editor dimmed again
  await p.evaluate(() => window.infekt.showTab("editor"));
  await p.click("#ws-on");
  ok((await p.getAttribute("#e-body", "class")) === "ed-off" && !(await p.evaluate(() => window.infekt.WS.on)), "off again");
  // the engine's speed on this browser's main thread (the worklet runs the same code)
  const perf = await p.evaluate(async () => {
    const b64 = document.getElementById("wsdata").textContent.trim();
    const bin = Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
    const buf = await new Response(new Blob([bin]).stream().pipeThrough(new DecompressionStream("gzip"))).arrayBuffer();
    const X = (await WebAssembly.instantiate(buf, {})).instance.exports;
    X.ws_init(44100);
    const P = new Uint8Array(X.memory.buffer, X.ws_patch(), X.ws_plen());
    const D = JSON.parse(document.getElementById("eddata").textContent);
    P.set(D.init); P.set(D.shx.defs, 256);   // (the init patch, the shimmer's defaults)
    Object.entries({ 40: 90, 97: 3, 98: 60, 99: 100, 105: 80, 112: 4, 113: 100, 116: 110, 169: 2, 170: 60, 228: 3, 229: 60, 213: 50, 256: 100 })
      .forEach(([b, v]) => { P[b] = v; });
    X.ws_load();
    for (let k = 0; k < 16; k++) X.ws_midi(0x90, 40 + 3 * k, 100);
    let v = 0;
    for (let i = 0; i < 50; i++) v = X.ws_render();
    const N = 3000, t0 = performance.now();
    for (let i = 0; i < N; i++) v = X.ws_render();
    const ms = performance.now() - t0;
    return { voices: v, rtf: (N * 128 / 44100 * 1000) / ms, us: ms / N * 1000 };
  });
  // (1.18C47w4) the user wave bank of the module (the page's own waves, make_page.py --waves): a patch on wave 2 renders
  // differently once a record (here: a sine with a strong 7th) is set as wave 2, and as before without one
  const uw = await p.evaluate(async () => {
    const b64 = document.getElementById("wsdata").textContent.trim();
    const bin = Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
    const buf = await new Response(new Blob([bin]).stream().pipeThrough(new DecompressionStream("gzip"))).arrayBuffer();
    const D = JSON.parse(document.getElementById("eddata").textContent);
    const run = async (rec) => {
      const X = (await WebAssembly.instantiate(buf, {})).instance.exports;
      X.ws_init(44100);
      if (rec) { new Uint8Array(X.memory.buffer, X.ws_uwave_buf(), rec.length).set(rec); X.ws_uwave_set(2); }
      const P = new Uint8Array(X.memory.buffer, X.ws_patch(), X.ws_plen());
      P.set(D.init); P.set(D.shx.defs, 256);
      P[17] = 0; P[19] = 2;              // (osc 1: the wave chosen, wave 2)
      X.ws_load(); X.ws_render(); X.ws_midi(0x90, 60, 100);
      const out = [];
      for (let i = 0; i < 60; i++) { X.ws_render(); out.push(...new Float32Array(X.memory.buffer, X.ws_out_l(), 128)); }
      return out;
    };
    const amp = new Array(64).fill(0), phs = new Array(64).fill(0);
    amp[0] = 1; amp[6] = 0.8;
    const rec = Array.from(WV.record(amp, phs, "TEST UW"));
    const a = await run(null), b = await run(null), c = await run(rec);
    const rms = (x, y) => Math.sqrt(x.reduce((n, v, i) => n + (v - y[i]) ** 2, 0) / x.length);
    return { same: rms(a, b), diff: rms(a, c), level: Math.sqrt(a.reduce((n, v) => n + v * v, 0) / a.length) };
  });
  ok(uw.same === 0 && uw.diff > uw.level * 0.1, `the module's user wave: set, it plays (difference ${uw.diff.toFixed(4)} at level ${uw.level.toFixed(4)}; unset: ${uw.same})`);
  console.log(`websynth perf: heavy patch, ${perf.voices} voices: ${perf.rtf.toFixed(1)}x real time (${perf.us.toFixed(0)} us a block of 2.9 ms)`);
  ok(perf.voices >= 12 && perf.rtf > 2, "the engine fast enough: " + perf.rtf.toFixed(1) + "x");
  ok(!errs.length, "page errors: " + errs.join(" | "));
  await p.close();

  // ---- the file list's ▶: the browser first, no MIDI asked, no device needed ----
  {
    const ctx = await browser.newContext();
    await ctx.addInitScript(() => {
      window.__midiAsks = 0;
      const real = navigator.requestMIDIAccess && navigator.requestMIDIAccess.bind(navigator);
      Object.defineProperty(Navigator.prototype, "requestMIDIAccess", { configurable: true, value: function () {
        window.__midiAsks++; return real ? real() : Promise.reject(new Error("none"));
      } });
      try { localStorage.clear(); } catch (_) { /* */ }
    });
    const q = await ctx.newPage(), qerr = [];
    q.on("pageerror", (e) => qerr.push(String(e)));
    await q.goto(pathToFileURL(synthPage).href + "#patches");
    await q.waitForTimeout(300);
    // a single file made here: the init patch's sound named "TEST 900" (the made-up bank's bytes may be silent)
    const syx = await q.evaluate(() => {
      const D = JSON.parse(document.getElementById("eddata").textContent), d = Uint8Array.from(D.init);
      [..."TEST 900  "].forEach((c, k) => { d[240 + k] = c.charCodeAt(0); });
      return Array.from(BK.buildSingle(d, 0, 0));
    });
    await q.setInputFiles("#b-file", { name: "test900.syx", mimeType: "application/octet-stream", buffer: Buffer.from(syx) });
    await q.waitForSelector("#b-list button.play");
    await q.evaluate(() => { const m = document.getElementById("b-pvmode"); m.value = "chord"; m.dispatchEvent(new Event("change"));
                             const l = document.getElementById("b-pvlen"); l.value = "4"; l.dispatchEvent(new Event("change")); });
    ok(!(await q.isVisible("#ws-dock")), "files: no bar before ▶");
    await q.click("#b-list tbody tr:first-child button.play");
    await q.waitForFunction(() => window.infekt.WS.on && window.infekt.WS.name === "TEST 900", null, { timeout: 15000 }).catch(() => {});
    await q.evaluate(() => { const W = window.infekt.WS, an = W.ctx.createAnalyser(); an.fftSize = 8192; W.node.connect(an); window.__an = an; });
    await q.waitForTimeout(500);
    const rms = await q.evaluate(() => { const d = new Float32Array(window.__an.fftSize); window.__an.getFloatTimeDomainData(d);
                                         let e = 0; for (const x of d) e += x * x; return Math.sqrt(e / d.length); });
    const st = await q.evaluate(() => ({ asks: window.__midiAsks, on: window.infekt.WS.on, name: window.infekt.WS.name,
      dock: !document.getElementById("ws-dock").hidden, kb: !document.getElementById("ws-kbw").hidden,
      btn: document.querySelector("#b-list tbody tr:first-child button.play").textContent, warn: document.getElementById("b-warn").open,
      bar: [document.getElementById("ws-pvmode").value, document.getElementById("ws-pvlen").value] }));
    ok(st.asks === 0, "files ▶: no MIDI access asked (" + st.asks + ")");
    ok(st.on && st.name === "TEST 900" && st.dock && !st.kb && !st.warn, "files ▶: the sound on, TEST 900 loaded, the bar shown (no keyboard, no device dialog) " + JSON.stringify(st));
    ok(rms > 0.01, `files ▶: TEST 900 sounds (RMS ${rms.toFixed(4)})`);
    ok(st.btn === "■", "files ▶: ■ while it plays");
    ok(st.bar[0] === "chord" && st.bar[1] === "4", "the bar's audition settings follow the tab's " + st.bar);
    await q.click("#b-list tbody tr:first-child button.play");
    await q.waitForTimeout(800);
    const rms2 = await q.evaluate(() => { const d = new Float32Array(window.__an.fftSize); window.__an.getFloatTimeDomainData(d);
                                          let e = 0; for (const x of d) e += x * x; return Math.sqrt(e / d.length); });
    const btn2 = await q.textContent("#b-list tbody tr:first-child button.play");
    ok(btn2 === "▶" && rms2 < rms * 0.5, `files ■ stops it (RMS ${rms.toFixed(4)} -> ${rms2.toFixed(4)})`);
    // the bar's setting back to the tab's
    await q.selectOption("#ws-pvmode", "note");
    ok((await q.evaluate(() => document.getElementById("b-pvmode").value)) === "note", "the bar's audition setting sets the tab's");
    // a device connected (a stand-in pair), "Notes to" Browser: the ▶ sends it nothing
    await q.evaluate(() => {
      const D = window.infekt.DEV, sent = (window.__sent = []);
      D.access = D.access || {};
      D.pair = { input: { state: "connected", addEventListener() {}, removeEventListener() {} },
                 output: { state: "connected", send(m) { sent.push(Array.from(m)); } }, name: "TEST DEVICE" };
    });
    ok((await q.evaluate(() => window.infekt.WS.to)) === "b", "Notes to: Browser");
    await q.click("#b-list tbody tr:first-child button.play");
    await q.waitForTimeout(600);
    const dv = await q.evaluate(() => ({ sent: window.__sent.length, asks: window.__midiAsks, name: window.infekt.WS.name }));
    ok(dv.sent === 0 && dv.asks === 0 && dv.name === "TEST 900", "a device connected, Notes to Browser: nothing sent to it " + JSON.stringify(dv));
    ok(!qerr.length, "files: page errors " + qerr.join(" | "));
    await ctx.close();
  }
} catch (e) {
  fails++;
  console.log("FAIL " + (e && e.stack || e));
} finally {
  await browser.close();
}
console.log(fails ? `websynth: ${fails} FAIL` : "websynth all ok");
process.exit(fails ? 1 : 0);
