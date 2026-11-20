// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 J0KERPACK
// The InfeKt-FM1 page's patch manager (Patches tab, "Patchverwaltung") in Chromium (Playwright) with a simulated FM-1
// that speaks the bank handshake (cleaner/fm1/app.c, patchx.c) as the firmware does: the
// hello (version 4: 07 a bank's names, 06 a slot emptied within an upload; 3 without them), the upload (02, singles,
// extension frames, 03), the dump (05). Checks: the names read at once (no dumps), local banks from files (a bank file,
// single patches added to the bank shown, files dropped from the desktop, a bad checksum skipped), copies to the FM-1
// only queued (marked, nothing sent) until "write" and its confirmation (one upload per bank, slots in order, the slot
// cleared with 06, the extension after the singles, the names read back), a rename on the FM-1, drag and drop between
// and within the columns, swap, undo, the unsaved marker, the exports (a local bank, a selection, a device bank, all
// banks as one backup that opens again as one local bank per device bank), a firmware of version 3 (read patch by
// patch, no clearing on the device). Screenshots of the desktop and the phone layout into SHOTS (if given).
//   node test_organizer.mjs PAGE.html [SHOTS_DIR]
import { createRequire } from "node:module";
import { pathToFileURL } from "node:url";
import { mkdirSync } from "node:fs";
const { chromium } = createRequire(process.cwd() + "/")("playwright");

const [html, shots] = process.argv.slice(2);
let bad = 0;
const check = (what, ok) => { console.log((ok ? "ok   " : "FAIL ") + what); if (!ok) bad++; };
if (shots) mkdirSync(shots, { recursive: true });

// ---- the simulated FM-1, inside the page (navigator.requestMIDIAccess answers with it) ----
const FAKE = () => {
  const patch = (k, nm) => {
    const p = new Uint8Array(256).map((_, i) => (i * 13 + k * 31) % 128);
    [...(nm || "DEV " + String(k).padStart(3, "0")).padEnd(10, " ")].slice(0, 10).forEach((c, i) => (p[240 + i] = c.charCodeAt(0)));
    return p;
  };
  class FakeFM1 {
    constructor(ver) {
      this.ver = ver; this.sent = []; this.dumps = 0; this.names = 0; this.uploads = []; this.cur = null;
      this.input = { name: "InfeKt-FM1", state: "connected", connection: "closed", onmidimessage: null, open: async () => {}, close: async () => {} };
      this.output = { name: "InfeKt-FM1", state: "connected", connection: "closed", open: async () => {}, close: async () => {}, send: (m) => this.rx(Array.from(m)) };
      this.access = { inputs: new Map([["i", this.input]]), outputs: new Map([["o", this.output]]), onstatechange: null, sysexEnabled: true };
      this.banks = Array.from({ length: 6 }, () => new Map());
      for (const k of [0, 1, 2, 9, 20]) this.banks[0].set(k, { data: patch(k), x: null });
      this.banks[0].set(5, { data: patch(5, "SHIMMERY"), x: Uint8Array.from([64, 2, 90, 70, 0, 0, 1, 20, 2, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0]) });
      for (const k of [0, 127]) this.banks[1].set(k, { data: patch(100 + k), x: null });
      this.edit = { data: patch(77, "EDITBUF"), x: null };
      this.armed = -1;
    }
    tx(b) { setTimeout(() => this.input.onmidimessage && this.input.onmidimessage({ data: Uint8Array.from(b) }), 1); }
    rx(m) {
      const FW = window.FWX, BK = window.BK;
      this.sent.push(m);
      if (m.join() === FW.HS_QUERY.join()) {
        const t = [..."FM-1_13011829"].map((c) => c.charCodeAt(0));
        this.tx([0xF0, ...FW.pack7([0, 0x59, 0x11, 0, 0, 0, ...t, ...new Array(28 - t.length).fill(0)]), 0xF7]);
      } else if (m[0] === 0xF0 && m[1] === 0x7D && m[2] === 0x42 && m[3] === 0x55) {
        const c = m[4], bu = (...a) => this.tx([0xF0, 0x7D, 0x42, 0x55, ...a, 0xF7]);
        if (c === 1) bu(0x41, this.ver);
        else if (c === 2) { this.armed = m[5]; this.cur = { bank: m[5], n: m[6] << 7 | m[7], ops: [] }; bu(0x42, m[5], 0); }
        else if (c === 3 || c === 4) { bu(0x43, this.armed < 0 ? 0x7F : this.armed, 0, this.cur ? this.cur.ops.length : 0, 0); if (this.cur) this.uploads.push(this.cur); this.cur = null; this.armed = -1; }
        else if (c === 5) {
          const b = m[5], s = m[6], p = b === 0x7F ? this.edit : this.banks[b] ? this.banks[b].get(s) : undefined;
          this.dumps++;
          if (p) {
            this.tx(BK.buildSingle(p.data, b === 0x7F ? 0x40 : s, b === 0x7F ? 0 : BK.bankByte(b)));
            if (p.x) this.tx(BK.buildExt(p.x, b === 0x7F ? 0x40 : s, b === 0x7F ? 0 : BK.bankByte(b)));
          }
          bu(0x53, b, s, p ? 0 : this.banks[b] ? 1 : 2, p && p.x ? 1 : 0);
        } else if (c === 6 && this.ver >= 4) {
          const s = m[5];
          if (this.armed < 0) bu(0x52, s, 4, 0);
          else { this.banks[this.armed].delete(s); this.cur.ops.push(["clear", s]); bu(0x52, s, 0, this.armed + 1); }
        } else if (c === 7 && this.ver >= 4) {
          const b = m[5];
          this.names++;
          if (!this.banks[b]) { bu(0x54, b, 0, 2); return; }
          for (let part = 0; part < 4; part++) {
            const e = [];
            for (let k = 0; k < 32; k++) {
              const p = this.banks[b].get(part * 32 + k);
              e.push(p ? 1 : 0, ...(p ? Array.from(p.data.subarray(240, 250)) : new Array(10).fill(0)));
            }
            bu(0x54, b, part, 0, ...e);
          }
        }
      } else if (m[0] === 0xF0 && m.length === 267 && m[1] === 0) {
        const data = Uint8Array.from(m.slice(9, 265));
        if (this.armed >= 0) { this.banks[this.armed].set(m[8], { data, x: null }); this.cur.ops.push(["single", m[8]]); }
        else this.edit = { data, x: null };
        this.tx([0xF0, 0x7D, 0x42, 0x55, 0x50, m[8], 0, this.armed >= 0 ? this.armed + 1 : 0, 0xF7]);
      } else if (m[0] === 0xF0 && m[1] === 0x7D && m[2] === 0x49 && m[3] === 0x58) {
        const e = window.BK.parseExt(Uint8Array.from(m)), pp = m[6];
        let st = e && e.ok ? 0 : 1;
        if (!st && this.armed >= 0) { const p = this.banks[this.armed].get(pp); if (p) { p.x = e.x; this.cur.ops.push(["ext", pp]); } else st = 4; }
        this.tx([0xF0, 0x7D, 0x42, 0x55, 0x51, pp, st, this.armed >= 0 ? this.armed + 1 : 0, 0xF7]);
      }
    }
  }
  window.__mkFake = (ver) => { window.__fake = new FakeFM1(ver); return true; };
  navigator.requestMIDIAccess = async () => window.__fake.access;
  window.confirm = () => true;
};

// ---- files: made-up patches (a bank of 20, single patches, one with a bad checksum) ----
function single(k, prog, nm, bank = 1, badsum = false) {
  const d = new Uint8Array(256).map((_, i) => (i * 7 + k * 17) % 128);
  [...(nm || "LOC " + String(k).padStart(3, "0")).padEnd(10, " ")].slice(0, 10).forEach((c, i) => (d[240 + i] = c.charCodeAt(0)));
  const f = new Uint8Array(267);
  f.set([0xF0, 0, 0x20, 0x33, 0x01, 0, 0x10, bank, prog]);
  f.set(d, 9);
  let s = 0;
  for (let i = 5; i < 265; i++) s += f[i];
  f[265] = badsum ? (s + 1) & 0x7F : s & 0x7F;
  f[266] = 0xF7;
  if (badsum) f[20] = 0x80;              // (not 7-bit either: refused)
  return f;
}
const cat = (...a) => { const o = new Uint8Array(a.reduce((n, x) => n + x.length, 0)); let i = 0; for (const x of a) { o.set(x, i); i += x.length; } return o; };
const bankFile = cat(...Array.from({ length: 20 }, (_, k) => single(k, k * 2)));   // (slots 1, 3, 5 ..)
const sgl = (k, nm) => ({ name: nm + ".syx", mimeType: "application/octet-stream", buffer: Buffer.from(single(k, 0, nm.toUpperCase().slice(0, 10))) });

async function run(ver) {
  const browser = await chromium.launch();
  const ctx = await browser.newContext({ viewport: { width: 1360, height: 1000 }, deviceScaleFactor: 2 });
  const page = await ctx.newPage();
  const errs = [];
  page.on("pageerror", (e) => errs.push(e.message));
  page.on("console", (m) => { if (m.type() === "error") errs.push(m.text()); });
  await page.addInitScript(FAKE);
  await page.addInitScript(`window.__mkFake(${ver})`);
  await page.goto(pathToFileURL(html).href + "#patches");
  await page.evaluate(() => { window.FWX = FW; window.BK = BK; });   // (the page's cores: global, not on window)
  return { browser, page, errs };
}
const ev = (page, f, a) => page.evaluate(f, a);
const until = async (page, f, a, ms = 8000) => { try { await page.waitForFunction(f, a, { timeout: ms }); return true; } catch (_) { return false; } };
const rows = (page, sd) => page.$$eval(`#org-${sd === "L" ? "llist" : "dlist"} .orow`, (r) => r.map((x) => ({
  i: +x.dataset.i, name: x.querySelector(".oname").textContent, cls: x.className, tags: x.querySelector(".otags").textContent })));
const row = (sd, i) => `#org-${sd === "L" ? "llist" : "dlist"} .orow[data-i="${i}"]`;

// ======================= version 4 =======================
{
  const { browser, page, errs } = await run(4);
  await page.click("#d-connect");
  check("connected, the patch handshake's version 4", await until(page, () => window.infekt.DEV.state === "ok" && window.infekt.bk.link && window.infekt.bk.link.ver === 4));
  check("the button in the Patches area, the manager closed", await page.isVisible("#org-open") && !(await page.isVisible("#org-box")) &&
        /Patchverwaltung öffnen/.test(await page.textContent("#org-open")));
  await page.click("#org-open");
  check("open: the classic panels hidden, the three columns shown", await page.isVisible("#org-colL") && await page.isVisible("#org-colD") &&
        !(await page.isVisible("#b-grid")));
  check("the banks' names read at once (07), no dumps", await until(page, () => window.infekt.org.DB.banks.every((b) => b.read)) &&
        await ev(page, () => window.__fake.names === 6 && window.__fake.dumps === 0));
  let d = await rows(page, "D");
  check("bank A: 128 slots, names as stored, empty ones marked", d.length === 128 && d[0].name === "DEV 000" && d[5].name === "SHIMMERY" &&
        /empty/.test(d[3].cls) && d[3].name === "– leer –");
  // ---- local: a bank file, then two single patches into it, a bad file
  await page.setInputFiles("#org-lfile", [{ name: "mybank.syx", mimeType: "application/octet-stream", buffer: Buffer.from(bankFile) }]);
  await until(page, () => window.infekt.org.LB.banks.length === 1);
  let L = await rows(page, "L");
  check("a bank file: a local bank of its own, the patches at their program numbers", L.length === 128 && L[0].name === "LOC 000" &&
        L[2].name === "LOC 001" && /empty/.test(L[1].cls) && await page.inputValue("#org-lbank") === "0" &&
        /mybank/.test(await page.textContent("#org-lbank")));
  check("opened from a file: not marked unsaved", !/ungespeichert/.test(await page.textContent("#org-lstate")));
  await page.setInputFiles("#org-lfile", [sgl(50, "bass one"), sgl(51, "lead two")]);
  await until(page, () => window.infekt.org.LB.banks[0].slots[1]);
  L = await rows(page, "L");
  check("single patches: into the free slots of the bank shown, marked unsaved", L[1].name === "BASS ONE" && L[3].name === "LEAD TWO" &&
        /ungespeichert/.test(await page.textContent("#org-lstate")) && /● mybank/.test(await page.textContent("#org-lbank")));
  await page.setInputFiles("#org-lfile", [{ name: "broken.syx", mimeType: "application/octet-stream", buffer: Buffer.from(single(9, 0, "BROKEN", 1, true)) }]);
  await page.waitForTimeout(200);
  check("a single with a bad checksum (not 7-bit): skipped, said", !(await ev(page, () => window.infekt.org.LB.banks[0].slots.some((s) => s && s.name === "BROKEN"))) &&
        /BROKEN|broken/.test(await page.textContent("#org-status")));
  // a file dropped from the desktop onto local slot 100
  await ev(page, () => {
    const f = new File([Uint8Array.from(window.BK.buildSingle(new Uint8Array(256).fill(5).map((v, i) => (i >= 240 && i < 250 ? "DROPPED   ".charCodeAt(i - 240) : v)), 0))], "dropped.syx");
    const dt = new DataTransfer(); dt.items.add(f);
    const r = document.querySelector('#org-llist .orow[data-i="99"]');
    r.dispatchEvent(new DragEvent("drop", { dataTransfer: dt, bubbles: true, cancelable: true }));
  });
  check("a file dropped from the desktop on slot 100: there", await until(page, () => (window.infekt.org.LB.banks[0].slots[99] || {}).name === "DROPPED"));

  // ---- left -> right: LOC 000, BASS ONE, LOC 001 (slots 1-3) to the FM-1 from A011 on: queued only
  await page.click(row("L", 0)); await page.click(row("L", 2), { modifiers: ["Shift"] });
  await page.click(row("D", 10));
  const sent0 = await ev(page, () => window.__fake.sent.length);
  await page.click("#org-toR");
  d = await rows(page, "D");
  if (d[10].name !== "LOC 000") console.log(JSON.stringify(d.slice(9, 14)), await page.textContent("#org-status"));
  check("copy to the FM-1: queued at A011-A013, marked, nothing sent", d[10].name === "LOC 000" && d[11].name === "BASS ONE" && d[12].name === "LOC 001" &&
        /pend/.test(d[10].cls) && /neu/.test(d[10].tags) && await ev(page, (n) => window.__fake.sent.length === n, sent0) &&
        /\(3\)/.test(await page.textContent("#org-dwrite")) && (await page.textContent("#org-dtabs button[data-b='0']")) === "A" &&
        await page.getAttribute("#org-dtabs button[data-b='0']", "data-n") === "3");
  // drag LOC 005 (local slot 11) onto the FM-1's A001 (filled: replaces)
  await page.click(row("L", 10));
  await page.dragAndDrop(row("L", 10), row("D", 0));
  d = await rows(page, "D");
  check("drag from local onto A001: queued, replacing DEV 000", d[0].name === "LOC 005" && /ersetzt/.test(d[0].tags));
  // clear A003 on the FM-1, rename A010 (dumps its bytes first)
  await page.click(row("D", 2)); await page.click("#org-clear");
  d = await rows(page, "D");
  check("clear A003: queued, the old name struck", /del/.test(d[2].cls) && d[2].name === "DEV 002" && /geleert/.test(d[2].tags));
  await page.dblclick(row("D", 9) + " .oname");
  await page.fill(row("D", 9) + " input.oedit", "Renamed!");
  await page.press(row("D", 9) + " input.oedit", "Enter");
  await until(page, () => (document.querySelector('#org-dlist .orow[data-i="9"] .oname') || {}).textContent === "Renamed!");   // (the rename
  d = await rows(page, "D");             // reads the slot's bytes first: a dump, then the list again)
  check("rename A010 on the FM-1: its bytes read (one dump), the rename queued", d[9].name === "Renamed!" && /pend/.test(d[9].cls) &&
        await ev(page, () => window.__fake.dumps === 1));
  // copy SHIMMERY (A006) to the local bank (dumped with its extension), then to A050 (to be written with it)
  await page.click(row("D", 5)); await page.click(row("L", 60));
  await page.click("#org-toL");
  L = await rows(page, "L");
  check("A006 to local slot 61: with its extension (tagged)", L[60].name === "SHIMMERY" && /Shimmer/.test(L[60].tags));
  await page.click(row("L", 60)); await page.click(row("D", 49));
  await page.click("#org-toR");
  // ---- undo: the last queue taken back, then again
  await page.click("#org-undo");
  check("undo: A050 no longer queued", !(await ev(page, () => window.infekt.org.DB.banks[0].pending.has(49))));
  await page.click(row("L", 60)); await page.click(row("D", 49)); await page.click("#org-toR");
  const pend = await ev(page, () => window.infekt.org.DB.banks[0].pending.size);
  check("queued: 7 changes in bank A", pend === 7);
  if (shots) {
    await page.click(row("L", 0)); await page.click(row("L", 4), { modifiers: ["Control"] });
    await page.screenshot({ path: shots + "/organizer-desktop.png", fullPage: true });
  }
  // ---- write: the confirmation, cancelled first
  await page.click("#org-dwrite");
  check("write: a confirmation listing the 7 changes", await page.isVisible("#org-confirm") && (await page.$$("#org-confirmlist li")).length === 7 &&
        /A003: leeren \(DEV 002\)/.test(await page.textContent("#org-confirmlist")));
  if (shots) await page.screenshot({ path: shots + "/organizer-confirm.png" });
  await page.click("#org-confirmno");
  check("cancelled: nothing sent, still queued", await ev(page, () => window.__fake.uploads.length === 0 && window.infekt.org.DB.banks[0].pending.size === 7));
  await page.click("#org-dwrite"); await page.click("#org-confirmyes");
  check("written", await until(page, () => !window.infekt.org.busy && window.infekt.org.DB.banks[0].pending.size === 0 && window.__fake.uploads.length === 1));
  const up = await ev(page, () => window.__fake.uploads[0]);
  check(`one upload into bank A, the slots in order, A003 cleared with 06, the extension after the singles: ${up.ops.map((o) => o.join(":")).join(" ")}`,
        up.bank === 0 && up.n === 7 && up.ops.map((o) => o.join(":")).join(" ") === "single:0 clear:2 single:9 single:10 single:11 single:12 single:49 ext:49");
  check("the FM-1 holds the bytes: A011 = local slot 1, A050 with SHIMMERY's extension, A003 gone, A010 renamed", await ev(page, () => {
    const f = window.__fake.banks[0], o = window.infekt.org.LB.banks[0];
    return f.get(10).data.every((v, i) => v === o.slots[0].data[i]) && !f.has(2) && f.get(49).x && f.get(49).x[0] === 64 &&
           window.BK.nameOf(f.get(9).data) === "Renamed!" && window.BK.nameOf(f.get(0).data) === "LOC 005";
  }));
  check("the names read back after the write (07 again), no longer marked", await ev(page, () => window.__fake.names === 7) &&
        !(await rows(page, "D")).some((r) => /pend/.test(r.cls)) && /Geschrieben: 7/.test(await page.textContent("#org-status")));
  // ---- swap: local slot 2 (BASS ONE) <-> A002 (DEV 001). The swap reads A002's bytes first (a dump: the simulated
  //      FM-1 answers a millisecond later), so its result is waited for, not read at once after the click (a read at
  //      once failed about one run in six: the dump not yet answered; the swap itself was right every time)
  await page.click(row("L", 1)); await page.click(row("D", 1)); await page.click("#org-swap");
  check("swap: local slot 2 holds DEV 001, A002 queued with BASS ONE", await until(page, () => window.infekt.org.LB.banks[0].slots[1]?.name === "DEV 001" &&
        window.infekt.org.DB.banks[0].pending.get(1)?.name === "BASS ONE"));
  await page.click("#org-ddiscard");
  check("discard: nothing queued", await ev(page, () => window.infekt.org.DB.banks[0].pending.size === 0));
  // ---- rearrange within the local bank by dragging: slot 1 onto slot 6 (empty): moved
  await page.click(row("L", 0));
  await page.dragAndDrop(row("L", 0), row("L", 5));
  check("drag within local: slot 1 moved to slot 6, slot 1 empty", await ev(page, () => { const s = window.infekt.org.LB.banks[0].slots; return !s[0] && s[5] && s[5].name === "LOC 000"; }));
  // ---- move from the FM-1 to local: A021 (DEV 020) queued to be cleared
  await page.click(row("D", 20)); await page.click(row("L", 120)); await page.click("#org-mvL");
  check("move A021 to local: there, A021 queued to be cleared", await until(page, () => window.infekt.org.LB.banks[0].slots[120]?.name === "DEV 020" &&
        window.infekt.org.DB.banks[0].pending.get(20) === null));   // (a dump first, as the swap's)
  await page.click("#org-ddiscard");
  // ---- exports
  await page.click("#org-lsave");
  let sv = await ev(page, () => { const f = window.infekt.bk.saved.at(-1); return { name: f.name, p: window.BK.parseFile(f.bytes, f.name) }; });
  check(`the local bank saved: ${sv.name}, no longer marked`, sv.name === "mybank.syx" && sv.p.singles.length === 25 && sv.p.ext === 1 &&
        !/●/.test(await page.textContent("#org-lbank")));
  await page.click(row("L", 1)); await page.click(row("L", 2), { modifiers: ["Control"] }); await page.click("#org-lexp");
  sv = await ev(page, () => { const f = window.infekt.bk.saved.at(-1); return { name: f.name, n: window.BK.parseFile(f.bytes).singles.length }; });
  check(`the selection exported: ${sv.name}`, sv.n === 2 && /^mybank_002-003\.syx$/.test(sv.name));
  await page.click("#org-dbakall");
  check("all banks backed up as one file", await until(page, () => /InfeKt_Backup_ABCDEF\.syx/.test(window.infekt.bk.saved.at(-1).name)));
  sv = await ev(page, () => { const f = window.infekt.bk.saved.at(-1); const p = window.BK.parseFile(f.bytes); return { n: p.singles.length, banks: [...new Set(p.singles.map((s) => s.bank))].join(), ext: p.ext, bytes: Array.from(f.bytes) }; });
  check(`the backup: every patch of A and B with its bank byte (${sv.banks}, ${sv.n}), the extensions`, sv.n === 11 && sv.banks === "3,4" && sv.ext === 2);
  await page.setInputFiles("#org-lfile", [{ name: "InfeKt_Backup_ABCDEF.syx", mimeType: "application/octet-stream", buffer: Buffer.from(sv.bytes) }]);
  check("the backup opened again: a local bank per device bank", await until(page, () => window.infekt.org.LB.banks.length === 3) &&
        await ev(page, () => { const b = window.infekt.org.LB.banks; return b[1].slots.filter((x) => x).length === 9 && b[2].slots.filter((x) => x).length === 2 && b[2].slots[127]; }));
  await page.click("#org-dtabs button[data-b='1']");
  await page.click("#org-dbak");
  check("bank B backed up", await until(page, () => window.infekt.bk.saved.at(-1).name === "InfeKt_Bank_B.syx"));
  // ---- the edit buffer to local, a new bank, and the phone layout
  await page.click("#org-lnew");
  await page.click("#org-editget");
  check("the edit buffer into a new local bank", await until(page, () => (window.infekt.org.LB.banks[3].slots[0] || {}).name === "EDITBUF"));
  // ---- "Alle wählen" / "Keine" on both lists: every filled slot of the bank shown (not the empty ones, whether shown or
  // not), the count, the actions and undo as with a choice made by hand
  await page.selectOption("#org-lbank", "0");
  const filledL = await ev(page, () => window.infekt.org.LB.banks[window.infekt.org.LB.cur].slots.filter((x) => x).length);
  for (const empty of [true, false]) {
    if ((await page.isChecked("#org-lempty")) !== empty) await page.click("#org-lempty");
    await page.click("#org-lnone");
    await page.click("#org-lall");
    const st = await ev(page, () => ({ rows: document.querySelectorAll("#org-llist .orow.sel").length,
      emptySel: document.querySelectorAll("#org-llist .orow.sel.empty").length,
      boxes: document.querySelectorAll("#org-llist .orow input:checked").length, count: document.getElementById("org-lcount").textContent }));
    check(`local "Alle wählen" (empty slots ${empty ? "shown" : "hidden"}): the ${filledL} filled ones, no empty one (${st.count})`,
          st.rows === filledL && st.boxes === filledL && st.emptySel === 0 && new RegExp(`${filledL} gewählt`).test(st.count));
  }
  check("its labels as the files list's", (await page.textContent("#org-lall")) === (await page.textContent("#b-all")) &&
        (await page.textContent("#org-lnone")) === (await page.textContent("#b-none")) &&
        (await page.textContent("#org-dall")) === (await page.textContent("#b-all")));
  await page.click("#org-lexp");
  sv = await ev(page, () => { const f = window.infekt.bk.saved.at(-1); return { n: window.BK.parseFile(f.bytes).singles.length }; });
  check(`"Alle wählen", then export selection: all ${filledL} filled`, sv.n === filledL);
  await page.click("#org-lnone");
  check("local \"Keine\": nothing chosen, the count 0, export selection off", / 0 gewählt/.test(await page.textContent("#org-lcount")) &&
        await page.isDisabled("#org-lexp") && !(await page.$("#org-llist .orow.sel")));
  // the FM-1's list: all of bank A, copied into a new local bank, undone
  await page.click("#org-dtabs button[data-b='0']");
  const filledD = await ev(page, () => document.querySelectorAll("#org-dlist .orow:not(.empty)").length);
  await page.click("#org-dall");
  check(`FM-1 "Alle wählen": the ${filledD} filled slots of bank A`, new RegExp(`${filledD} gewählt`).test(await page.textContent("#org-dcount")) &&
        (await page.$$("#org-dlist .orow.sel")).length === filledD && !(await page.$("#org-dlist .orow.sel.empty")) && !(await page.isDisabled("#org-toL")));
  await page.click("#org-lnew");
  const nb = await ev(page, () => window.infekt.org.LB.cur);
  await page.click("#org-toL");
  const cp = await until(page, () => window.infekt.org.LB.banks[window.infekt.org.LB.cur].slots.filter((x) => x).length > 0);
  const nCp = await ev(page, () => window.infekt.org.LB.banks[window.infekt.org.LB.cur].slots.filter((x) => x).length);
  check(`copied to local: all of them (${nCp} of ${filledD}; ${await page.textContent("#org-status")})`, cp && nCp === filledD);
  await page.click("#org-undo");
  check("undo: the copy taken back", await until(page, (k) => !window.infekt.org.LB.banks[k] || window.infekt.org.LB.banks[k].slots.every((x) => !x), nb));
  await page.click("#org-dnone");
  check("FM-1 \"Keine\": nothing chosen", / 0 gewählt/.test(await page.textContent("#org-dcount")) && await page.isDisabled("#org-toL"));
  check("no page errors", !errs.length);
  if (errs.length) console.log(errs.join("\n"));
  if (shots) {
    await page.selectOption("#org-lbank", "0");
    await page.click("#org-dtabs button[data-b='0']");
    await page.click(row("L", 1)); await page.click(row("D", 30)); await page.click("#org-toR");
    await page.setViewportSize({ width: 390, height: 844 });
    await page.waitForTimeout(200);
    await page.screenshot({ path: shots + "/organizer-phone.png", fullPage: true });
    await page.evaluate(() => document.getElementById("org-colD").scrollIntoView());
    await page.screenshot({ path: shots + "/organizer-phone-device.png" });
    const w = await page.evaluate(() => document.documentElement.scrollWidth);
    check(`phone width: no horizontal scroll (${w})`, w <= 390);
  }
  await browser.close();
}

// ======================= version 3: no names, no clearing =======================
{
  const { browser, page, errs } = await run(3);
  await page.click("#d-connect");
  await until(page, () => window.infekt.DEV.state === "ok" && window.infekt.bk.link);
  await page.click("#org-open");
  await page.waitForTimeout(300);
  check("version 3: bank A read patch by patch at the opening (128 dumps, no names asked)", await until(page, () => window.infekt.org.DB.banks[0].read && !window.infekt.org.busy, null, 20000) &&
        await ev(page, () => window.__fake.dumps === 128 && window.__fake.names === 0));
  check("version 3: the button reads the bank shown", /Bank A lesen/.test(await page.textContent("#org-dread")));
  await page.click(row("D", 0));
  check("clearing on the FM-1 and moving from it: off", await page.isDisabled("#org-clear") && await page.isDisabled("#org-mvL"));
  await page.click("#org-toL");
  check("copy to local works (bytes known already: no dump)", await ev(page, () => window.infekt.org.LB.banks[0].slots[0].name === "DEV 000" && window.__fake.dumps === 128));
  check("no page errors (version 3)", !errs.length);
  if (errs.length) console.log(errs.join("\n"));
  await browser.close();
}
console.log(bad ? `${bad} FAILED` : "organizer: all ok");
process.exit(bad ? 1 : 0);
