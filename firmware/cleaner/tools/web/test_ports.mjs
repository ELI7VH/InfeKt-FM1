// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 J0KERPACK
// The InfeKt-FM1 page's port detection under jsdom: a port is the FM-1 only once it answered the FM-1's own handshake
// (the identity request, the patch, wave or editor hello), never by its name. Simulated: MIDI keyboards that never answer
// (one echoing everything back, as a MIDI thru does; one whose name holds "FM1"; one with the FM-1's generic name) beside
// the real (answering) FM-1. Checked in every tab (Patches: an upload and a preview; Wavetables: a wave; Editor: a
// set; Installation: the crash report) that nothing but a handshake reaches a keyboard and the page
// says which port it sends to; with only the keyboards: "Kein FM-1 gefunden", nothing sent; a port chosen by hand that
// does not answer: a warning, nothing sent; the FM-1 unplugged and back: asked again before anything is sent.
// jsdom from the working directory's node_modules (npm install jsdom):
//   node test_ports.mjs PAGE.html     (PAGE: make_page.py's output)
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { webcrypto } from "node:crypto";
const { JSDOM, VirtualConsole } = createRequire(process.cwd() + "/")("jsdom");

const [html] = process.argv.slice(2);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let bad = 0;
const check = (what, ok) => { console.log((ok ? "ok   " : "FAIL ") + what); if (!ok) bad++; };
async function until(f, ms, step = 30) { const end = Date.now() + ms; while (Date.now() < end) { if (f()) return true; await sleep(step); } return !!f(); }

// ---- the MIDI system: devices come and go, each an input and an output (ids unique, names not) ----
const access = { inputs: new Map(), outputs: new Map(), onstatechange: null, sysexEnabled: true };
let nid = 0;
class Gear {
  constructor(name, { echo = false } = {}) {
    this.name = name; this.echo = echo; this.sent = []; const id = ++nid;
    this.input = { id: "in" + id, name, state: "disconnected", connection: "closed", onmidimessage: null, open: async () => {}, close: async () => {} };
    this.output = { id: "out" + id, name, state: "disconnected", connection: "closed", open: async () => {}, close: async () => {},
                    send: (m) => { if (this.output.state !== "connected") throw new Error("gone"); this.sent.push(Array.from(m)); this.rx(Array.from(m)); } };
  }
  plug() {
    access.inputs.set(this.input.id, this.input); access.outputs.set(this.output.id, this.output);
    this.input.state = this.output.state = "connected";
    if (access.onstatechange) access.onstatechange({ port: this.input });
    return this;
  }
  unplug() {
    this.input.state = this.output.state = "disconnected";
    access.inputs.delete(this.input.id); access.outputs.delete(this.output.id);
    if (access.onstatechange) access.onstatechange({ port: this.input });
  }
  tx(bytes) { setTimeout(() => this.input.state === "connected" && this.input.onmidimessage && this.input.onmidimessage({ data: Uint8Array.from(bytes) }), 2); }
  rx(m) { if (this.echo) this.tx(m); }     // (a keyboard: nothing, or its MIDI thru)
}

const vc = new VirtualConsole();
vc.on("jsdomError", (e) => { if (!/Not implemented/.test(e.message)) console.log("jsdom:", e.message); });
const dom = new JSDOM(readFileSync(html, "utf8"), { runScripts: "dangerously", pretendToBeVisual: true, url: "http://localhost/infekt-fm1.html#patches",
  virtualConsole: vc,
  beforeParse(w) {
    w.navigator.requestMIDIAccess = async () => access;
    Object.defineProperty(w, "crypto", { value: webcrypto });
    w.TextEncoder = TextEncoder;
    w.HTMLDialogElement.prototype.showModal = function () { this.setAttribute("open", ""); };
    w.HTMLDialogElement.prototype.close = function (v) {
      if (v !== undefined) this.returnValue = v;
      this.removeAttribute("open"); this.dispatchEvent(new w.Event("close"));
    };
    w.Element.prototype.scrollIntoView = function () {};
    w.URL.createObjectURL = () => "blob:x"; w.URL.revokeObjectURL = () => {};
    w.confirm = () => true;
  } });
const w = dom.window, d = w.document, $ = (id) => d.getElementById(id);
const FW = w.eval("FW"), BK = w.eval("BK"), WV = w.eval("WV"), RE = w.eval("RE"), ED = JSON.parse($("eddata").textContent);
const LEN = 256 + ED.shx.defs.length;

// ---- the FM-1: its identity, the patch upload, the wave hello and write, the editor's hello, dump and set ----
const idFrame = (txt) => { const t = [...txt].map((c) => c.charCodeAt(0)); return [0xF0, ...FW.pack7([0, 0x59, 0x11, 0, 0, 0, ...t, ...new Array(28 - t.length).fill(0)]), 0xF7]; };
class FM1 extends Gear {
  constructor(name, ident = "FM-1_13010200") {
    super(name); this.ident = ident; this.plan = []; this.armed = 0; this.saved = 0;
    this.buf = new Uint8Array(LEN); this.buf.set(ED.init); this.buf.set(ED.shx.defs, 256);
  }
  request(addr, len) {                   // the update protocol: a read request
    this.waiting = addr;
    const u = [0, 0x59, 0x30, 8, 0, 0, 0, addr & 0xFF, (addr >>> 8) & 0xFF, (addr >>> 16) & 0xFF, (addr >>> 24) & 0xFF, len & 0xFF, len >> 8, 0];
    let s = 0;
    for (let i = 6; i < 14; i++) s += u[i];
    this.tx([0xF0, ...FW.pack7([...u, ~s & 0xFF]), 0xF7]);
  }
  rx(m) {
    const ota = /^ota-/.test(this.ident);
    if (m.join() === FW.HS_QUERY.join()) {
      this.tx(idFrame(this.ident));
    } else if (m.join() === FW.UPGRADE.join()) {
      this.queue = ota ? [[0, 512], [512, 512], [0xF0000000, 8]] : [[0, 512], [0xE0000000, 8]];
      this.request(...this.queue.shift());
    } else if (this.waiting != null && m[0] === 0xF0 && m.length > 20 && m[1] !== 0x7D) {
      const addr = this.waiting;
      this.waiting = null;
      if (addr >= 0xE0000000) {          // restarts: into its loader, or the new firmware (the same port here)
        const next = this.plan.shift();
        setTimeout(() => this.unplug(), 20);
        if (next) setTimeout(() => { this.ident = next; this.plug(); }, 1200);
        return;
      }
      this.request(...this.queue.shift());
    } else if (ota) {
      return;
    } else if (m[0] === 0xF0 && m[1] === 0x7D && m[2] === 0x42 && m[3] === 0x55) {
      const c = m[4];
      if (c === 1) this.tx([0xF0, 0x7D, 0x42, 0x55, 0x41, 1, 0xF7]);
      else if (c === 2) { this.armed = 1; this.saved = 0; this.tx([0xF0, 0x7D, 0x42, 0x55, 0x42, m[5], 0, 0xF7]); }
      else if (c === 3 || c === 4) { this.armed = 0; this.tx([0xF0, 0x7D, 0x42, 0x55, 0x43, 0, this.saved >> 7, this.saved & 127, 0, 0xF7]); }
    } else if (m[0] === 0xF0 && m.length === 267 && m[1] === 0) {
      if (this.armed) this.saved++;
      this.tx([0xF0, 0x7D, 0x42, 0x55, 0x50, m[8], 0, this.armed ? 1 : 0, 0xF7]);
    } else if (m[0] === 0xF0 && m[1] === 0x7D && m[2] === 0x56 && m[3] === 0x46) {
      const H = [0xF0, 0x7D, 0x56, 0x46];
      if (m[4] === 0x01) this.tx([...H, 0x41, 1, 62, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xF7]);
      else if (m[4] === 0x10) {
        const rec = WV.unpack7(Uint8Array.from(m.slice(6, m.length - 4)), WV.W.REC), c = WV.crc16(rec);
        this.tx([...H, 0x50, 0x10, m[5], 0, (c >> 14) & 3, (c >> 7) & 0x7F, c & 0x7F, 0xF7]);
      }
    } else {
      const r = RE.parse(Uint8Array.from(m)), H = (cmd, ...a) => [0xF0, 0x7D, 0x52, 0x45, cmd, ...a, 0xF7];
      if (!r) return;
      const st = [0, 0, 0, 0, ..."A001".split("").map((c) => c.charCodeAt(0))];
      if (r.cmd === 1) this.tx(H(0x41, 1, 2, LEN >> 7, LEN & 127, ...st));
      else if (r.cmd === 2) this.tx(H(0x42, ...st, LEN >> 7, LEN & 127, ...RE.pack(this.buf)));
      else if (r.cmd === 3) {
        for (let k = 2; k + 3 < r.a.length; k += 4) this.buf[(r.a[k] << 7) | r.a[k + 1]] = (r.a[k + 2] << 7) | r.a[k + 3];
        this.tx(H(0x43, r.a[0], 0));
      }
    }
  }
}

// what a port that has not answered may get: the handshakes only
const HELLOS = new Set([FW.HS_QUERY.join(), [...BK.buMsg(1)].join(), [...WV.msgHello()].join(), [...RE.msg(RE.HELLO)].join()]);
const notHello = (g) => g.sent.filter((m) => !HELLOS.has(m.join()));
const lcd = () => [1, 2, 3, 4].map((k) => $("d-l" + k).textContent).join(" | ");
const DEV = w.infekt.DEV, bk = w.infekt.bk, wv = w.infekt.wv, ed = w.infekt.ed;

// ---- the pairs asked, in their order: the usual names first; Windows' in/out names paired; namesakes each way ----
{
  const P = (name, id) => ({ id, name, state: "connected" });
  const acc = (ins, outs) => ({ inputs: new Map(ins.map((p) => [p.id, p])), outputs: new Map(outs.map((p) => [p.id, p])) });
  const usual = (n) => /fm-?1|infekt/i.test(n);
  const a = FW.pairsOf(acc([P("Arturia KeyStep", "i1"), P("MIDIIN2 (InfeKt-FM1)", "i2")], [P("Arturia KeyStep", "o1"), P("MIDIOUT2 (InfeKt-FM1)", "o2")]), usual);
  check("pairs: the usual name first, Windows' MIDIIN2 (x) / MIDIOUT2 (x) paired", a.map((p) => p.input.id + p.output.id).join() === "i2o2,i1o1");
  const b = FW.pairsOf(acc([P("USB-MIDI", "i1"), P("USB-MIDI", "i2")], [P("USB-MIDI", "o1"), P("USB-MIDI", "o2")]));
  check("pairs: two ports of one name, every combination (the answer tells)", b.length === 4);
  const c = FW.pairsOf(acc([P("Keyboard", "i1"), P("Other", "i2")], [P("Synth Out", "o1")]));
  check("pairs: no output of its name: not paired with some other one", c.length === 0);
}

// a few patches to send
const raw = (k) => { const p = new Uint8Array(256).map((_, i) => (i * 3 + k) % 100); [..."PORT " + k + "     "].slice(0, 10).forEach((c, i) => (p[240 + i] = c.charCodeAt(0))); return p; };
const syx = Uint8Array.from([0, 1, 2].flatMap((k) => [...BK.buildSingle(raw(k), k)]));
await bk.addFiles([{ name: "t.syx", arrayBuffer: async () => syx.buffer.slice(0) }]);
const singlesTo = (g) => g.sent.filter((m) => m.length === 267 && m[0] === 0xF0);

// ==== 1. keyboards plugged in before the FM-1 (asked first by the map's order), one named like it ====
const kbd = ["Arturia KeyStep", "Keystation 49 MK3", "MPK mini 3", "USB MIDI 1"].map((n) => new Gear(n).plug());
const ctl = new Gear("FM1 Controller", { echo: true }).plug();          // (its name holds "FM1"; it echoes, as a MIDI thru)
const keys = [...kbd, ctl];
let fm = new FM1("InfeKt-FM1").plug();
await w.infekt.scan();
check(`automatic: the answering port taken (${lcd()})`, DEV.state === "ok" && DEV.pair && DEV.pair.input === fm.input && DEV.pair.output === fm.output);
check("its name in the header's LCD", /Port: InfeKt-FM1/.test(lcd()));
check(`and in the Patches tab (${$("b-port").textContent})`, $("b-port").textContent === "an: InfeKt-FM1");
check("the keyboards got nothing but handshakes", keys.every((g) => !notHello(g).length));

bk.singles.forEach((s) => (s.sel = true)); bk.render();
await bk.upload();
check(`upload: 3 singles to the FM-1 (${$("b-status").textContent})`, singlesTo(fm).length === 3 && /Fertig: 3/.test($("b-status").textContent));
check("not one to a keyboard", keys.every((g) => !singlesTo(g).length && !notHello(g).length));

// the preview: the single and its notes to the FM-1 only
$("b-pvlen").value = "0.2"; $("b-pvwait").value = "0.1";
d.querySelectorAll("#b-list tbody tr:not(.notes)")[1].querySelector("button.play").click();
await sleep(50);
if ($("b-warn").open) $("b-warnyes").click();
await until(() => fm.sent.some((m) => (m[0] & 0xF0) === 0x90), 3000);
check("the preview: its single and notes to the FM-1", singlesTo(fm).length === 4 && fm.sent.some((m) => (m[0] & 0xF0) === 0x90));
await sleep(500);
check("the keyboards: still nothing", keys.every((g) => !notHello(g).length));

// a wave
w.infekt.showTab("waves");
const wav = WV.wavFloat(Array.from({ length: 600 }, (_, k) => 0.7 * (2 * k / 600 - 1)), 48000);
await wv.addFiles([{ name: "W12_saw.wav", arrayBuffer: async () => wav.buffer.slice(0) }]);
$("w-send").click();
await until(() => fm.sent.some((m) => m[1] === 0x7D && m[2] === 0x56 && m[4] === 0x10), 4000);
check("a wave to the FM-1, none to a keyboard", fm.sent.some((m) => m[1] === 0x7D && m[2] === 0x56 && m[4] === 0x10) && keys.every((g) => !notHello(g).length));

// the editor: a set to the FM-1
w.infekt.showTab("editor");
await ed.connect();
await until(() => ed.live, 3000);
ed.setByte(40, 55);
await until(() => fm.buf[40] === 55, 2000);
check("the editor: connected on the FM-1, its set there", ed.live && fm.buf[40] === 55 && keys.every((g) => !notHello(g).length));
w.infekt.showTab("install");
await sleep(100);

// the crash report: the FM-1's identity first; nothing else to a keyboard
$("i-reportgo").click();
await until(() => $("i-reportstatus").textContent && !/Lese/.test($("i-reportstatus").textContent) && !$("i-reportgo").disabled, 15000);
check(`the crash report asked of the FM-1 (${$("i-reportstatus").textContent})`, fm.sent.some((m) => m[1] === 0x7D && m[2] === 0x4C));
await until(() => DEV.state === "ok", 15000);

// ==== 2. a port chosen by hand that does not answer: a warning, nothing sent ====
w.infekt.showTab("patches");
$("d-port").value = "FM1 Controller"; $("d-port").dispatchEvent(new w.Event("change"));
await until(() => DEV.state === "notfound", 15000);
check(`the chosen keyboard: not taken (${lcd()})`, DEV.state === "notfound" && !DEV.pair && /Keine Antwort/.test(lcd()) && /FM1 Controller/.test(lcd()));
let n0 = singlesTo(fm).length;
await bk.upload();
check(`upload refused with a warning (${$("b-status").textContent})`, /gewählte Port antwortet nicht/.test($("b-status").textContent) &&
      $("b-status").classList.contains("err") && singlesTo(fm).length === n0 && keys.every((g) => !notHello(g).length));
check(`the Patches tab says so (${$("b-port").textContent})`, /kein FM-1/.test($("b-port").textContent));
$("d-port").value = ""; $("d-port").dispatchEvent(new w.Event("change"));
await until(() => DEV.state === "ok", 15000);
check("back to automatic: the FM-1 again", DEV.state === "ok" && DEV.pair.input === fm.input);

// ==== 3. the FM-1 unplugged: only the keyboards; nothing goes anywhere ====
fm.unplug();
check(`unplugged: no pair (${lcd()})`, !DEV.pair && DEV.state === "lost");
const t0 = Date.now();
await bk.upload();
check(`only keyboards: "Kein FM-1 gefunden" (${$("b-status").textContent}), ${((Date.now() - t0) / 1000).toFixed(1)} s`,
      DEV.state === "notfound" && /Kein FM-1 gefunden/.test($("b-status").textContent) && /Kein FM-1 gefunden/.test(lcd()));
check("every keyboard asked, none sent anything else", keys.every((g) => g.sent.some((m) => m.join() === FW.HS_QUERY.join()) && !notHello(g).length));
w.infekt.showTab("editor");
await ed.connect();
check("the editor: nothing sent either", !ed.live && keys.every((g) => !notHello(g).length));
w.infekt.showTab("patches");
// a single keyboard alone (the old fallback paired any input with the only output)
for (const g of keys.slice(1)) g.unplug();
await w.infekt.scan();
await bk.upload();
check("one keyboard alone: not taken, nothing sent", DEV.state === "notfound" && !DEV.pair && !notHello(kbd[0]).length);
for (const g of keys.slice(1)) g.plug();

// ==== 4. the FM-1 back under a generic name that a keyboard has too; plugged in after it ====
const twin = new Gear("USB-MIDI").plug();
fm = new FM1("USB-MIDI").plug();
await until(() => DEV.state === "ok", 40000);
check(`back: found again by itself, the very pair that answered (${lcd()})`, DEV.state === "ok" && DEV.pair.input === fm.input && DEV.pair.output === fm.output);
n0 = singlesTo(fm).length;
await bk.upload();
check(`upload to it, none to its namesake (${$("b-status").textContent})`, singlesTo(fm).length === n0 + 3 && !notHello(twin).length &&
      keys.every((g) => !notHello(g).length) && /an: USB-MIDI/.test($("b-port").textContent));

// ==== 5. keyboards that answer the identity request (the maker's USB chip's: "SMK25V2_130"; a universal identity reply;
// a keyboard's own update loader): never the FM-1, never "Verbunden", nothing but the request sent to them ====
class IdGear extends Gear {
  constructor(name, reply) { super(name); this.reply = reply; }
  rx(m) { if (m.join() === FW.HS_QUERY.join()) this.tx(this.reply); }
}
for (const g of [...keys, twin, fm]) if (g.input.state === "connected") g.unplug();
const UNIVERSAL = [0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x47, 0x31, 0x00, 0x19, 0x00, 0x01, 0x00, 0x00, 0x00, 0xF7];
const smk = new IdGear("SINCO SMK25-Private", idFrame("SMK25V2_130")).plug();
const uni = new IdGear("USB-MIDI Keyboard", UNIVERSAL).plug();
const smkup = new IdGear("SINCO Update", idFrame("ota-SMK-37 Pro_900")).plug();
const idk = [smk, uni, smkup];
const seen = [];                         // every state the header's LCD showed
new w.MutationObserver(() => seen.push(lcd())).observe($("d-l1"), { childList: true, characterData: true, subtree: true });
w.infekt.showTab("patches");
await w.infekt.scan();
check(`answering keyboards only: "Kein FM-1 gefunden", the other device named (${lcd()})`,
      DEV.state === "notfound" && !DEV.pair && /Kein FM-1 gefunden/.test(lcd()) && /fremdes Gerät: .* – kein FM-1/.test(lcd()));
await bk.upload();
check(`upload: refused (${$("b-status").textContent})`, /Kein FM-1 gefunden/.test($("b-status").textContent) && idk.every((g) => !notHello(g).length));
w.infekt.showTab("install");
$("i-go").click();
await sleep(50);
if ($("i-confirm").hasAttribute("open")) { $("i-bakok").checked = true; $("i-bakok").dispatchEvent(new w.Event("change")); $("i-confirm").close("go"); }
await until(() => /nicht gefunden/.test($("i-alert").textContent), 40000);
check(`Install with only them: nothing written (${$("i-alert").textContent.slice(0, 40)})`, !DEV.writing && /nicht gefunden/.test($("i-alert").textContent) && idk.every((g) => !notHello(g).length && !g.sent.some((m) => m.join() === FW.UPGRADE.join())));
$("i-reportstatus").textContent = "";
$("i-reportgo").click();
await until(() => /Keine Antwort/.test($("i-reportstatus").textContent), 40000);
check(`crash report: nothing to them (${$("i-reportstatus").textContent})`,
      /Keine Antwort/.test($("i-reportstatus").textContent) && idk.every((g) => !notHello(g).length));
w.infekt.showTab("patches");
$("d-port").value = "SINCO SMK25-Private"; $("d-port").dispatchEvent(new w.Event("change"));
await w.infekt.scan();
check(`the keyboard chosen by hand: "Kein FM-1", named (${lcd()})`, DEV.state === "notfound" && /Kein FM-1/.test(lcd()) && /SMK25V2_130/.test(lcd()));
check("never \"Verbunden\" for one of them", !seen.some((x) => /Verbunden|Update-Modus/.test(x)));

// the FM-1 beside them, chosen by hand: everything goes to it; an install too, across its update loader (the keyboards
// asked first by the map's order, the keyboard's own loader there all along)
fm = new FM1("InfeKt-FM1 MIDI 1", "FM-1_13010200").plug();
$("d-port").value = "InfeKt-FM1 MIDI 1"; $("d-port").dispatchEvent(new w.Event("change"));
await until(() => DEV.state === "ok", 15000);
check(`the FM-1 chosen by hand: connected (${lcd()})`, DEV.state === "ok" && DEV.pair.input === fm.input && /Port: InfeKt-FM1 MIDI 1/.test(lcd()));
n0 = singlesTo(fm).length;
await bk.upload();
check("upload: to the FM-1 only", singlesTo(fm).length === n0 + 3 && idk.every((g) => !notHello(g).length));
const PROD = w.infekt.I.product;
fm.plan = ["ota-FM-1_1", PROD];
w.infekt.showTab("install");
$("i-mboot").checked = false;
$("i-go").click();
await sleep(50);
$("i-bakok").checked = true; $("i-bakok").dispatchEvent(new w.Event("change"));
$("i-confirm").close("go");
await until(() => DEV.writing, 3000);
await until(() => !DEV.writing, 60000, 100);
check(`install: to the chosen FM-1, across its loader (${$("i-status").textContent})`, /^Fertig/.test($("i-status").textContent) && fm.ident === PROD &&
      fm.sent.some((m) => m.join() === FW.UPGRADE.join()) && idk.every((g) => !notHello(g).length && !g.sent.some((m) => m.join() === FW.UPGRADE.join())));
await until(() => DEV.state === "ok", 15000);
check(`back: the FM-1 with the new version (${lcd()})`, DEV.state === "ok" && DEV.pair.input === fm.input && new RegExp(PROD).test(lcd()));
// automatic: the same
$("d-port").value = ""; $("d-port").dispatchEvent(new w.Event("change"));
await sleep(100); await until(() => DEV.state === "ok", 15000);
check(`automatic beside them: the FM-1 (${lcd()})`, DEV.state === "ok" && DEV.pair.input === fm.input && idk.every((g) => !notHello(g).length));
// the stock firmware on it: an FM-1 for the installer only (no patch / wave / editor hello), installed over from it
fm.unplug(); fm.ident = "FM-1_016"; fm.sent.length = 0; fm.plug();
await sleep(100); await w.infekt.scan();
check(`the stock firmware: an FM-1 (${lcd()})`, DEV.state === "ok" && DEV.pair.input === fm.input && /Original|stock/i.test(lcd()));
w.infekt.showTab("patches");
await bk.upload();
check(`patches: refused, only its identity asked (${$("b-status").textContent})`, /läuft nicht InfeKt-FM1/.test($("b-status").textContent) && !notHello(fm).length);
w.infekt.showTab("editor");
await ed.connect();
check("editor: nothing sent", !ed.live && !notHello(fm).length);
w.infekt.showTab("install");
fm.plan = ["ota-FM-1_015", PROD];
$("i-go").click();
await sleep(50);
$("i-bakok").checked = true; $("i-bakok").dispatchEvent(new w.Event("change"));
$("i-confirm").close("go");
await until(() => DEV.writing, 3000);
await until(() => !DEV.writing, 60000, 100);
check(`install over the stock firmware (${$("i-status").textContent})`, /^Fertig/.test($("i-status").textContent) && fm.ident === PROD &&
      idk.every((g) => !notHello(g).length));
check("never \"Verbunden\" with one of them", !seen.some((x, k) => /Verbunden/.test(x) && /SMK25|SINCO|USB-MIDI Keyboard|MIDI ID/.test(x)));

console.log(bad ? bad + " FAILED" : "ports all ok");
process.exit(bad ? 1 : 0);
