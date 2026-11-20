// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 J0KERPACK
// Node test of the InfeKt-FM1 page's wave core (infekt-fm1.html's script "wavecore", no DOM) against vfwaves.py; a port
// of tools/waves/test_core.mjs:
//   node test_waves_core.mjs DIR    (DIR: ../waves/test_host.sh's output: its WAV files w/*.wav, up.syx, dump1.syx, dump2.syx)
// The same WAVs give the same messages byte for byte (or the amplitudes within a half float's last bit, the phases
// within one step); the host simulator's answers decode; a record round-trips; a malformed frame is refused.
import { readFileSync, readdirSync } from "node:fs";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

const here = fileURLToPath(new URL(".", import.meta.url));
const src = readFileSync(here + "infekt-fm1.html", "utf8").match(/<script id="wavecore">([\s\S]*?)<\/script>/)[1];
const ctx = { module: { exports: {} } };
vm.runInNewContext(src, ctx);
const C = ctx.module.exports;
let bad = 0;
const check = (what, ok) => { console.log((ok ? "  ok " : "FAIL ") + what); if (!ok) bad++; };
const dir = process.argv[2];
const frames = (b) => { const out = []; let cur = null; for (const v of b) { if (v === 0xF0) cur = [v]; else if (cur) { cur.push(v); if (v === 0xF7) { out.push(Uint8Array.from(cur)); cur = null; } } } return out; };

const py = frames(new Uint8Array(readFileSync(dir + "/up.syx")));
const files = readdirSync(dir + "/w").filter((f) => f.endsWith(".wav")).sort();
check(`${files.length} WAVs, ${py.length} messages from vfwaves.py`, files.length === py.length && files.length > 0);
files.forEach((f, k) => {
  const { x } = C.parseWav(new Uint8Array(readFileSync(dir + "/w/" + f))), { amp, phs } = C.harmonics(x), so = C.slotOf(f);
  const js = C.msgWrite(so.slot, C.record(amp, phs, so.name)), p = py[k];
  if (js.length === p.length && js.every((v, i) => v === p[i])) { check(`${f}: the same message, byte for byte`, true); return; }
  // (a float's last bits may round otherwise: the records compared value by value)
  const a = C.parseRecord(C.unpack7(js.subarray(6, js.length - 4), C.W.REC)), b = C.parseRecord(C.unpack7(p.subarray(6, p.length - 4), C.W.REC));
  let da = 0, dp = 0;
  for (let n = 0; n < 64; n++) { da = Math.max(da, Math.abs(a.amp[n] - b.amp[n]) / Math.max(b.amp[n], 1e-6)); if (b.amp[n] > 1e-4) dp = Math.max(dp, Math.abs(((a.phs[n] - b.phs[n] + 1.5) % 1) - 0.5)); }
  check(`${f}: the same wave (amplitudes within ${(da * 100).toFixed(3)} %, phases within ${(dp * 65536).toFixed(1)} / 65536)`,
        a.name === b.name && da < 0.002 && dp * 65536 <= 1.5);
});
for (const n of ["dump1.syx", "dump2.syx"]) {
  const rs = frames(new Uint8Array(readFileSync(dir + "/" + n))).map((f) => C.parseReply(f));
  check(`${n}: ${rs.length} dumps from the host simulator, all checksums right`, rs.length > 0 && rs.every((r) => r.kind === "dump" && r.ok));
  const r = rs[0], { y } = C.engineWave(r.rec);
  check(`${n}: slot ${r.slot} as a wave (peak ${Math.max(...y.map(Math.abs)).toFixed(3)})`, y.length === 2048 && Math.max(...y.map(Math.abs)) > 0.2);
}
const rec = C.record([1, 0.5, 0.25, ...Array(61).fill(0)], [0, 0.25, 0.5, ...Array(61).fill(0)], "téstéok", -3);
const back = C.parseRecord(rec);
check("a record round-trips (name cleaned, trim -3 dB)", back.name === "t_st_ok" && back.trim === -3 && back.amp[1] === 0.5 && back.phs[2] === 0.5);
const m = C.msgWrite(12, rec);
check("a write message: 317 bytes, F0 7D 56 46 10 0C .. F7", m.length === 317 && m[4] === 0x10 && m[5] === 12 && m.at(-1) === 0xF7 &&
      m.every((v, i) => i === 0 || i === m.length - 1 || v < 128));
check("a reply that is not ours: null", C.parseReply(Uint8Array.from([0xF0, 0x7D, 0x42, 0x55, 0x41, 1, 0xF7])) === null);
const w = C.wavFloat([0, 0.5, -0.5, 1]), back2 = C.parseWav(w);
check("a float WAV written and read", back2.rate === 46875 && back2.x.length === 4 && back2.x[3] === 1);
console.log(bad ? `${bad} FAILED` : "all ok");
process.exit(bad ? 1 : 0);
