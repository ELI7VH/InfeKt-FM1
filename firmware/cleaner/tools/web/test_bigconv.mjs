// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 J0KERPACK
// Node test of the converter for 512-byte singles of the bigger successor model (infekt-fm1.html's script "bankcore",
// bigConvert): a made-up 524-byte single (no third-party data). Block 1 is kept as the reference plays it; only values
// our engine does not take are replaced; block 2 is listed in the notes, not played.
//   node test_bigconv.mjs
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

const here = fileURLToPath(new URL(".", import.meta.url));
const html = readFileSync(here + "infekt-fm1.html", "utf8");
const ctx = { module: { exports: {} } };
vm.runInNewContext(html.match(/<script id="bankcore">([\s\S]*?)<\/script>/)[1], ctx);
const C = ctx.module.exports;
let bad = 0;
const check = (what, ok) => { console.log((ok ? "ok   " : "FAIL ") + what); if (!ok) bad++; };

// a synthetic single: block 1 a made-up patch, block 2 with an oscillator model, its reverb, an extra destination
const d1 = new Uint8Array(256).map((_, i) => (i * 11 + 5) % 100);
[..."SYNTH TEST"].forEach((c, i) => (d1[240 + i] = c.charCodeAt(0)));
Object.assign(d1, { 2: 0, [128 + 17]: 4, [128 + 19]: 6, [128 + 21]: 0, [128 + 28]: 1, [128 + 52]: 0, [128 + 53]: 64,
                    [128 + 62]: 0, [128 + 63]: 0, 250: 0 });   // (the other clocks legal, the other unread bytes reset)
Object.assign(d1, { 0: 8, 17: 127, 18: 70, 19: 90, 24: 40, 51: 6, 52: 5, 97: 1, 112: 2, 113: 88, 114: 64, 115: 63,
                    116: 16, 117: 12, 118: 1, [128 + 18]: 21, [128 + 20]: 5, [128 + 65]: 117, [128 + 99]: 0,
                    [128 + 51]: 77, 1: 127 });
const b2 = new Uint8Array(256);
Object.assign(b2, { 1: 1, 2: 76, 3: 3, 4: 92, 30: 1, 35: 2, 90: 9, 91: 100 });
const f = new Uint8Array(524);
f.set([0xF0, 0, 0x20, 0x33, 0x01, 0, 0x10, 1, 42]); f.set(d1, 9); f.set(b2, 266); f[523] = 0xF7;
let s = 0; for (let i = 5; i < 265; i++) s += f[i];
f[265] = s & 0x7F;

check("a 524-byte single is recognised", C.isBigSingle(f) && !C.isBigSingle(f.subarray(0, 523)));
const r = C.parseFile(f, "t.syx");
const p = r.singles[0], c = p.data;
check("parsed as one converted single: name, program, the original block 1 kept", r.singles.length === 1 && r.big === 1 &&
      p.conv && p.prog === 42 && p.name === "SYNTH TEST" && p.orig.every((v, i) => v === d1[i]));
check("version byte 7", c[0] === 7);
check("delay/reverb as block 1 has it (mode 2 = reverb, its bytes, the clock)",
      [112, 113, 114, 115, 116, 117, 118, 128 + 20].every((i) => c[i] === d1[i]));
check("oscillators, unison, filter modes, matrix destination >= 114 unchanged (the reference plays them so)",
      [17, 18, 22, 23, 97, 98, 51, 52, 128 + 65].every((i) => c[i] === d1[i]));
check("wave > 64 -> 0 (the reference plays wave 0), a wave <= 64 kept", c[19] === 0 && c[24] === 40);
check("an LFO clock > 19 -> 19", c[128 + 18] === 19);
check("the unread bytes reset (A 1, B 51)", c[1] === 0 && c[128 + 51] === 0);
const changed = [...c].map((v, i) => i).filter((i) => c[i] !== d1[i]);
check(`nothing else changed (${changed.join(",")})`, changed.join() === [0, 1, 19, 128 + 18, 128 + 51].join());
const nt = p.notes.join("\n");
check("notes: each replacement", /A19 90 → 0/.test(nt) && /B18 21 → 19/.test(nt) && /A1, B51/.test(nt));
check("notes: block 2 not played (models, its reverb, extra destination)",
      /Block 2 nicht gespielt.*Osc1 HyperSaw.*Osc2 WaveTable.*eigener Hall \(C1=1, Send 76\).*1 Matrix-Zusatzziel/.test(nt));
// a plain block 2 and legal values: nothing but the version byte
const d2 = Uint8Array.from(d1); Object.assign(d2, { 0: 7, 1: 0, 19: 10, [128 + 18]: 3, [128 + 51]: 0 });   // (no wave above 64, the clocks legal)
const q = C.bigConvert(d2, new Uint8Array(256));
check("legal values, empty block 2: the bytes as they are, no notes", q.data.every((v, i) => v === d2[i]) && q.notes.length === 0);
console.log(bad ? `${bad} FAILED` : "all ok");
process.exit(bad ? 1 : 0);
