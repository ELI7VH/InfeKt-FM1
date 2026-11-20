// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 J0KERPACK
// The InfeKt-FM1 page's Editor tab under jsdom with a simulated FM-1 that speaks the remote edit protocol
// (SysEx F0 7D 52 45) as the firmware does: one SysEx frame taken at a time (a frame that
// comes while the last is still being taken is lost, as fm1-base's USB reader drops it), the undo step's coalescing
// (the same byte within 1.5 s), the edited mark, the chunks and the state told when the panel changes, the dump after a
// patch change, MIDI CC told as the engine's values, the matrix's slots' levels streamed (4B), two parts (the part
// edited switched). Checked: every parameter of the firmware's table on the page, the round trip, the throttle (newest
// values only, never a frame lost, no stale value last), the knobs (drag, keys, double-click), the lists, the matrix's
// lights (in use, and live), the notifications, UNDO, the name, the keepalive and the bye; the layout (six panels with
// sub-tabs, every control in one, the matrix always shown, the sub-tab's choice remembered, the arrow keys). jsdom from the working
// directory's node_modules (npm install jsdom):
//   node test_editor.mjs PAGE.html     (PAGE: make_page.py's output)
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { webcrypto } from "node:crypto";
const { JSDOM, VirtualConsole } = createRequire(process.cwd() + "/")("jsdom");

const [html] = process.argv.slice(2);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let bad = 0;
const check = (what, ok) => { console.log((ok ? "ok   " : "FAIL ") + what); if (!ok) bad++; };
async function until(f, ms, step = 20) { const end = Date.now() + ms; while (Date.now() < end) { if (f()) return true; await sleep(step); } return !!f(); }

const vc = new VirtualConsole();
vc.on("jsdomError", (e) => { if (!/Not implemented/.test(e.message)) console.log("jsdom:", e.message); });
let dev = null;
const dom = new JSDOM(readFileSync(html, "utf8"), { runScripts: "dangerously", pretendToBeVisual: true, url: "http://localhost/infekt-fm1.html#editor",
  virtualConsole: vc,
  beforeParse(w) {
    w.navigator.requestMIDIAccess = async () => dev.access;
    Object.defineProperty(w, "crypto", { value: webcrypto });
    w.TextEncoder = TextEncoder; w.TextDecoder = TextDecoder;
    w.HTMLDialogElement.prototype.showModal = function () { this.setAttribute("open", ""); };
    w.HTMLDialogElement.prototype.close = function () { this.removeAttribute("open"); };
    w.Element.prototype.scrollIntoView = function () {};
  } });
const w = dom.window, d = w.document, $ = (id) => d.getElementById(id);
const FW = w.eval("FW"), RE = w.eval("RE"), ED = JSON.parse($("eddata").textContent);
const LEN = 256 + ED.shx.defs.length;

// ---- the simulated FM-1: its identity, and the remote editor as remote.c has it ----
class FakeFM1 {
  constructor({ multi = false, shim = true } = {}) {
    this.sent = []; this.out = []; this.dropped = 0; this.busy = false; this.multi = multi; this.shim = shim;
    this.input = { name: "InfeKt-FM1", state: "connected", connection: "closed", onmidimessage: null, open: async () => {}, close: async () => {} };
    this.output = { name: "InfeKt-FM1", state: "connected", connection: "closed", open: async () => {}, close: async () => {}, send: (m) => this.rx(Array.from(m)) };
    this.access = { inputs: new Map([["i", this.input]]), outputs: new Map([["o", this.output]]), onstatechange: null, sysexEnabled: true };
    const init = () => { const b = new Uint8Array(LEN); b.set(ED.init); b.set(ED.shx.defs, 256); return b; };
    this.buf = [init(), init()]; this.part = 0; this.mxOn = multi; this.patch = [0, 768]; this.edited = [0, 0];
    this.undo = []; this.undoT = 0; this.alive = 0; this.cc = null; this.procMs = [2, 9];
    this.buf[1].set([..."PART B    "].map((c) => c.charCodeAt(0)), 240);
    this.buf[0].set([..."ACID WARS "].map((c) => c.charCodeAt(0)), 240);
    this.tags = new Map([[0, "A001"], [1, "A002"], [768, "INIT"]]);
    this.sets = new Array(16).fill(null); this.running = false; this.setWrites = 0;
  }
  get b() { return this.buf[this.part]; }
  tx(bytes) { this.out.push({ t: Date.now(), m: bytes }); setTimeout(() => this.input.onmidimessage && this.input.onmidimessage({ data: Uint8Array.from(bytes) }), 1); }
  stat() {
    const p = this.patch[this.part];
    return [this.part, (this.edited[this.part] ? 1 : 0) | (this.multi && this.mxOn ? 4 : 0), p >> 7, p & 127, ...[...this.tags.get(p)].map((c) => c.charCodeAt(0))];
  }
  H(cmd, ...a) { return [0xF0, 0x7D, 0x52, 0x45, cmd, ...a, 0xF7]; }
  dump() { this.tx(this.H(0x42, ...this.stat(), LEN >> 7, LEN & 127, ...RE.pack(this.b))); }
  rx(m) {
    this.sent.push({ t: Date.now(), m });
    if (m[0] !== 0xF0) { if ((m[0] & 0xF0) === 0xB0 && this.ccEcho) this.ccEcho(m); return; }
    if (this.busy) { this.dropped++; return; }           // (fm1-base: one frame at a time)
    this.busy = true;
    const [lo, hi] = this.procMs;
    setTimeout(() => { this.busy = false; this.take(m); }, lo + Math.random() * (hi - lo));
  }
  take(m) {
    const j = m.join();
    if (j === FW.HS_QUERY.join()) {
      const t = [..."FM-1_13009800"].map((c) => c.charCodeAt(0));
      this.tx([0xF0, ...FW.pack7([0, 0x59, 0x11, 0, 0, 0, ...t, ...new Array(28 - t.length).fill(0)]), 0xF7]);
      return;
    }
    const r = RE.parse(Uint8Array.from(m));
    if (!r) return;
    const a = r.a;
    this.alive = Date.now();
    if (r.cmd === 1) this.tx(this.H(0x41, 1, (this.multi ? 9 : 0) | (this.shim ? 2 : 0), LEN >> 7, LEN & 127, ...this.stat()));
    else if (r.cmd >= 7 && r.cmd <= 9 && this.multi) {   // (1.18C27) the launcher's sets (remote.c re_sets)
      const k = a[0], rec = this.sets[k];
      if (r.cmd === 7) {
        if (k === 127) this.tx(this.H(0x47, 127, 0, 16, 1, 1, 64));
        else if (k >= 16) this.tx(this.H(0x47, k, 2));
        else if (!rec) this.tx(this.H(0x47, k, 1));
        else this.tx(this.H(0x47, k, 0, 1, 64, ...RE.pack(rec), ...[..."A001A002"].map((c) => c.charCodeAt(0))));
        return;
      }
      let st = k >= 16 ? 2 : this.running ? 3 : 0;
      if (!st && r.cmd === 8) {
        const b = a.length === 1 + 220 ? RE.unpack(a, 1, 192) : null;
        if (!b || b[0] !== 0x4C || b[1] !== 0x53 || b[2] !== 1) st = 1; else { this.sets[k] = b; this.setWrites++; }
      } else if (!st) this.sets[k] = null;
      this.tx(this.H(r.cmd === 8 ? 0x4C : 0x4D, k, st));
    }
    else if (r.cmd === 2) this.dump();
    else if (r.cmd === 4) this.alive = 0;
    else if (r.cmd === 3) {
      let s = 0;
      if (a[1] !== this.part) s = 1;
      else for (let k = 2; k + 3 < a.length; k += 4) {
        const b = (a[k] << 7) | a[k + 1], v = (a[k + 2] << 7) | a[k + 3];
        if (b >= LEN || (v > 127 && !(ED.shx.rb.includes(b) && v >= 200 && v < 202))) { s = 2; continue; }
        this.edit(b, v);
      }
      this.tx(this.H(0x43, a[0], s));
    } else if (r.cmd === 5) {
      if (!this.multi || (a[0] && !this.mxOn)) { this.tx(this.H(0x45, ...this.stat())); return; }
      this.part = a[0] & 1; this.undo = [];
      this.tx(this.H(0x45, ...this.stat()));
      setTimeout(() => this.dump(), 40);
    } else if (r.cmd === 6) {
      const u = this.undo.pop();
      if (u) this.b[u.b] = u.old;
      this.tx(this.H(0x46, ...this.stat()));
      if (u) this.chunk(u.b);
    }
  }
  edit(b, v) {                            // edit_set: the undo step coalesced, the edited mark
    const now = Date.now(), last = this.undo[this.undo.length - 1];
    if (!last || last.b !== b || now - this.undoT > 1500) this.undo.push({ b, old: this.b[b] });
    this.undoT = now;
    this.b[b] = v;
    if (!this.edited[this.part]) { this.edited[this.part] = 1; if (this.on()) setTimeout(() => this.tx(this.H(0x48, ...this.stat())), 40); }
  }
  on() { return Date.now() - this.alive < 5000 && this.alive; }
  chunk(b) {
    if (!this.on()) return;
    const k = (b / 32) | 0, n = Math.min(32, LEN - k * 32);
    setTimeout(() => this.tx(this.H(0x49, this.part, k, ...RE.pack(this.b.slice(k * 32, k * 32 + n)))), 40);
  }
  panel(b, v) { this.edit(b, v); this.chunk(b); }           // a knob turned on the FM-1
  loadPatch(p) {                          // PRESET: a patch with other bytes
    this.patch[this.part] = p; this.edited[this.part] = 0; this.undo = [];
    for (let k = 0; k < 240; k++) this.b[k] = (k * 7 + p * 13) % 128;
    this.b.set([..."PATCH TWO "].map((c) => c.charCodeAt(0)), 240);
    if (this.on()) setTimeout(() => this.dump(), 40);
  }
  streamLv() {                            // re_lv: the slots' levels, at most every 40 ms, only when they changed
    this.lv = this.lv || new Array(9).fill(0);
    let last = "";
    this.lvT = setInterval(() => {
      const j = this.lv.join();
      if (this.on() && j !== last) { last = j; this.tx(this.H(0x4B, this.part, ...this.lv)); }
    }, 40);
  }
  midiCC(b, v) { if (this.on()) setTimeout(() => this.tx(this.H(0x4A, this.part, b, v)), 40); }
  frames(cmd, since = 0) { return this.sent.filter((x) => x.t >= since && x.m[0] === 0xF0 && x.m[1] === 0x7D && x.m[2] === 0x52 && x.m[4] === cmd); }
}
dev = new FakeFM1();
const E = w.infekt.ed;
const same = () => { for (let k = 0; k < LEN; k++) if (E.buf[k] !== dev.b[k]) return k; return -1; };
const knobOf = (b) => (E.ctl.get(b) || []).find((c) => c.knob);
const lcd = () => [$("e-l1").textContent, $("e-l1r").textContent, $("e-l2").textContent, $("e-l2r").textContent, $("e-l3").textContent].join(" | ");

// ---- the page as it opens on #editor: the table, every parameter, nothing sent ----
await sleep(200);
check("the Editor tab shown (#editor), the page wide", !$("panel-editor").hidden && d.body.classList.contains("wide"));
const rows = ED.rows.filter((r) => r.f !== "noshim");
const bytes = new Set(rows.flatMap((r) => r.p.flatMap((p) => (p.k === "WPW" ? [p.b, p.b - 1] : [p.k.startsWith("SX") ? 256 + p.b : p.b]))));
const missing = [...bytes].filter((b) => !E.ctl.has(b));
check(`every parameter of the firmware's table on the page (${bytes.size} bytes, ${rows.length} rows; the matrix's ${E.mx.length} slots)${missing.length ? " missing " + missing : ""}`,
      !missing.length && E.mx.length === 9 && d.querySelectorAll("#e-sections .ctl").length >= 150);
check("the hidden EDIT groups' options there too (EQ, phaser stages, LFO 1 > trigger, key base, shimmer swell)",
      [222, 212, 73, 161, 263].every((b) => E.ctl.has(b)));
// ---- the layout: six panels with sub-tabs (every control in one), the matrix beside them always shown ----
const pans = [...d.querySelectorAll("#e-sections > .edpan")];
const allCtl = [...d.querySelectorAll("#e-sections .ctl")];
check(`six panels (${pans.map((p) => p.id.slice(4)).join(" ")}), each a tab list with one sub-tab shown; every control in a sub-tab`,
      pans.length === 6 && pans.every((p) => p.querySelector("[role=tablist]") && p.querySelectorAll(".subpanel:not([hidden])").length === 1) &&
      allCtl.every((c) => c.closest(".subpanel[role=tabpanel]")));
check("every row of the table named by the layout (no fallback MORE sub-tab)", !d.querySelector('.subtab[data-t="s_more"]'));
const subs = [...d.querySelectorAll(".subtab")].map((b) => b.textContent);
check(`the sub-tabs: ${subs.join(" ")}`, ["OSC 1", "OSC 2", "OSC 3", "FM/SYNC", "MIX", "UNISON", "VOICE", "VELO", "FILTER", "ENV", "SLOPES",
      "LFO 1", "LFO 3", "DLY/REV", "SHIMMER", "CHORUS", "PHASER", "DIST", "EQ", "IN/OUT", "ARP", "GROOVE"].every((x) => subs.includes(x)));
const tabSel = (id) => $(id).getAttribute("aria-selected") === "true" && !$(id.replace("e-t-", "e-tp-")).hidden;
$("e-t-osc-1").click();
check("a sub-tab clicked: OSC 2 shown, OSC 1 hidden, its choice remembered", tabSel("e-t-osc-1") && !tabSel("e-t-osc-0") &&
      $("e-tp-osc-0").hidden && w.localStorage.getItem("infekt.edsub.osc") === "OSC 2" && $("e-t-osc-1").tabIndex === 0 && $("e-t-osc-0").tabIndex === -1);
$("e-t-osc-1").dispatchEvent(new w.KeyboardEvent("keydown", { key: "ArrowRight", bubbles: true }));
check("ArrowRight: OSC 3, focused", tabSel("e-t-osc-2") && d.activeElement === $("e-t-osc-2"));
$("e-t-osc-2").dispatchEvent(new w.KeyboardEvent("keydown", { key: "Home", bubbles: true }));
check("Home: OSC 1", tabSel("e-t-osc-0"));
$("e-t-fx-5").click(); $("e-t-lfo-2").click();
const mxShown = () => E.mx.length === 9 && E.mx.every((m) => !m.el.closest("[hidden]") && !m.el.closest(".subpanel"));
check("the matrix outside the sub-tabs, all nine slots shown whatever sub-tab", mxShown());
check("the controls of a hidden sub-tab still there (EQ's MID Q while LFO 3 is shown)", E.ctl.get(222)[0].el.closest(".subpanel").hidden === false &&
      E.ctl.get(73)[0].el.closest(".subpanel").hidden === true);
check("no help texts; a short USB-only note with its reason", !d.querySelector('[data-t="e_hint"], [data-t="e_keys"]') &&
      $("e-usb").textContent === "nur USB" && /SysEx/.test($("e-usb").title));
$("e-t-fx-0").click(); $("e-t-lfo-0").click();
check("nothing sent before Connect", dev.sent.length === 0 && /Nicht verbunden/.test(lcd()) && $("e-body").classList.contains("ed-off"));

// ---- connect: the identity, hello, the dump ----
$("e-connect").click();
await until(() => E.live, 6000);
check(`connected: ${lcd()}`, E.live && same() < 0 && /A001 ACID WARS/.test(lcd()) && !$("e-body").classList.contains("ed-off"));
check("hello then the dump, each after the answer to the last", dev.frames(1).length === 1 && dev.frames(2).length === 1 && dev.dropped === 0);
check("one part: no part buttons", $("e-pb").hidden);
check(`the LCD's third line empty without MIDI CC (no saving hint): "${$("e-l3").textContent}"`, $("e-l3").textContent.trim() === "" && !/SAVE/.test(lcd()));
check("the name field shows the patch's name", $("e-name").value === "ACID WARS");

// ---- a knob dragged fast: the newest values only, one frame on its way at a time, one undo step ----
const cut = knobOf(42), t0 = Date.now();
cut.knob.dispatchEvent(new w.MouseEvent("pointerdown", { clientY: 300, bubbles: true }));
for (let y = 299; y >= 200; y--) { cut.knob.dispatchEvent(new w.MouseEvent("pointermove", { clientY: y, bubbles: true })); if (y % 4 === 0) await sleep(1); }
cut.knob.dispatchEvent(new w.MouseEvent("pointerup", { bubbles: true }));
await until(() => !E.pending.size && dev.b[42] === E.buf[42], 3000);
await sleep(100);
const sets = dev.frames(3, t0), dt = Date.now() - t0;
check(`drag: ${E.buf[42]} on the page and the FM-1, ${sets.length} set frames in ${dt} ms (at most one each 15 ms), none lost`,
      dev.b[42] === E.buf[42] && E.buf[42] === ED.init[42] + 63 && sets.length >= 2 && sets.length <= dt / 15 + 2 && dev.dropped === 0);
check("the strip names it: " + lcd(), /Resonance/.test(lcd()) && /EDITED|BEARBEITET/.test(lcd()) && cut.el.classList.contains("hot"));
check("one undo step on the FM-1 for the whole drag", dev.undo.filter((u) => u.b === 42).length === 1);
const gaps = sets.slice(1).map((x, k) => x.t - sets[k].t);
check(`the frames spaced (smallest gap ${Math.min(...gaps)} ms)`, gaps.every((g) => g >= 14));

// ---- the keys, a double-click, a list, a switch, a pair ----
cut.knob.dispatchEvent(new w.KeyboardEvent("keydown", { key: "ArrowDown", bubbles: true }));
cut.knob.dispatchEvent(new w.KeyboardEvent("keydown", { key: "PageDown", bubbles: true }));
await until(() => dev.b[42] === E.buf[42] && !E.pending.size, 2000);
check(`keys: ArrowDown, PageDown: ${E.buf[42]}`, dev.b[42] === ED.init[42] + 63 - 11);
cut.knob.dispatchEvent(new w.MouseEvent("dblclick", { bubbles: true }));
await until(() => dev.b[42] === ED.init[42], 2000);
check("double-click: the init patch's value", dev.b[42] === ED.init[42] && cut.knob.getAttribute("aria-valuenow") === String(ED.init[42]));
const fm = E.ctl.get(51)[0].sel;
fm.value = "5"; fm.dispatchEvent(new w.Event("change"));
const sync = E.ctl.get(28)[0];
sync.btn.click();
const pol = E.ctl.get(158)[0];
pol.btns[0].click();
await until(() => dev.b[51] === 5 && dev.b[28] === 1 && dev.b[158] === 0, 2000);
check("a list (filter 1 mode ANALOG 2), a switch (sync ON), a pair (env polarity NEG)", dev.b[51] === 5 && dev.b[28] === 1 &&
      sync.btn.getAttribute("aria-pressed") === "true" && pol.btns[0].getAttribute("aria-pressed") === "true");
check("the delay / reverb bytes follow the mode: TIME in a delay mode", E.ctl.get(114)[0].lbEl.textContent === "TIME");
const mode = E.ctl.get(112)[0].sel;
mode.value = "2"; mode.dispatchEvent(new w.Event("change"));
await until(() => dev.b[112] === 2, 2000);
check("REVERB 1: 114 is the pre-delay, 116 the decay, 117 the room (1-4)", E.ctl.get(114)[0].lbEl.textContent === "PREDLY" &&
      E.ctl.get(116)[0].lbEl.textContent === "DECAY" && E.ctl.get(117)[0].lbEl.textContent === "ROOM");

// ---- the matrix: every slot, its light ----
const s1 = E.mx[0];
check("slot 1 dark as the init patch has it", !s1.el.classList.contains("on"));
const src = s1.el.querySelector("select.src"), dst = s1.el.querySelector("select.dst");
src.value = "21"; src.dispatchEvent(new w.Event("change"));
dst.value = "24"; dst.dispatchEvent(new w.Event("change"));
const amtK = s1.el.querySelector(".knob");
amtK.dispatchEvent(new w.KeyboardEvent("keydown", { key: "PageUp", bubbles: true }));
await until(() => dev.b[192] === 21 && dev.b[193] === 24 && dev.b[194] === 74, 2000);
check(`slot 1: LFO 1 > CUTOFF 1, +10: lit (${s1.valEl.textContent})`, s1.el.classList.contains("on") && s1.valEl.textContent === "+10");
const sh = [...dst.options].some((o) => o.value === "200" && o.text === "SHIM AMT");
dst.value = "200"; dst.dispatchEvent(new w.Event("change"));
await until(() => dev.b[193] === 200, 2000);
check("a shimmer target (SHIM AMT, 200) offered and taken; the slot still lit", sh && dev.b[193] === 200 && s1.el.classList.contains("on"));
const s2a = E.mx[1], s2b = E.mx[2];
s2a.el.querySelector("select.src").value = "3"; s2a.el.querySelector("select.src").dispatchEvent(new w.Event("change"));
await until(() => dev.b[195] === 3, 2000);
check("slots 2A and 2B share their source (one byte): 2B's list follows", s2b.el.querySelector("select.src").value === "3" &&
      /2B/.test(s2a.el.querySelector(".nm").textContent));

// ---- the slots' lights live: the glow follows the level the FM-1 streams; a slot not in use stays dark ----
const glow = (m) => +(m.dot.style.getPropertyValue("--g") || 0);
dev.streamLv();
dev.lv[0] = 127; dev.lv[6] = 127;
await until(() => glow(s1) > 0.95, 1500);
check(`slot 1 in use, its level 127: bright (${glow(s1).toFixed(2)}); slot 4 not in use: dark (${glow(E.mx[6]).toFixed(2)})`,
      glow(s1) > 0.95 && glow(E.mx[6]) === 0 && !E.mx[6].el.classList.contains("on"));
dev.lv[0] = 32;
await until(() => Math.abs(glow(s1) - 0.5) < 0.02, 1500);
check(`level 32: half the glow (${glow(s1).toFixed(2)}), eased frame by frame`, Math.abs(glow(s1) - 0.5) < 0.02 && E.lvFrames >= 2);
dev.lv[0] = 0;
await until(() => glow(s1) === 0, 1500);
check("level 0: the light at its in-use floor (no glow)", glow(s1) === 0 && s1.el.classList.contains("on"));

// ---- the FM-1 tells: a panel edit, a patch change, MIDI CC ----
let n0 = dev.out.length;
dev.panel(43, 99);
await until(() => E.buf[43] === 99, 1000);
check(`a knob on the FM-1: the page follows (${lcd()}; ${knobOf(43).knob.getAttribute("aria-valuenow")})`, E.buf[43] === 99 && /Resonance 2/.test(lcd()) && knobOf(43).knob.getAttribute("aria-valuenow") === "99");
check("the sub-tab holding the byte the FM-1 changed marked (FILTER)", $("e-t-filter-0").classList.contains("hot") && !$("e-t-filter-1").classList.contains("hot"));
dev.loadPatch(1);
await until(() => /A002/.test(lcd()), 1000);
check(`a patch change: the whole buffer (${lcd()})`, same() < 0 && /A002 PATCH TWO/.test(lcd()) && !/BEARBEITET/.test(lcd()) && $("e-name").value === "PATCH TWO");
dev.midiCC(44, 111);
await until(() => /MIDI CC/.test(lcd()), 1000);
check(`MIDI CC: the engine's value on the LCD, not in the buffer (${$("e-l3").textContent})`, /Filt Env Amt 111/.test(lcd()) && E.buf[44] !== 111);

// ---- UNDO, the name ----
knobOf(43).knob.dispatchEvent(new w.KeyboardEvent("keydown", { key: "End", bubbles: true }));
await until(() => dev.b[43] === 127, 2000);
const before = dev.undo.length;
await E.undo();
await until(() => E.buf[43] !== 127, 1500);
check(`UNDO from the page: the FM-1's step undone and told back (${E.buf[43]})`, dev.undo.length === before - 1 && E.buf[43] === dev.b[43] && dev.b[43] !== 127);
$("e-name").value = "My Sound"; $("e-name").dispatchEvent(new w.Event("input"));
await until(() => String.fromCharCode(...dev.b.slice(240, 250)) === "My Sound  ", 2000);
check("the name: bytes 240..249", String.fromCharCode(...dev.b.slice(240, 250)) === "My Sound  ");

// ---- many bytes at once (a burst over 24 bytes, each changed several times): the newest of each, nothing lost ----
const t1 = Date.now(), want = new Map();
for (let rep = 0; rep < 5; rep++) for (let b = 54; b < 78; b++) { const v = (b * 3 + rep * 11) % 128; E.setByte(b, v); want.set(b, E.buf[b]); }
await until(() => !E.pending.size && [...want].every(([b, v]) => dev.b[b] === v), 4000);
await sleep(50);
check(`a burst of 120 changes on 24 bytes: ${dev.frames(3, t1).length} frames, the newest values, none lost`,
      [...want].every(([b, v]) => dev.b[b] === v) && dev.frames(3, t1).length <= 4 && dev.dropped === 0);
check(`stats: ${JSON.stringify(E.stats)}`, E.stats.sets > 0);

// ---- an echo of a value the page sent later: the page keeps its own ----
E.setByte(45, 10);
await sleep(5);
E.setByte(45, 11);
dev.tx(dev.H(0x49, 0, 1, ...RE.pack(Uint8Array.from({ length: 32 }, (_, k) => (k === 13 ? 10 : dev.b[32 + k])))));
await sleep(80);
check("a late echo (10) does not undo the newer value (11)", E.buf[45] === 11);
await until(() => dev.b[45] === 11, 1000);

// ---- the keepalive while the tab is shown; leaving it: bye, nothing more ----
const h0 = dev.frames(1).length;
await sleep(3300);
check(`the keepalive: ${dev.frames(1).length - h0} hellos in 3.3 s`, dev.frames(1).length - h0 >= 2);
w.infekt.showTab("patches");
await sleep(50);
check("the tab left: bye, the link closed", dev.frames(4).length === 1 && !E.live);
const n1 = dev.sent.length;
await sleep(1700);
check("then nothing more sent (and the FM-1 streams no levels)", dev.sent.length === n1 && !dev.on());
w.infekt.showTab("editor");
await until(() => E.live, 3000);
check("back on the tab: connected again by itself (hello, dump)", E.live && same() < 0);

// ---- the editor's own port list: the header's ports, the same choice (remembered as "fm1-ports"); a change reconnects ----
const other = { name: "IAC Bus 1", state: "connected", connection: "closed", onmidimessage: null, open: async () => {}, close: async () => {}, send: () => {} };
dev.access.inputs.set("xi", other); dev.access.outputs.set("xo", other);
await w.infekt.scan();
await E.connect();
const eopts = () => [...$("e-port").options].map((o) => o.value);
check(`the port list in the editor's head beside Connect: ${[...$("e-port").options].map((o) => o.text).join(", ")}`,
      $("e-port").closest(".edctl") && $("e-port").nextElementSibling === $("e-connect") && eopts()[0] === "" &&
      eopts().includes("InfeKt-FM1") && eopts().includes("IAC Bus 1") && eopts().join() === [...$("d-port").options].map((o) => o.value).join() &&
      $("e-port").value === "" && !$("e-usb").hidden);
const h1 = dev.frames(1).length, b1 = dev.frames(4).length;
$("e-port").value = "InfeKt-FM1"; $("e-port").dispatchEvent(new w.Event("change"));
await until(() => E.live && dev.frames(1).length > h1, 10000);
check("a port chosen in the editor: bye on the old link, reconnected on it (hello, dump), the header's list the same",
      E.live && dev.frames(4).length === b1 + 1 && dev.frames(1).length > h1 && w.infekt.DEV.manual === "InfeKt-FM1" &&
      w.infekt.DEV.port === "InfeKt-FM1" && $("d-port").value === "InfeKt-FM1" && same() < 0);
check("the choice remembered as the other tabs' picker has it (fm1-ports)", JSON.parse(w.localStorage.getItem("fm1-ports") || "[]").includes("InfeKt-FM1"));
$("e-port").value = "IAC Bus 1"; $("e-port").dispatchEvent(new w.Event("change"));
await until(() => !E.live && $("e-status").textContent && w.infekt.DEV.state === "notfound", 10000);
check(`a port without an FM-1: not connected, said (${$("e-status").textContent})`, !E.live && w.infekt.DEV.state === "notfound" &&
      $("e-port").value === "IAC Bus 1" && $("e-status").textContent.length > 0);
$("e-port").value = ""; $("e-port").dispatchEvent(new w.Event("change"));
await until(() => E.live, 10000);
check("back to automatic: found and connected again", E.live && w.infekt.DEV.manual === "" && $("e-port").value === "" && same() < 0);
dev.access.inputs.delete("xi"); dev.access.outputs.delete("xo");

// ---- two parts: PART B, its own bytes; a set goes to it ----
E.close();
dev.multi = true; dev.mxOn = true;
await E.connect();
check("two parts: the part buttons", !$("e-pb").hidden && !$("e-pb").disabled && $("e-pa").getAttribute("aria-pressed") === "true");
$("e-pb").click();
await until(() => E.stat.part === 1 && /PART B/.test(lcd()), 2000);
check(`PART B: ${lcd()}`, E.stat.part === 1 && dev.part === 1 && same() < 0 && /^B: INIT PART B/.test($("e-l1").textContent));
E.setByte(40, 33);
await until(() => dev.buf[1][40] === 33, 1000);
check("a set goes to part B, part A keeps its byte", dev.buf[1][40] === 33 && dev.buf[0][40] !== 33);
dev.part = 0;                             // (the FM-1 switched back on its panel: the next set is refused, the page re-reads)
E.setByte(41, 77);
await until(() => /Part|part/.test($("e-status").textContent), 1500);
check(`the part changed on the FM-1: refused and noticed (${$("e-status").textContent})`, dev.buf[0][41] !== 77 && dev.buf[1][41] !== 77);

// ---- (1.18C27) the launcher's sets: read when connected (two parts), exported, imported into a slot (asked when filled),
//      refused while the FM-1 runs, a file of another kind refused, all exported and imported ----
const LS = w.infekt.LSET;
const mkRec = (bpm, a, b) => {
  const r = new Uint8Array(192);
  r.set([0x4C, 0x53, 1, 0, 1, 2, 1, 3, 3, 0, 2, 1]);
  r[68] = 122; r[69] = 100; r[70] = 60; r[71] = bpm - 63 + 1; r[72] = 0; r[74] = 1;
  r.set([...a.padEnd(10)].map((c) => c.charCodeAt(0)), 76); r.set([...b.padEnd(10)].map((c) => c.charCodeAt(0)), 86);
  r[96] = 0x4B; r[97] = 1; r[98] = 200;  // (the kit's mark, version; BD's LEVEL: 200 > 127, a byte with its high bit)
  return r;
};
let saved = null;
w.URL.createObjectURL = (blob) => { saved = blob; return "blob:x"; };
w.URL.revokeObjectURL = () => {};
dev.sets[0] = mkRec(150, "ACID WARS", "HearMy 303");
await sleep(600);                        // (the part's re-read above done)
E.close();
await E.connect();
await until(() => E.live, 3000);
check("sets: read by themselves once connected with two parts", await until(() => LS.slots[15] !== null && !LS.busy, 6000));
await LS.read();
await until(() => !$("ls-box").hidden && LS.slots[15] !== null && !LS.busy, 6000);
check(`sets: the box shown and read (${$("ls-status").textContent})`, !$("ls-box").hidden && LS.slots[0] && LS.slots[0].rec
      && LS.slots[1] && !LS.slots[1].rec && /ACID WARS/.test($("ls-list").rows[0].textContent) && /A001/.test($("ls-list").rows[0].textContent));
LS.exportOne(0);
const f1 = LS.lastFile.obj;
check("sets: one exported (the record and its fields)", f1.format === "infekt-fm1-launcher-set" && f1.version === 1 && f1.slot === 1
      && f1.record.length === 384 && f1.bpm === 150 && f1.partA.name === "ACID WARS" && f1.partB.tag === "A002" && f1.rows.length === 2
      && f1.rows[0].mode === "2x" && saved && saved.type === "application/json");
const enc = (o) => new TextEncoder().encode(JSON.stringify(o));
const p1 = LS.parse(enc(f1));
await LS.importTo(1, p1);
await until(() => !LS.busy, 4000);
check("sets: imported into the empty slot 2 (no question), read again", dev.sets[1] && dev.sets[1].join() === dev.sets[0].join()
      && LS.slots[1] && LS.slots[1].rec && dev.setWrites === 1);
let asked = 0;
w.confirm = () => { asked++; return false; };
const p2 = LS.parse(enc({ ...f1, record: Array.from(mkRec(99, "OTHER", "SET"), (x) => x.toString(16).padStart(2, "0")).join("") }));
await LS.importTo(0, p2);
check("sets: a filled slot asked, no: nothing written", asked === 1 && dev.setWrites === 1 && dev.sets[0][71] === 150 - 62);
w.confirm = () => { asked++; return true; };
await LS.importTo(0, p2);
await until(() => !LS.busy, 4000);
check("sets: yes: overwritten", asked === 2 && dev.setWrites === 2 && dev.sets[0][71] === 99 - 62 && LS.slots[0].rec[71] === 99 - 62);
dev.running = true;
await LS.importTo(5, p1);
check(`sets: refused while the FM-1 runs (${$("ls-status").textContent})`, !dev.sets[5] && dev.setWrites === 2 && $("ls-status").classList.contains("err"));
dev.running = false;
check("sets: other files refused", !LS.parse(enc({ format: "x" })).length && !LS.parse(new Uint8Array(192)).length
      && !LS.parse(enc({ ...f1, record: "4c53" })).length && LS.parse(dev.sets[0]).length === 1);
LS.exportAll();
const fa = LS.lastFile.obj;
check("sets: all exported", fa.format === "infekt-fm1-launcher-sets" && fa.sets.length === 2 && fa.sets[1].slot === 2);
dev.sets[0] = dev.sets[1] = null;
await LS.read();
await LS.importAll(LS.parse(enc(fa)));
await until(() => !LS.busy, 4000);
check("sets: all imported into their slots", dev.sets[0] && dev.sets[0][71] === 99 - 62 && dev.sets[1] && dev.sets[1][71] === 150 - 62 && !dev.sets[2]);
check(`no frame lost in the whole run (dropped ${dev.dropped})`, dev.dropped === 0);

// ---- English ----
$("lang").click();
check("English", $("tab-editor").textContent === "Editor" && /Disconnect editor|Connect editor/.test($("e-connect").textContent) &&
      d.querySelector("#e-sections h2").textContent === "Oscillators" && $("e-usb").textContent === "USB only");
$("lang").click();

clearInterval(dev.lvT);
console.log(bad ? bad + " FAILED" : "editor all ok");
process.exit(bad ? 1 : 0);
