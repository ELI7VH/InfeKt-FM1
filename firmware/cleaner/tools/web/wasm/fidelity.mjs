// SPDX-License-Identifier: GPL-3.0-only
// fidelity.mjs LAB_ENGINE WS.wasm [FS]: the web synth's module against the host lab engine built with the same engine
// switches (build: cc ... -DHYB_MULTI -DHYB_RV_LEND -DSY_MAX_VOICES=16
// -DSY_DELAY_FRAMES=32768 -DSY_UNI_CAP host/lab_engine.c + the engine). The same patches (the project's own init patch
// and variations of it made here: no bank data), the same notes, the same blocks: the lab's WAV (24 bits) against the
// module's float output put through the lab's own 24-bit rounding. Prints per case the samples that differ, the largest
// difference (LSB) and the difference's level against the signal (dB); exit 1 when a case is not bit-identical
// (FIDELITY_TOL_DB: a level below which a difference passes instead, e.g. -120).
import { readFileSync, writeFileSync, mkdtempSync, rmSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const [lab, wasmPath, fsArg] = process.argv.slice(2);
if (!lab || !wasmPath) { console.error("usage: fidelity.mjs LAB_ENGINE WS.wasm [FS]"); process.exit(2); }
const FS = +(fsArg || 44100);
const TOL = process.env.FIDELITY_TOL_DB ? +process.env.FIDELITY_TOL_DB : null;
const HERE = dirname(fileURLToPath(import.meta.url));
// the init patch: init_patch.h (tools/make_init.py's), as make_params.py hands it to the page
const ih = readFileSync(join(HERE, "../../../fm1/init_patch.h"), "utf8");
const INIT = Uint8Array.from(ih.slice(ih.indexOf("{") + 1, ih.indexOf("};")).match(/\d+/g).map(Number));
if (INIT.length !== 256) throw new Error("init patch: " + INIT.length);
const mk = (edits) => { const p = INIT.slice(); for (const [b, v] of Object.entries(edits)) p[+b] = v; return p; };
const rnd = (seed) => { let s = seed >>> 0; return () => ((s = (s * 1664525 + 1013904223) >>> 0) >>> 25); };
const randomPatch = (seed) => { const r = rnd(seed), p = new Uint8Array(256); for (let k = 0; k < 256; k++) p[k] = r(); return p; };

const chord = (on) => on.flatMap(([n, v]) => ["on", n, v]);
const offs = (on) => on.flatMap(([n]) => ["off", n]);
const C = [[48, 100], [55, 90], [60, 110], [64, 80]];
// cases: [name, patch, shimmer values (8) or null, commands after the preset]
const CASES = [
  ["init saw, chord", mk({}), null, [...chord(C), "run", 220, ...offs(C), "run", 120]],
  ["chorus + delay", mk({ 105: 80, 106: 40, 107: 70, 112: 1, 113: 70, 114: 50, 115: 70 }), null,
    [...chord(C), "run", 200, ...offs(C), "run", 300]],
  ["reverb 2, unison", mk({ 97: 5, 98: 60, 99: 100, 112: 2, 113: 90, 116: 100, 117: 1 }), null,
    [...chord(C), "run", 200, ...offs(C), "run", 300]],
  ["reverb 4 + shimmer", mk({ 112: 4, 113: 100, 116: 110 }), [100, 1, 100, 64, 0, 0, 0, 57],
    [...chord(C), "run", 200, ...offs(C), "run", 300]],
  ["reverb 3 + shimmer WIDTH 127, W GLIDE 0.5 s (1.18C47)", mk({ 112: 3, 113: 100, 116: 110, 117: 3 }),
    [100, 0, 100, 64, 0, 127, (0 ^ 5) << 1, 57],
    [...chord(C), "run", 150, ...offs(C), "run", 500, ...chord(C), "run", 100, ...offs(C), "run", 300]],
  ["dist, phaser, EQ, reso filter, LFO", mk({ 228: 3, 229: 90, 225: 40, 213: 70, 214: 50, 215: 80, 42: 110, 40: 50,
    207: 1, 208: 100, 67: 70, 223: 90, 224: 30 }), null, [...chord(C), "run", 220, ...offs(C), "run", 150]],
  ["FM, sync, osc 3, glide mono, bend", mk({ 27: 70, 28: 1, 169: 2, 170: 90, 94: 1, 5: 60, 22: 20, 24: 30 }), null,
    ["on", 40, 120, "run", 60, "midi", 0xE0, 0, 100, "run", 40, "on", 52, 100, "run", 80, "off", 40, "off", 52, "run", 120]],
  ["random bytes 1", randomPatch(11), null, [...chord(C), "run", 200, ...offs(C), "run", 200]],
  ["random bytes 2", randomPatch(23), null, [...chord(C), "run", 200, ...offs(C), "run", 200]],
  ["16 notes (voice stealing)", mk({ 112: 3, 113: 60, 63: 90 }), null,
    [...Array.from({ length: 16 }, (_, k) => ["on", 36 + 3 * k, 60 + 4 * k, "run", 6]).flat(), "run", 100,
     ...Array.from({ length: 16 }, (_, k) => ["off", 36 + 3 * k]).flat(), "run", 200]],
];

// a fresh instance a case, as the lab is a fresh process a render (the engine keeps state in statics that its init
// does not reset: the phase hand-over table, the reverb's state)
const MOD = await WebAssembly.compile(readFileSync(wasmPath));
let X = null;
const T = mkdtempSync(join(tmpdir(), "wsfid-"));
const f32 = Math.fround;
function put24(x) {                     // lab_engine.c put24: (long)(x * 8388608.f + (x >= 0 ? 0.5f : -0.5f)), clamped
  let v = Math.trunc(f32(f32(x * 8388608) + (x >= 0 ? 0.5 : -0.5)));
  return v > 8388607 ? 8388607 : v < -8388608 ? -8388608 : v;
}
async function runWasm(patch, shim, cmds) {
  X = (await WebAssembly.instantiate(MOD, {})).exports;
  X.ws_init_lab(FS);
  const mem = () => new Uint8Array(X.memory.buffer);
  mem().set(patch, X.ws_patch());
  X.ws_load_raw();
  if (shim) { mem().set(shim, X.ws_patch() + 256); X.ws_shim_raw(); }
  const out = [];
  for (let i = 0; i < cmds.length;) {
    const c = cmds[i++];
    if (c === "on") { X.ws_midi(0x90, cmds[i], cmds[i + 1]); i += 2; }
    else if (c === "off") { X.ws_midi(0x80, cmds[i], 0); i += 1; }
    else if (c === "midi") { X.ws_midi(cmds[i], cmds[i + 1], cmds[i + 2]); i += 3; }
    else if (c === "run") {
      const n = cmds[i++];
      for (let b = 0; b < n; b++) {
        X.ws_render();
        const B = X.ws_block();
        const l = new Float32Array(X.memory.buffer, X.ws_out_l(), B), r = new Float32Array(X.memory.buffer, X.ws_out_r(), B);
        for (let k = 0; k < B; k++) out.push(put24(l[k]), put24(r[k]));
      }
    } else throw new Error("command " + c);
  }
  return Int32Array.from(out);
}
function runLab(patch, shim, cmds) {
  const pf = join(T, "p.raw"), wf = join(T, "o.wav");
  writeFileSync(pf, patch);
  const env = { ...process.env, SY_FS: String(FS) };
  if (shim) env.SY_SHIM = shim.join(","); else delete env.SY_SHIM;
  execFileSync(lab, [wf, "preset", pf, ...cmds.map(String)], { env });
  const w = readFileSync(wf), n = (w.length - 44) / 3, o = new Int32Array(n);
  for (let k = 0; k < n; k++) { const v = w[44 + 3 * k] | w[45 + 3 * k] << 8 | w[46 + 3 * k] << 16; o[k] = v << 8 >> 8; }
  return o;
}
let bad = 0;
const rows = [];
for (const [name, patch, shim, cmds] of CASES) {
  const a = runLab(patch, shim, cmds), b = await runWasm(patch, shim, cmds);
  if (a.length !== b.length) { console.log(`FAIL ${name}: length ${a.length} / ${b.length}`); bad++; continue; }
  let diff = 0, maxd = 0, es = 0, ed = 0;
  for (let k = 0; k < a.length; k++) {
    const d = a[k] - b[k];
    if (d && !diff && process.env.FID_FIRST) console.log(name, 'first diff at frame', k >> 1, a[k], b[k]);
    if (d) { diff++; maxd = Math.max(maxd, Math.abs(d)); }
    es += a[k] * a[k]; ed += d * d;
  }
  const db = ed ? 10 * Math.log10(ed / Math.max(es, 1)) : -Infinity;
  let pk = 1;
  for (let k = 0; k < a.length; k++) pk = Math.max(pk, Math.abs(a[k]));
  const peak = 20 * Math.log10(pk / 8388608);
  const ok = diff === 0 || (TOL !== null && db < TOL);
  if (!ok) bad++;
  rows.push(`${ok ? "ok  " : "FAIL"} ${name.padEnd(36)} ${a.length / 2} frames, peak ${peak.toFixed(1)} dBFS: ` +
    (diff ? `${diff} samples differ, max ${maxd} LSB, difference ${db.toFixed(1)} dB re signal` : "bit-identical"));
}
rmSync(T, { recursive: true, force: true });
console.log(rows.join("\n"));
console.log(bad ? `fidelity: ${bad} of ${CASES.length} FAIL` : `fidelity: ${CASES.length} cases ok (fs ${FS})`);
process.exit(bad ? 1 : 0);
