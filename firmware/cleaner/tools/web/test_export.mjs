// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 J0KERPACK
// The InfeKt-FM1 page's patch export (Patches tab, panel 4) under jsdom with a simulated
// FM-1 that answers the dump (F0 7D 42 55 05 bank slot: its single, its extension frame F0 7D 49 58, then 53) and
// takes the extension frame after a single (51): a bank read with a range, empty slots left out, the extension tagged;
// one file of the selection (InfeKt: the extension frames; compatible: none, a shimmer target's amount to none), the
// same as .mid, a file per patch, the whole bank (read whole first), the edit buffer; the round trip: the exported
// file loaded in the upload list and uploaded into another bank gives the same bytes and extensions (the extension
// frames after all singles), the preview sends a patch's extension after its single; a firmware without the dump
// (hello version 1) is told. jsdom from the working directory's node_modules (npm install jsdom):
//   node test_export.mjs PAGE.html     (PAGE: make_page.py's output)
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { webcrypto } from "node:crypto";
const { JSDOM, VirtualConsole } = createRequire(process.cwd() + "/")("jsdom");

const [html] = process.argv.slice(2);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let bad = 0;
const check = (what, ok) => { console.log((ok ? "ok   " : "FAIL ") + what); if (!ok) bad++; };
async function until(f, ms, step = 30) { const end = Date.now() + ms; while (Date.now() < end) { if (f()) return true; await sleep(step); } return !!f(); }

const vc = new VirtualConsole();
vc.on("jsdomError", (e) => { if (!/Not implemented/.test(e.message)) console.log("jsdom:", e.message); });
let dev = null;
const dom = new JSDOM(readFileSync(html, "utf8"), { runScripts: "dangerously", pretendToBeVisual: true, url: "http://localhost/infekt-fm1.html",
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
    w.URL.createObjectURL = () => "blob:x"; w.URL.revokeObjectURL = () => {};
    w.HTMLAnchorElement.prototype.click = function () { w.downloads = (w.downloads || 0) + 1; };
    w.confirm = () => true;
  } });
const w = dom.window, d = w.document, $ = (id) => d.getElementById(id);
const FW = w.eval("FW"), BK = w.eval("BK");

// ---- patches: made-up bytes, a name; a shimmer extension (values, slot 1 on SHIM MOD (SHIM TONE before 1.18C45), LFO 1's assign on SHIM AMT)
function patch(k) {
  const p = new Uint8Array(256).map((_, i) => (i * 11 + k * 29) % 128);
  [..."EXP " + String(k).padStart(3, "0") + "   "].slice(0, 10).forEach((c, i) => (p[240 + i] = c.charCodeAt(0)));
  p[0] = 7;
  return p;
}
const EXTX = Uint8Array.from([64, 2, 90, 70, 0, 90, 1, 20, 2, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 3]);   // (1.18C47: WIDTH 90, W GLIDE 3)
function stored(k, x) {                  // as the FM-1 stores a patch: a destination on the shimmer off
  const p = patch(k);
  if (x) for (let r = 0; r < 12; r++) if (x[8 + r]) p[BK.EXT.RB[r]] = BK.EXT.DST_OFF[r];
  return p;
}

// ---- the simulated FM-1: the identity, the patch handshake (version v), the dump, the extension frame ----
class FakeFM1 {
  constructor(name, ident, ver = 2) {
    this.sent = []; this.ver = ver;
    this.input = { name, state: "connected", connection: "closed", onmidimessage: null, open: async () => {}, close: async () => {} };
    this.output = { name, state: "connected", connection: "closed", open: async () => {}, close: async () => {}, send: (m) => this.rx(Array.from(m)) };
    this.access = { inputs: new Map([["i", this.input]]), outputs: new Map([["o", this.output]]), onstatechange: null, sysexEnabled: true };
    this.ident = ident;
    // banks 0..2 C..E, 3 A, 4 B: slot -> {data, x}
    this.banks = [new Map(), new Map(), new Map(), new Map(), new Map()];
    this.banks[0].set(0, { data: stored(0, null), x: null });
    this.banks[0].set(2, { data: stored(2, EXTX), x: EXTX });
    this.banks[0].set(5, { data: stored(5, null), x: null });
    this.banks[0].set(100, { data: stored(100, null), x: null });
    this.banks[3].set(0, { data: stored(40, null), x: null });
    this.edit = { data: stored(7, EXTX), x: EXTX };
    this.armed = -1; this.lastEdit = -1; this.dumps = 0;
  }
  tx(bytes) { setTimeout(() => this.input.onmidimessage && this.input.onmidimessage({ data: Uint8Array.from(bytes) }), 1); }
  rx(m) {
    this.sent.push({ t: Date.now(), m });
    const j = m.join(), last = this.lastEdit;
    this.lastEdit = -1;
    if (j === FW.HS_QUERY.join()) {
      const t = [...this.ident].map((c) => c.charCodeAt(0));
      this.tx([0xF0, ...FW.pack7([0, 0x59, 0x11, 0, 0, 0, ...t, ...new Array(28 - t.length).fill(0)]), 0xF7]);
    } else if (m[0] === 0xF0 && m[1] === 0x7D && m[2] === 0x42 && m[3] === 0x55) {
      const c = m[4];
      if (c === 1) this.tx([0xF0, 0x7D, 0x42, 0x55, 0x41, this.ver, 0xF7]);
      else if (c === 2) { this.armed = m[5]; this.got = 0; this.tx([0xF0, 0x7D, 0x42, 0x55, 0x42, m[5], 0, 0xF7]); }
      else if (c === 3 || c === 4) { this.tx([0xF0, 0x7D, 0x42, 0x55, 0x43, this.armed, 0, this.got, 0, 0xF7]); this.armed = -1; }
      else if (c === 5 && this.ver >= 2) {
        const b = m[5], s = m[6], p = b === 0x7F ? this.edit : this.banks[b] ? this.banks[b].get(s) : undefined;
        this.dumps++;
        if (p) {
          this.tx(BK.buildSingle(p.data, b === 0x7F ? 0x40 : s, b === 0x7F ? 0 : BK.bankByte(b)));
          if (p.x) this.tx(BK.buildExt(p.x, b === 0x7F ? 0x40 : s, b === 0x7F ? 0 : BK.bankByte(b)));
        }
        this.tx([0xF0, 0x7D, 0x42, 0x55, 0x53, b, s, p ? 0 : this.banks[b] ? 1 : 2, p && p.x ? 1 : 0, 0xF7]);
      }
    } else if (m[0] === 0xF0 && m.length === 267 && m[1] === 0) {
      const data = Uint8Array.from(m.slice(9, 265));
      if (this.armed >= 0) { this.banks[this.armed].set(m[8], { data, x: null }); this.got++; }
      else { this.edit = { data, x: null }; this.lastEdit = m[8]; }
      this.tx([0xF0, 0x7D, 0x42, 0x55, 0x50, m[8], 0, this.armed >= 0 ? this.armed + 1 : 0, 0xF7]);
    } else if (m[0] === 0xF0 && m[1] === 0x7D && m[2] === 0x49 && m[3] === 0x58 && this.ver >= 2) {
      const e = BK.parseExt(Uint8Array.from(m)), pp = m[6];
      let st = e && e.ok ? 0 : 1;
      if (!st && this.armed >= 0) { const p = this.banks[this.armed].get(pp); if (p) p.x = e.x; else st = 4; }
      else if (!st) { if (last === pp) this.edit.x = e.x; else st = 4; }
      this.tx([0xF0, 0x7D, 0x42, 0x55, 0x51, pp, st, this.armed >= 0 ? this.armed + 1 : 0, 0xF7]);
    }
  }
}
dev = new FakeFM1("InfeKt-FM1", "FM-1_13010500");
await sleep(300);
w.infekt.showTab("patches");
$("d-connect").click();
await until(() => w.infekt.DEV.state === "ok", 8000);
const bk = w.infekt.bk;
check("connected, the patch link's hello version 2", w.infekt.DEV.state === "ok" && bk.link && bk.link.ver === 2);
check("panel 4 in German: bank A first, the edit buffer last, the InfeKt format explained", /Vom FM-1 exportieren/.test($("x-panel").textContent) &&
      [...$("x-bank").options].map((o) => o.textContent).join() === "A,B,C,D,E,F,Edit-Puffer" && /Erweiterung/.test($("x-fmthelp").textContent) &&
      $("x-dlsel").disabled && $("x-dlbank").textContent === "Ganze Bank A");
check("hello version 2 (before 1.18C30): banks D-F off", [...$("x-bank").options].filter((o) => o.disabled).map((o) => o.textContent).join() === "D,E,F");

// ---- a bank read with a range ----
$("x-range").value = "1-8, 101";
await bk.xRead();
const items = () => bk.xItems;
check(`read C 1-8, 101: ${$("x-found").textContent}`, items().length === 4 && items().map((it) => it.slot).join() === "0,2,5,100" &&
      /4 Patches gelesen \(A\), 5 Plätze leer/.test($("x-found").textContent) && dev.dumps === 9);
check("listed with their slots and names, the extension tagged", d.querySelectorAll("#x-list tbody tr").length === 4 &&
      d.querySelector("#x-list tbody tr:nth-child(2) td.to").textContent === "A003" && /EXP 002/.test(d.querySelector("#x-list tbody tr:nth-child(2)").textContent) &&
      d.querySelectorAll("#x-list .tag").length === 1 && /Shimmer/.test(d.querySelector("#x-list tbody tr:nth-child(2) .tag").textContent));
check("the bytes as stored, the extension as sent", items()[1].data.every((v, i) => v === stored(2, EXTX)[i]) && items()[1].x.join() === EXTX.join() && !items()[0].x);

// ---- the selection as one file (InfeKt) ----
items()[3].sel = false;
$("x-dlsel").click();
let f = bk.saved.at(-1);
let p = BK.parseFile(f.bytes, f.name);
check(`one file of the selection: ${f.name}`, f.name === "InfeKt_A001-A006.syx" && p.singles.length === 3 && p.ext === 1 && p.other === 0 &&
      p.singles.map((s) => s.prog).join() === "0,2,5" && p.singles.every((s) => s.bank === 3 && s.ok && !s.sumbad));
check("its bytes and extension as read", p.singles.every((s, k) => s.data.every((v, i) => v === items()[k].data[i])) && p.singles[1].x.join() === EXTX.join() &&
      !p.singles[0].x && BK.framesOfSyx(f.bytes).length === 4);

// ---- compatible ----
$("x-compat").checked = true; $("x-compat").dispatchEvent(new w.Event("change"));
check("compatible: the help says the classic single format, the tag says it is dropped",
      /klassisches Single-Format \(256 Byte, ohne Erweiterungen\)/.test($("x-fmthelp").textContent) && d.querySelector("#x-list .tag.drop"));
$("x-dlsel").click();
f = bk.saved.at(-1);
p = BK.parseFile(f.bytes, f.name);
const c2 = p.singles[1].data, o2 = items()[1].data;
check(`compatible file: ${f.name}, no extension frames`, f.name === "InfeKt_A001-A006_compat.syx" && p.singles.length === 3 && p.ext === 0 &&
      BK.framesOfSyx(f.bytes).length === 3 && p.singles.every((s) => s.ok && !s.sumbad));
check("the shimmer's destinations off, their amounts none (slot 1: B 66 = 64, LFO 1: B 80 = 64), the rest as read",
      c2[193] === 0 && c2[194] === 64 && c2[207] === 0 && c2[208] === 64 &&
      c2.every((v, i) => i === 194 || i === 208 || v === o2[i]) && p.singles[0].data.every((v, i) => v === items()[0].data[i]));
check("the log names what was neutralised", /A003 EXP 002: kompatibel, 2 Bytes neutralisiert/.test($("b-log").textContent));
$("x-compat").checked = false; $("x-compat").dispatchEvent(new w.Event("change"));

// ---- .mid, a file each ----
$("x-fmt").value = "mid"; $("x-fmt").dispatchEvent(new w.Event("change"));
$("x-dlsel").click();
f = bk.saved.at(-1);
p = BK.parseFile(f.bytes, f.name);
check(`.mid: ${f.name}, the same singles and extension`, f.name === "InfeKt_A001-A006.mid" && p.singles.length === 3 && p.ext === 1 &&
      p.singles[1].x.join() === EXTX.join() && p.singles[2].data.every((v, i) => v === items()[2].data[i]));
$("x-fmt").value = "syx"; $("x-fmt").dispatchEvent(new w.Event("change"));
let n0 = bk.saved.length;
await bk.xSaveEach();
check(`a file each: ${bk.saved.slice(n0).map((x) => x.name).join(", ")}`, bk.saved.length === n0 + 3 &&
      bk.saved.slice(n0).map((x) => x.name).join() === "A001_EXP_000.syx,A003_EXP_002.syx,A006_EXP_005.syx" &&
      BK.parseFile(bk.saved[n0 + 1].bytes).singles[0].x.join() === EXTX.join() && /3 Dateien gespeichert/.test($("b-status").textContent));

// ---- the whole bank: read whole first, the filled slots in order ----
const d0 = dev.dumps;
await bk.xSaveBank();
f = bk.saved.at(-1);
p = BK.parseFile(f.bytes, f.name);
check(`whole bank A: ${f.name}, 128 slots asked, the 4 filled in order`, f.name === "InfeKt_Bank_A.syx" && dev.dumps - d0 === 128 &&
      p.singles.map((s) => s.prog).join() === "0,2,5,100" && p.ext === 1);
await bk.xSaveBank();
check("again: not read again", dev.dumps - d0 === 128 && bk.saved.at(-1).name === "InfeKt_Bank_A.syx");

// ---- the edit buffer (cleaner: no factory banks to read) ----
$("x-bank").value = "127"; $("x-bank").dispatchEvent(new w.Event("change"));
check("the edit buffer: no range, no whole bank", $("x-range").disabled && $("x-dlbank").disabled);
await bk.xRead();
$("x-dlsel").click();
f = bk.saved.at(-1);
p = BK.parseFile(f.bytes, f.name);
check(`the edit buffer: ${f.name}, its extension`, items().length === 1 && f.name === "EDIT_EXP_007.syx" && p.singles.length === 1 &&
      p.singles[0].bank === 0 && p.singles[0].prog === 0 && p.singles[0].x.join() === EXTX.join());

// ---- the round trip: the InfeKt file of C uploaded into bank D, on its own numbers ----
const whole = bk.saved.find((x) => x.name === "InfeKt_Bank_A.syx");
await bk.addFiles([{ name: whole.name, arrayBuffer: async () => whole.bytes.buffer.slice(0) }]);
check("loaded in the upload list: 4 singles, one tagged Shimmer", bk.singles.length === 4 && d.querySelectorAll("#b-list .tag").length === 1 &&
      /4 Singles, 1 mit InfeKt-Erweiterung/.test($("b-log").textContent));
$("b-bank").value = "1";
d.querySelector("input[name=b-slots][value=keep]").checked = true;
bk.render();
n0 = dev.sent.length;
await bk.upload();
const a = dev.sent.slice(n0).filter((x) => x.m[0] === 0xF0), kinds = a.map((x) => (x.m.length === 267 ? "S" + x.m[8] : x.m[2] === 0x49 ? "X" + x.m[6] : x.m[4] === 2 ? "arm" : x.m[4] === 3 ? "end" : "?"));
check(`upload into D: ${kinds.join(" ")}`, kinds.join() === "arm,S0,S2,S5,S100,X2,end");
const D = dev.banks[1];
check("D holds C's bytes and extension (the round trip)", [0, 2, 5, 100].every((s) => D.get(s) && D.get(s).data.every((v, i) => v === dev.banks[0].get(s).data[i])) &&
      D.get(2).x.join() === EXTX.join() && !D.get(0).x && /Fertig: 4 in Bank B gespeichert/.test($("b-status").textContent));

// ---- the preview: the extension after its single ----
$("b-pvmode").value = "off";
n0 = dev.sent.length;
w.infekt.bk.render();
const rows = () => [...d.querySelectorAll("#b-list tbody tr:not(.notes)")];
rows()[1].querySelector("button.play").click();
await sleep(50);
if ($("b-warn").open) $("b-warnyes").click();
await until(() => dev.sent.slice(n0).some((x) => x.m[2] === 0x49), 2000);
const pv = dev.sent.slice(n0).filter((x) => x.m.length === 267 || x.m[2] === 0x49);
check("preview: the single, then its extension frame; the edit buffer has both", pv.length === 2 && pv[0].m.length === 267 && pv[1].m[2] === 0x49 &&
      dev.edit.x && dev.edit.x.join() === EXTX.join());

// ---- English; a firmware without the dump ----
$("lang").click();
check("English texts", /Export from the FM-1/.test($("x-panel").textContent) && /Selection as a file/.test($("x-dlsel").textContent) &&
      /extension/.test($("x-fmthelp").textContent));
$("lang").click();
bk.link.ver = 1;
$("x-bank").value = "0"; $("x-bank").dispatchEvent(new w.Event("change"));
await bk.xRead();
check(`hello version 1: ${$("b-status").textContent}`, /sendet keine Patches/.test($("b-status").textContent));

console.log(bad ? `export: ${bad} FAILED` : "export all ok");
process.exit(bad ? 1 : 0);
