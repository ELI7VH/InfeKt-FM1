// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 J0KERPACK
// Node test of the InfeKt-FM1 page's patch core (infekt-fm1.html's script "bankcore", no DOM); a port of
// tools/bank_upload/test_core.mjs:
//   node test_bank_core.mjs BANK.syx BANK.mid   (as made by make_syx.py from raw patches; both hold the same singles)
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

const here = fileURLToPath(new URL(".", import.meta.url));
const html = readFileSync(here + "infekt-fm1.html", "utf8");
const src = html.match(/<script id="bankcore">([\s\S]*?)<\/script>/)[1];
const ctx = { module: { exports: {} } };
vm.runInNewContext(src, ctx);
const C = ctx.module.exports;
let bad = 0;
const check = (what, ok) => { console.log((ok ? "ok   " : "FAIL ") + what); if (!ok) bad++; };

const [syxPath, midPath] = process.argv.slice(2);
const syx = new Uint8Array(readFileSync(syxPath)), mid = new Uint8Array(readFileSync(midPath));
const a = C.parseFile(syx, "a.syx"), b = C.parseFile(mid, "b.mid");
check(`.syx: ${a.singles.length} singles, all checksums ok`, a.singles.length > 0 && a.singles.every((s) => s.ok) && !a.other);
check(`.mid: the same ${b.singles.length} singles`, b.singles.length === a.singles.length &&
      b.singles.every((s, k) => s.prog === a.singles[k].prog && s.name === a.singles[k].name &&
                       s.data.every((v, i) => v === a.singles[k].data[i])));
check(`names: ${a.singles.slice(0, 3).map((s) => s.name).join(", ")} ..`, a.singles.every((s) => s.name.length > 0));
check("program numbers in order", a.singles.every((s, k) => s.prog === k));
// built again with another number: the same bytes but pp and cs
const re = C.buildSingle(a.singles[5].data, 77, 1, 0);
check("buildSingle: a single, checksum right", C.isSingle(re) && C.sxSum(re) === re[265] && re[8] === 77);
check("buildSingle at the same number = the file's bytes", C.buildSingle(a.singles[5].data, 5).every((v, i) => v === syx[5 * 267 + i]));
// a bad checksum flagged, other SysEx and channel messages left out
const junk = Uint8Array.from([0x90, 60, 100, 0xF0, 0x7D, 0x42, 0x55, 0x41, 1, 0xF7, 0xF8, ...syx.subarray(0, 267)]);
junk[11 + 265] ^= 1;
const j = C.parseFile(junk, "junk.syx");
check("a differing checksum marked but usable, other SysEx counted apart", j.singles.length === 1 && j.singles[0].ok && j.singles[0].sumbad && j.other === 1);
// a data byte with bit 7 set is not usable
const hi = Uint8Array.from(syx.subarray(0, 267)); hi[20] = 0x80;
check("a data byte >= 0x80: not usable", !C.parseFile(hi).singles.length || !C.parseFile(hi).singles[0].ok);
// a multi dump (0x11 frame + parts): each sound once
const mf = Uint8Array.from(syx.subarray(0, 267)); mf[6] = 0x11;
const md = C.parseFile(Uint8Array.from([...mf, ...syx.subarray(0, 267), ...syx.subarray(0, 267), ...syx.subarray(267, 534)]));
check("a multi dump: each part's sound once", md.multi && md.singles.length === 2 && md.other === 1);
// realtime bytes inside a dump are dropped
const rt = Array.from(syx.subarray(0, 267)); rt.splice(100, 0, 0xF8, 0xFE);
check("realtime bytes inside a dump", C.parseFile(Uint8Array.from(rt)).singles[0]?.ok === true);
// raw patches (256 bytes each)
const raw = new Uint8Array(512); raw.set(a.singles[0].data); raw.set(a.singles[1].data, 256);
const r = C.parseFile(raw, "two.raw");
check("raw 256-byte patches", r.singles.length === 2 && r.singles[1].name === a.singles[1].name && r.singles[1].ok);
// a .mid with a dump split into an F0 event and an F7 continuation, and running status + a meta event
const d = syx.subarray(0, 267);
const vlq = (n) => (n < 128 ? [n] : [0x80 | (n >> 7), n & 0x7F]);
const trk = [0, 0xFF, 0x03, 4, 0x54, 0x65, 0x73, 0x74, 0, 0x90, 60, 100, 10, 62, 100,
             0, 0xF0, ...vlq(100), ...d.subarray(1, 101), 5, 0xF7, ...vlq(166), ...d.subarray(101), 0, 0xFF, 0x2F, 0];
const smf = Uint8Array.from([0x4D, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 0, 0, 1, 0, 96, 0x4D, 0x54, 0x72, 0x6B,
                             0, 0, trk.length >> 8, trk.length & 255, ...trk]);
const m = C.parseFile(smf, "split.mid");
check("a .mid: split dump joined, running status, meta skipped", m.singles.length === 1 && m.singles[0].ok && m.singles[0].name === a.singles[0].name);
// slots
check("plan: keep / one after another", C.plan(a.singles.slice(3, 6), "keep", 0).join() === "3,4,5" &&
      C.plan(a.singles.slice(3, 6), "seq", 10).join() === "10,11,12" && C.slotName(1, 0) === "B001" && C.slotName(2, 127) === "C128");
check("handshake frames", C.buMsg(2, 1, 0, 20).join() === "240,125,66,85,2,1,0,20,247" &&
      JSON.stringify(C.buReply(Uint8Array.from([0xF0, 0x7D, 0x42, 0x55, 0x50, 3, 0, 2, 0xF7]))) === '{"cmd":80,"args":[3,0,2]}' &&
      C.buReply(Uint8Array.from([0xF0, 0x7E, 0x42, 0x55, 0x50, 0xF7])) === null);
// 512-byte singles of the bigger successor model (made here from a patch of the file: block 1 = it, block 2 set by hand)
function big(d1, b2set, pp = 9) {
  const f = new Uint8Array(524), b2 = new Uint8Array(256);
  for (const [i, v] of Object.entries(b2set)) b2[i] = v;
  f.set([0xF0, 0, 0x20, 0x33, 0x01, 0, 0x10, 1, pp]); f.set(d1, 9); f.set(b2, 266); f[523] = 0xF7;
  let s = 0; for (let i = 5; i < 265; i++) s += f[i];
  f[265] = s & 0x7F;
  return f;
}
const base = Uint8Array.from(a.singles[0].data);
base[0] = 9; base[51] = 4; base[112] = 2; base[113] = 70;
base[128 + 51] = 99;                     // (an unread byte the bigger model fills)
const bf = C.parseFile(Uint8Array.from([...big(base, { 1: 1, 2: 90, 3: 3, 30: 1 }), ...syx.subarray(0, 267)]), "big.syx");
const bs = bf.singles[0];
check("a big single found and converted, beside an ordinary one", bf.singles.length === 2 && bf.big === 1 && bs.conv && bs.ok &&
      !bf.singles[1].conv && bs.prog === 9 && bs.name === a.singles[0].name && bs.orig[0] === 9);
check("block 1 as the reference plays it: version 7, filter mode 4 and reverb mode 2 kept, an unread byte reset",
      bs.data[0] === 7 && bs.data[51] === 4 && bs.data[112] === 2 && bs.data[113] === 70 && bs.data[128 + 51] === 0 &&
      bs.notes.some((x) => /Block 2 nicht gespielt.*HyperSaw.*Hall/.test(x)));
// (the converter's own cases: test_bigconv.mjs)
check("Python's rounding (half to even)", C.pyRound(2.5) === 2 && C.pyRound(3.5) === 4 && C.pyRound(-0.5) === 0 && C.pyRound(1.25) === 1);
check("preview notes: C3 = 48, a chord a major triad", C.noteName(48) === "C3" && C.noteName(60) === "C4" &&
      C.previewNotes("chord", 48).join() === "48,52,55" && C.previewNotes("note", 48).join() === "48" && !C.previewNotes("off", 48).length &&
      C.chMsg(0xB0, 0, 123, 0).join() === "176,123,0");
// ---- the export (1.05): the extension frame, the compatible single, the files ----
const X = Uint8Array.from([64, 2, 90, 70, 0, 100, 1, 20, 2, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 12]);   // (1.18C47: WIDTH 100, W GLIDE 12)
const xf = C.buildExt(X, 9, 4);
check("extension frame: F0 7D 49 58 01 bb pp 21 .. cs F7, read back", xf.length === 31 && [...xf.subarray(0, 8)].join() === "240,125,73,88,1,4,9,21" &&
      xf[30] === 0xF7 && C.parseExt(xf).ok && C.parseExt(xf).prog === 9 && C.parseExt(xf).bank === 4 && C.parseExt(xf).x.join() === X.join());
const xb = Uint8Array.from(xf); xb[10] ^= 1;
check("a bad checksum: not ok; a single is no extension frame", !C.parseExt(xb).ok && C.parseExt(syx.subarray(0, 267)) === null);
const short = C.buildExt(X.subarray(0, 8), 1);
check("a shorter frame (values alone): the targets none, W GLIDE its default", C.parseExt(short).ok && C.parseExt(short).x.subarray(8, 20).every((v) => v === 0) &&
      C.parseExt(short).x[20] === 5);
const x20 = C.parseExt(C.buildExt(X.subarray(0, 20), 1));
check("a frame of 1.18C46 (20 bytes): its values and targets, W GLIDE 5", x20.ok && x20.x.length === 21 && x20.x.subarray(0, 20).join() === X.subarray(0, 20).join() &&
      x20.x[20] === 5 && C.isDefaultExt(C.EXT.DEF.slice(0, 20)) && !C.isDefaultExt(Uint8Array.from([...C.EXT.DEF.slice(0, 20), 6])));
check("the defaults are no extension", C.isDefaultExt(C.EXT.DEF) && C.isDefaultExt(null) && !C.isDefaultExt(X));
const pd = Uint8Array.from(a.singles[0].data);
pd[193] = 0; pd[207] = 0; pd[139] = 3; pd[194] = 100; pd[208] = 20; pd[140] = 90;
const fr = C.exportFrames([{ data: pd, x: X, prog: 3, bank: 3 }, { data: a.singles[1].data, x: null, prog: 4, bank: 3 }]);
check("InfeKt export: single, its extension frame, the next single", fr.length === 3 && C.isSingle(fr[0]) && C.parseExt(fr[1]).prog === 3 && C.isSingle(fr[2]));
const rx = C.parseFile(C.syxOf(fr), "x.syx");
check("read back: the extension with its single, not counted as other", rx.singles.length === 2 && rx.ext === 1 && rx.other === 0 &&
      rx.singles[0].x.join() === X.join() && rx.singles[1].x === null && rx.singles[0].data.every((v, i) => v === pd[i]));
const rm = C.parseFile(C.midOf(fr), "x.mid");
check(".mid: the same", rm.singles.length === 2 && rm.ext === 1 && rm.singles[0].x.join() === X.join() && rm.singles[1].data.every((v, i) => v === a.singles[1].data[i]));
const fc = C.exportFrames([{ data: pd, x: X, prog: 3, bank: 3 }], { compat: true });
const cd = fc[0].subarray(9, 265);
check("compatible: no extension frame; slot 1 (B 65) amount none, LFO 1's assign (B 79) amount none, LFO 3 (B 11) amount 0",
      fc.length === 1 && cd[193] === 0 && cd[194] === 64 && cd[207] === 0 && cd[208] === 64 && cd[139] === 3 && cd[140] === 0 &&
      cd.every((v, i) => [194, 208, 140].includes(i) || v === pd[i]) && C.sxSum(fc[0]) === fc[0][265]);
check("compatible without an extension: the bytes as they are", C.compatData(pd, null).notes.length === 0);
check("slots typed", C.parseSlots("1-3, 7").join() === "0,1,2,6" && C.parseSlots("128").join() === "127" && C.parseSlots("0") === null &&
      C.parseSlots("5-2") === null && C.parseSlots("x") === null && C.parseSlots("1-128").length === 128);
check("bank bytes and letters", C.bankByte(0x10) === 1 && C.bankByte(0x11) === 2 && C.bankByte(0) === 3 && C.bankByte(5) === 8 &&
      C.bankByte(127) === 0 && C.dumpLetter(2) === "C" && C.dumpLetter(5) === "F" && C.dumpLetter(0x10) === "?" &&
      C.slotName(5, 127) === "F128" && C.fileSafe("Pad / 2!") === "Pad_2");
check("six user banks from the hello's version 3 (1.18C30), three before", C.banksOf(3) === 6 && C.banksOf(2) === 3 &&
      C.banksOf(1) === 3 && C.banksOf(null) === 6 && C.BU.BANKS.length === 6);
console.log(bad ? `${bad} FAILED` : "all ok");
process.exit(bad ? 1 : 0);
