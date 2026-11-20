// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 J0KERPACK
// The Installation tab's factory banks and wavetables (make_page.py --factory, --waves) under jsdom with a simulated FM-1: the option absent
// without them; present and ticked with them, its "?" with the thanks in both languages, the backup box's clause while
// ticked, hidden for another firmware file; an install with it ticked: once the new firmware answers, banks A and B
// written by the patch manager's write (02 armed, the 2 x 128 singles in slot order, the extension frame after them,
// 03 the flash write, the names read back); unticked: no bank written; a write that fails: the firmware still counts
// as installed, the button offered, and it works later on its own; the banks as local banks in the patch manager.
// The wavetables the same way (their own option, "?", clause, button): written by the Wavetables tab's write into
// their slots (Wave 03 -> slot 2 ..), before the banks.
// The wait display (WAIT) over the tab: shown through the whole install, the bank write's step and progress in it, a
// closable done state; a refused bank: a closable failure state with the status card's failure text.
// The banks are made up (TEST nnn), made by run_tests.sh. jsdom from the working directory's node_modules:
//   node test_factory.mjs PAGE_FACTORY.html PAGE_NONE.html
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { webcrypto } from "node:crypto";
const { JSDOM, VirtualConsole } = createRequire(process.cwd() + "/")("jsdom");

const [html, htmlNone] = process.argv.slice(2);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let bad = 0;
const check = (what, ok) => { console.log((ok ? "ok   " : "FAIL ") + what); if (!ok) bad++; };
async function until(f, ms, step = 50) { const end = Date.now() + ms; while (Date.now() < end) { if (f()) return true; await sleep(step); } return !!f(); }

// ---- the simulated FM-1: the identity, the update protocol (as test_ui.mjs), the patch handshake of version 4 (as
// test_organizer.mjs: 02 arm, singles, extension frames, 03 end, 06 clear, 07 names) ----
let FW = null, WV = null;
class FakeFM1 {
  constructor(name, ident) {
    this.sent = []; this.ops = [];
    this.input = { name, state: "disconnected", connection: "closed", onmidimessage: null, open: async () => {}, close: async () => {} };
    this.output = { name, state: "disconnected", connection: "closed", open: async () => {}, close: async () => {}, send: (m) => this.rx(Array.from(m)) };
    this.access = { inputs: new Map([["i", this.input]]), outputs: new Map([["o", this.output]]), onstatechange: null, sysexEnabled: true };
    this.plan = []; this.armed = -1; this.refuse = false; this.ackMs = 1; this.waves = new Map(); this.refuseWaves = false;
    this.banks = Array.from({ length: 6 }, () => new Map());
    for (const k of [0, 3, 7]) this.banks[0].set(k, "OWN " + String(k).padStart(3, "0"));   // (the user's own: overwritten)
    this.banks[2].set(5, "KEEP C006");
    this.up(ident);
  }
  up(ident) { this.ident = ident; this.input.state = this.output.state = "connected"; this.ev(); }
  down() { this.input.state = this.output.state = "disconnected"; this.ev(); }
  ev() { const f = this.access.onstatechange; if (f) f({ port: this.input }); }
  tx(bytes, ms = 2) { setTimeout(() => this.input.state === "connected" && this.input.onmidimessage && this.input.onmidimessage({ data: Uint8Array.from(bytes) }), ms); }
  request(addr, len) {
    this.waiting = addr;
    const u = [0, 0x59, 0x30, 8, 0, 0, 0, addr & 0xFF, (addr >>> 8) & 0xFF, (addr >>> 16) & 0xFF, (addr >>> 24) & 0xFF, len & 0xFF, len >> 8, 0];
    let s = 0;
    for (let i = 6; i < 14; i++) s += u[i];
    this.tx([0xF0, ...FW.pack7([...u, ~s & 0xFF]), 0xF7]);
  }
  rx(m) {
    if (this.output.state !== "connected") throw new Error("disconnected");
    this.sent.push(m);
    const j = m.join(), ota = /^ota-/.test(this.ident);
    if (j === FW.HS_QUERY.join()) {
      const t = [...this.ident].map((c) => c.charCodeAt(0));
      this.tx([0xF0, ...FW.pack7([0, 0x59, 0x11, 0, 0, 0, ...t, ...new Array(28 - t.length).fill(0)]), 0xF7]);
    } else if (j === FW.UPGRADE.join()) {
      this.queue = ota ? [[0, 512], [512, 512], [0xF0000000, 8]] : [[0, 512], [0xE0000000, 8]];
      this.request(...this.queue.shift());
    } else if (this.waiting != null && m[0] === 0xF0 && m.length > 20 && !(m[1] === 0x7D)) {
      const addr = this.waiting;
      this.waiting = null;
      if (addr >= 0xE0000000) {
        const next = this.plan.shift();
        setTimeout(() => this.down(), 20);
        if (next) setTimeout(() => this.up(next), 1200);
        return;
      }
      this.request(...this.queue.shift());
    } else if (ota) {
      return;
    } else if (m[0] === 0xF0 && m[1] === 0x7D && m[2] === 0x42 && m[3] === 0x55) {
      const c = m[4], bu = (...a) => this.tx([0xF0, 0x7D, 0x42, 0x55, ...a, 0xF7]);
      if (c === 1) bu(0x41, 4);
      else if (c === 2) {
        this.ops.push(["arm", m[5], m[6] << 7 | m[7]]);
        if (this.t0 == null) this.t0 = Date.now();
        if (this.refuse) { bu(0x42, m[5], 1); return; }
        this.armed = m[5]; bu(0x42, m[5], 0);
      } else if (c === 3 || c === 4) { this.t1 = Date.now(); this.ops.push([c === 3 ? "end" : "cancel", this.armed]); bu(0x43, this.armed < 0 ? 0x7F : this.armed, 0, 0, 0); this.armed = -1; }
      else if (c === 6) {
        if (this.armed < 0) { bu(0x52, m[5], 4, 0); return; }
        this.banks[this.armed].delete(m[5]); this.ops.push(["clear", this.armed, m[5]]); bu(0x52, m[5], 0, this.armed + 1);
      } else if (c === 7) {
        const b = m[5];
        this.ops.push(["names", b]);
        for (let part = 0; part < 4; part++) {
          const e = [];
          for (let k = 0; k < 32; k++) {
            const nm = this.banks[b].get(part * 32 + k);
            e.push(nm ? 1 : 0, ...(nm ? [...nm.padEnd(10, " ")].map((ch) => ch.charCodeAt(0)) : new Array(10).fill(0)));
          }
          bu(0x54, b, part, 0, ...e);
        }
      }
    } else if (m[0] === 0xF0 && m[1] === 0x7D && m[2] === 0x56 && m[3] === 0x46) {          // the user waves
      const c = m[4], H = [0xF0, 0x7D, 0x56, 0x46], crc3 = (c) => [(c >> 14) & 3, (c >> 7) & 0x7F, c & 0x7F];
      if (c === 0x01) {
        const bits = new Array(9).fill(0);
        for (const k of this.waves.keys()) bits[Math.floor((k - 2) / 7)] |= 1 << ((k - 2) % 7);
        this.tx([...H, 0x41, 1, 62, 2, ...bits, 0xF7]);
      } else if (c === 0x10) {
        if (this.refuseWaves) { this.ops.push(["wavefail", m[5]]); this.tx([...H, 0x50, 0x10, m[5], 3, 0, 0, 0, 0xF7]); return; }
        const rec = WV.unpack7(Uint8Array.from(m.slice(6, m.length - 4)), WV.W.REC);
        this.waves.set(m[5], rec);
        this.ops.push(["wave", m[5], String.fromCharCode(...rec.slice(0, 10)).trim()]);
        this.tx([...H, 0x50, 0x10, m[5], 0, ...crc3(WV.crc16(rec)), 0xF7], this.ackMs);
      }
    } else if (m[0] === 0xF0 && m.length === 267 && m[1] === 0) {
      const nm = String.fromCharCode(...m.slice(249, 259));
      if (this.armed >= 0) { this.banks[this.armed].set(m[8], nm.replace(/\s+$/, "")); this.ops.push(["single", this.armed, m[8], nm.trim()]); }
      this.tx([0xF0, 0x7D, 0x42, 0x55, 0x50, m[8], 0, this.armed >= 0 ? this.armed + 1 : 0, 0xF7], this.ackMs);
    } else if (m[0] === 0xF0 && m[1] === 0x7D && m[2] === 0x49 && m[3] === 0x58) {
      this.ops.push(["ext", this.armed, m[6]]);
      this.tx([0xF0, 0x7D, 0x42, 0x55, 0x51, m[6], this.armed >= 0 ? 0 : 4, this.armed >= 0 ? this.armed + 1 : 0, 0xF7], this.ackMs);
    }
  }
}

function open(file, dev) {
  const vc = new VirtualConsole();
  vc.on("jsdomError", (e) => { if (!/Not implemented/.test(e.message)) console.log("jsdom:", e.message); });
  const dom = new JSDOM(readFileSync(file, "utf8"), { runScripts: "dangerously", pretendToBeVisual: true, url: "http://localhost/infekt-fm1.html",
    virtualConsole: vc,
    beforeParse(w) {
      w.navigator.requestMIDIAccess = async () => dev.access;
      Object.defineProperty(w, "crypto", { value: webcrypto });
      w.TextEncoder = TextEncoder;
      w.HTMLDialogElement.prototype.showModal = function () { this.setAttribute("open", ""); };
      w.HTMLDialogElement.prototype.close = function (v) {
        if (v !== undefined) this.returnValue = v;
        this.removeAttribute("open"); this.dispatchEvent(new w.Event("close"));
      };
      w.Element.prototype.scrollIntoView = function () {};
      w.confirm = () => true;
    } });
  return dom.window;
}

// ---- without --factory: no option, no data ----
{
  const dev = new FakeFM1("InfeKt-FM1", "FM-1_13009400");
  const w = open(htmlNone, dev);
  FW = w.eval("FW"); WV = w.eval("WV");
  await sleep(300);
  const $ = (id) => w.document.getElementById(id);
  check("without --waves: the option hidden, no wave data, no button on the Wavetables tab", $("i-wavesopt").hidden &&
        $("wavedata").textContent.trim() === "null" && $("w-seterow").hidden && $("i-wavesrow").hidden);
  check("without --factory: the option hidden, no bank data, no button in the patch manager", $("i-factopt").hidden &&
        $("factdata").textContent.trim() === "null" && $("org-factgo").hidden && $("i-factrow").hidden &&
        w.infekt.org.LB.banks.length === 0);
  w.close();
}

// ---- with them ----
const dev = new FakeFM1("InfeKt-FM1", "FM-1_13009400");
const w = open(html, dev), d = w.document, $ = (id) => d.getElementById(id);
FW = w.eval("FW"); WV = w.eval("WV");
await sleep(300);
const PROD = w.infekt.I.product;
await until(() => !$("i-factopt").hidden, 3000);
check("with --factory: the option shown and ticked, its short line", !$("i-factopt").hidden && $("i-fact").checked &&
      /Werkbänke installieren/.test($("i-factopt").textContent));
const warnShown = () => [...d.querySelectorAll(".factwarn")].every((e) => !e.hidden);
check("ticked: the backup box says A and B are replaced (card and confirmation)", warnShown() &&
      /ersetzen Bank A und B/.test(d.querySelector(".ic-inst .note:not([hidden])").textContent) &&
      /ersetzen Bank A und B/.test($("i-confirm").querySelector(".note").textContent));
$("i-fact").checked = false; $("i-fact").dispatchEvent(new w.Event("change"));
check("unticked: the clause gone", [...d.querySelectorAll(".factwarn")].every((e) => e.hidden));
$("i-fact").checked = true; $("i-fact").dispatchEvent(new w.Event("change"));
// (1.18C48+ page pass: the tooltips laid out as a title, a list of facts and the thanks as their own section; compared
// as normalised text: case, punctuation, quotes and the list's separators left out)
const norm = (x) => x.toLowerCase().replace(/[„“”"'’]/g, "").replace(/[\s·;:,.()–—!?-]+/g, " ").trim();
const has = (txt, ...facts) => facts.every((f) => norm(txt).includes(norm(f)));
const tipTxt = (e) => { const c = e.cloneNode(true); c.querySelectorAll("li, .tp, .th, .th2, .tl").forEach((x) => x.append(" · ")); return c.textContent.replace(/\s+/g, " "); };
const tipOf = () => tipTxt($("i-factopt").querySelector(".tipt"));
check("its ? (German): overwrites A and B, C–F untouched, the thanks with the links", has(tipOf(), "überschreibt, was in A und B gespeichert ist",
      "C–F bleiben unberührt", "Besonderer Dank geht an drei Sounddesigner. Mit ihren Sounds macht ein Synth wie dieser besonders viel Spaß",
      "Manuel Schleis – Vengeance Sound", "weltweit bekannt für seine legendären Soundsets, Producer-Soundpacks und VST-Plug-ins",
      "Alan Marcero – Sounddesigner und Trance-Produzent", "Sete – Sounddesigner") &&
      $("i-factopt").querySelector(".tipt .tw") && /überschreibt/.test($("i-factopt").querySelector(".tipt .tw").textContent) &&
      !/https?:\/\//.test(tipOf()) &&
      $("i-factopt").querySelector('.tipt a[href="https://www.vengeance-sound.com/"][target="_blank"]') &&
      $("i-factopt").querySelector('.tipt a[href="https://alanmarcero.com/"][target="_blank"]') && $("i-factopt").querySelector(".tipt").hidden);
$("lang").click();
check("its ? (English): the same, the thanks in the website's words", has(tipOf(), "This overwrites what is stored in A and B",
      "Banks C–F stay untouched", "Special thanks go to three sound designers. With their sound designs a synth like this is particularly great fun:",
      "Manuel Schleis – Vengeance Sound (opens in a new tab), known worldwide for his legendary soundsets, producer sound packs and VST plug-ins",
      "Alan Marcero – sound designer and trance producer, alanmarcero.com (opens in a new tab)", "Sete – sound designer") &&
      /Factory banks replace banks A and B\./.test(d.querySelector(".ic-inst .note:not([hidden])").textContent));
$("lang").click();
check("with --waves: Sete's option shown and ticked, its short line with the slots", !$("i-wavesopt").hidden && $("i-waves").checked &&
      /Setes Wellen installieren \(Wellen 2–4\)/.test($("i-wavesopt").textContent) && !$("w-seterow").hidden);
const wTip = () => tipTxt($("i-wavesopt").querySelector(".tipt"));
check("its ? (German): made for this synth, thanks, from his no. 03 on, the Wavetables tab, more to come, the count",
      has(wTip(), "3 Wellen, eigens für diesen Synth gestaltet", "Vielen Dank an den Gestalter dieser Wellen: Sete",
          "ab seiner Nr. 03 (am FM-1: WAVE 2 bis WAVE 4)", "Sinus und Dreieck bleiben", "Tab Wavetables", "Weitere Wellen von Sete folgen"));
check("ticked: the backup box names the waves too", [...d.querySelectorAll(".setewarn")].every((e) => !e.hidden) &&
      /Setes Wellen ersetzen eigene Wellen auf WAVE 2–4\./.test(d.querySelector(".ic-inst .note:not([hidden])").textContent));
$("lang").click();
check("its ? (English)", has(wTip(), "3 wavetables, created especially for this synth", "With many thanks to the designer of these wavetables: Sete",
      "from his no. 03 on (on the FM-1: WAVE 2 to WAVE 4)", "Sine and triangle stay", "More wavetables by Sete will follow") &&
      /Sete's wavetables replace user waves on WAVE 2–4\./.test(d.querySelector(".ic-inst .note:not([hidden])").textContent));
$("lang").click();
$("i-waves").checked = false; $("i-waves").dispatchEvent(new w.Event("change"));
check("unticked: its clause gone", [...d.querySelectorAll(".setewarn")].every((e) => e.hidden));
$("i-waves").checked = true; $("i-waves").dispatchEvent(new w.Event("change"));
const L = w.infekt.org.LB.banks;
check("the patch manager: Factory A and Factory B as local banks (128 each, not unsaved), the button", L.length === 2 &&
      L[0].name === "Factory A" && L[1].name === "Factory B" && L.every((b) => b.slots.filter((x) => x).length === 128 && !b.dirty) &&
      L[0].slots[0].name === "TEST A000" && !!L[0].slots[0].x && L[1].slots[127].name === "TEST B127" && !$("org-factgo").hidden);

// another firmware file: hidden (the identity of this firmware, chosen as a file)
w.infekt.I.file = "other.fwsc";
$("lang").click(); $("lang").click();    // (the display said again: showFw)
check("another firmware file: both options hidden, no clause", $("i-factopt").hidden && $("i-wavesopt").hidden &&
      [...d.querySelectorAll(".factwarn, .setewarn")].every((e) => e.hidden));
w.infekt.I.file = "";
$("lang").click(); $("lang").click();
check("the included one again: shown", !$("i-factopt").hidden);

async function install() {
  dev.plan = ["ota-FM-1_1", PROD];
  dev.ops = [];
  $("i-mboot").checked = false;
  $("i-go").click();
  await sleep(50);
  $("i-bakok").checked = true; $("i-bakok").dispatchEvent(new w.Event("change"));
  $("i-confirm").close("go");
  await until(() => w.infekt.DEV.writing, 2000);
  await until(() => /^Fertig/.test($("i-status").textContent) || $("i-alert").textContent, 60000, 50);
  await until(() => !w.infekt.DEV.writing, 10000, 50);
}
const singles = (b) => dev.ops.filter((o) => o[0] === "single" && o[1] === b);
/* the wait display as it looks: shown, its headline, its line, the steps (class, label, detail), "step k of n", the bar,
   Close shown; taken at every change of its steps while an install runs */
const waitNow = () => ({ shown: !$("i-wait").hidden, h: $("i-waith").textContent, line: $("i-waitt").textContent,
  steps: [...$("i-waitsteps").children].map((li) => [li.className, li.querySelector(".wsl").textContent, li.querySelector(".wsd").textContent]),
  k: $("i-waitk").textContent, bar: +$("i-waitbar").value, bar0: +$("i-bar").value, close: !$("i-waitgo").hidden,
  unload: (() => { const e = new w.Event("beforeunload", { cancelable: true }); w.dispatchEvent(e); return e.defaultPrevented; })() });
function sampler() {
  const seen = [];
  const mo = new w.MutationObserver(() => { if (!$("i-wait").hidden) seen.push(waitNow()); });
  mo.observe($("i-waitsteps"), { childList: true });
  return { seen, stop: () => mo.disconnect() };
}

// ---- unticked: the firmware only ----
$("i-fact").checked = false; $("i-fact").dispatchEvent(new w.Event("change"));
$("i-waves").checked = false; $("i-waves").dispatchEvent(new w.Event("change"));
await install();
await until(() => /^Fertig/.test($("i-status").textContent), 10000);
await sleep(500);
check(`unticked: installed (${$("i-status").textContent}), no bank written`, /^Fertig$/.test($("i-status").textContent) &&
      !dev.ops.some((o) => o[0] === "arm" || o[0] === "single" || o[0] === "wave"));
{
  const x = waitNow();
  check(`unticked: the wait display with the firmware and its restart only, done, closable (${x.steps.map((s) => s[1]).join(", ")})`,
        x.shown && x.h === "Fertig." && x.close && x.steps.map((s) => s[0] + ":" + s[1]).join() === "ws-done:Firmware,ws-done:Neustart");
  $("i-waitclose").click();
}

// ---- ticked: after the restart, Sete's waves, then banks A and B ----
$("i-fact").checked = true; $("i-fact").dispatchEvent(new w.Event("change"));
$("i-waves").checked = true; $("i-waves").dispatchEvent(new w.Event("change"));
dev.t0 = null;
const smp = sampler();
await install();
await until(() => /Werkbänken/.test($("i-status").textContent) || $("i-alert").textContent, 60000, 50);
if (!/Werkbänken/.test($("i-status").textContent)) console.log($("i-alert").textContent + "\n" + $("i-log").textContent.slice(-1200));
const A = singles(0), B = singles(1), arms = dev.ops.filter((o) => o[0] === "arm"), ends = dev.ops.filter((o) => o[0] === "end");
check(`ticked: installed and the factory banks written (${$("i-status").textContent})`, /Fertig, mit Werkbänken/.test($("i-status").textContent) &&
      !$("i-alert").textContent && +$("i-bar").value === 100);
check(`banks A and B armed for 128 each, nothing else (${arms.map((o) => o.slice(1).join(":")).join(" ")})`,
      arms.length === 2 && arms[0][1] === 0 && arms[0][2] === 128 && arms[1][1] === 1 && arms[1][2] === 128);
check(`exactly 2 x 128 singles, in slot order, the right patches (${A.length} + ${B.length})`, A.length === 128 && B.length === 128 &&
      A.every((o, k) => o[2] === k && o[3] === "TEST A" + String(k).padStart(3, "0")) &&
      B.every((o, k) => o[2] === k && o[3] === "TEST B" + String(k).padStart(3, "0")) &&
      dev.ops.filter((o) => o[0] === "single").length === 256);
const ia = dev.ops.findIndex((o) => o[0] === "arm" && o[1] === 0), ib = dev.ops.findIndex((o) => o[0] === "arm" && o[1] === 1);
check("the extension frame after bank A's singles, before its flash write (03); one 03 a bank; no other bank touched",
      dev.ops.some((o, i) => o[0] === "ext" && o[1] === 0 && o[2] === 0 && i > dev.ops.lastIndexOf(A[127]) && i < ib) &&
      ends.length === 2 && dev.ops.indexOf(ends[0]) < ib && !dev.ops.some((o) => o[0] === "cancel" || o[0] === "clear") &&
      dev.banks[2].get(5) === "KEEP C006");
check("the names read back after the write (07 for A and B)", dev.ops.slice(dev.ops.indexOf(ends[1])).filter((o) => o[0] === "names").length >= 2);
check("the FM-1 holds them, the user's own in A replaced", dev.banks[0].get(0) === "TEST A000" && dev.banks[0].get(7) === "TEST A007" &&
      dev.banks[1].size === 128);
check("progress and log in the status card", /factory banks: bank A 128\/128/.test($("i-log").textContent) && /factory banks: bank B 128\/128/.test($("i-log").textContent));
check("nothing left queued in the patch manager", w.infekt.org.DB.banks.every((b) => b.pending.size === 0));
const wops = dev.ops.filter((o) => o[0] === "wave");
check(`the waves: Wave 03..05 into slots 2, 3, 4 in order with their names, before the banks (${wops.map((o) => o.slice(1).join(":")).join(" ")})`,
      wops.length === 3 && wops.map((o) => o[1]).join() === "2,3,4" && wops.map((o) => o[2]).join() === "TEST W03,TEST W04,TEST W05" &&
      dev.ops.indexOf(wops[2]) < dev.ops.findIndex((o) => o[0] === "arm") && /sete waves: 3\/3 WAVE 4 TEST W05/.test($("i-log").textContent));
check("the records as the Wavetables tab makes them (the same WAV through WV.harmonics, WV.record)", (() => {
  const S = w.eval("SETE");
  return S && S.waves.every((x) => WV.crc16(x.rec) === WV.crc16(dev.waves.get(x.slot)));
})());
smp.stop();
{
  const S = smp.seen, banks = S.filter((x) => x.steps.some((s) => s[0] === "ws-run" && s[1] === "Werkbänke A und B"));
  const mid = banks.find((x) => /^Bank A \d+\/128$/.test(x.steps[3][2]) && x.bar > 0 && x.bar < 100);
  check(`the wait display: shown from the start, four steps (${S.length ? S[0].steps.map((s) => s[1]).join(", ") : "none"})`, S.length > 0 &&
        S[0].shown && S[0].h === "Bitte warten" && S[0].line === "Das FM-1 nicht abziehen und diese Seite nicht schließen." &&
        S[0].steps.map((s) => s[1]).join() === "Firmware,Neustart,Setes Wellen,Werkbänke A und B" && S[0].steps[0][0] === "ws-run" && S[0].steps[0][2] === "läuft" &&
        S.every((x) => x.shown) && !S[0].close);
  check("the wait display during the bank write: its step running with the bank's progress, step 4 of 4, the bar mirrored, the earlier steps done, no Close, the page warns on leaving " +
        `(${mid ? mid.steps[3][2] + ", " + mid.k + ", " + mid.bar : "not seen"})`,
        !!mid && mid.h === "Bitte warten" && mid.k === "Schritt 4 von 4" && mid.bar === mid.bar0 &&
        mid.steps.slice(0, 3).every((s) => s[0] === "ws-done") && !mid.close && mid.unload);
  check("the wait display during the waves' write and the restart", S.some((x) => x.steps[2][0] === "ws-run" && /^Welle \d\/3$/.test(x.steps[2][2])) &&
        S.some((x) => x.steps[1][0] === "ws-run" && x.steps[0][0] === "ws-done"));
  const end = waitNow();
  check(`the wait display at the end: done, Close, every step done (${end.h} ${end.line})`, end.shown && end.h === "Fertig." &&
        end.line === "Das FM-1 kann jetzt abgezogen werden." && end.steps.every((s) => s[0] === "ws-done") && end.close && !end.unload);
  $("lang").click();
  const en = waitNow();
  check(`... in English (${en.h} ${en.line})`, en.h === "Done." && en.line === "You can unplug the FM-1 now." &&
        en.steps.map((s) => s[1]).join() === "Firmware,Restart,Sete's wavetables,Factory banks A and B");
  $("lang").click();
  $("i-waitclose").click();
  check("Close: the wait display gone", $("i-wait").hidden);
}
const ms = dev.t1 - dev.t0;
console.log(`timing: the bank write (2 x 128 singles, ack after ${dev.ackMs} ms) ${ms} ms on the simulated device`);

// ---- the waves refused: the banks still written, the waves' failure said, its button; then on its own ----
dev.refuseWaves = true;
await install();
await until(() => /Setes Wellen aber nicht/.test($("i-alert").textContent), 60000, 50);
check(`waves refused: the banks written all the same, the failure said, its button (${$("i-alert").textContent.slice(0, 70)})`,
      /Firmware installiert, Setes Wellen aber nicht/.test($("i-alert").textContent) && !$("i-wavesrow").hidden &&
      singles(0).length === 128 && singles(1).length === 128);
dev.refuseWaves = false; dev.ops = [];
$("i-wavesgo").click();
await until(() => /Setes Wellen installiert/.test($("i-status").textContent), 30000, 50);
check("its button later: written", /Setes Wellen installiert \(3\)/.test($("i-status").textContent) &&
      dev.ops.filter((o) => o[0] === "wave").length === 3 && $("i-wavesrow").hidden);
dev.ops = [];
$("w-setego").click();
await until(() => dev.ops.filter((o) => o[0] === "wave").length === 3 && !w.infekt.wv.busy, 30000, 50);
check("the Wavetables tab's button: the same", dev.ops.filter((o) => o[0] === "wave").length === 3);
$("i-waves").checked = false; $("i-waves").dispatchEvent(new w.Event("change"));
// ---- a write that fails: the firmware counts, the button offered, then on its own ----
dev.refuse = true;
await install();
await until(() => /Werkbänke aber nicht/.test($("i-alert").textContent), 60000, 50);
check(`a refused bank: the firmware installed, the failure said, the button shown (${$("i-alert").textContent.slice(0, 80)})`,
      /Firmware installiert, die Werkbänke aber nicht/.test($("i-alert").textContent) && !$("i-factrow").hidden && !$("i-factgo").disabled &&
      w.infekt.org.DB.banks.every((b) => b.pending.size === 0));
await until(() => !$("i-waitgo").hidden, 5000);
{
  const x = waitNow();
  check(`a refused bank: the wait display ends closable, the failure and its button named, the banks' step failed (${x.h}: ${x.line.slice(0, 60)})`,
        x.shown && x.h === "Nicht vollständig" && /Werkbänke aber nicht.*Später: „Werkbänke installieren“/.test(x.line) && x.close &&
        x.steps.map((s) => s[0]).join() === "ws-done,ws-done,ws-fail" && !x.unload);
  $("i-waitclose").click();
  check("Close: gone", $("i-wait").hidden);
}
dev.refuse = false; dev.ops = [];
$("i-factgo").click();
await until(() => /Werkbänke installiert/.test($("i-status").textContent) || /nicht installiert/.test($("i-alert").textContent), 60000, 50);
check(`the button later: written (${$("i-status").textContent})`, /Werkbänke installiert \(A und B\)/.test($("i-status").textContent) &&
      singles(0).length === 128 && singles(1).length === 128 && $("i-factrow").hidden);
{
  const x = waitNow();
  check(`the button on its own: its own wait display, one step, done, closable (${x.h})`, x.shown && x.h === "Fertig." && x.close &&
        x.steps.length === 1 && x.steps[0][0] === "ws-done" && x.steps[0][1] === "Werkbänke A und B");
  $("i-waitclose").click();
}
// the patch manager's button: the same
dev.ops = [];
$("org-factgo").click();
await until(() => singles(1).length === 128 && !w.infekt.org.busy, 60000, 50);
check("the patch manager's button: the same write", singles(0).length === 128 && singles(1).length === 128);
w.close();
console.log(bad ? bad + " FAILED" : "factory all ok");
process.exit(bad ? 1 : 0);
