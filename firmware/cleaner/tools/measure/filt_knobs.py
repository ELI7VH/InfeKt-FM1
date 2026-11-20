#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""filt_knobs.py ENGINE [--env K=V ...] [--wav DIR]: GLO > AUDIO > FILT KNOBS (synth.c flt_glide), measured on the
host engine with the project's own init patch (tools/perf/hostcases.py BASE), ORIGINAL against SMOOTH
(SY_FLT_SMOOTH=0 / 1; --env adds a measurement build's switches to both).

Zipper: a held note, its cutoff 1 (CC 40; cutoff 2 follows through CUT LINK) or resonance 1 (CC 42) moved in single
steps (cutoff 50 -> 100, 378 Hz -> 5.7 kHz; resonance 40 -> 100) at a speed (blocks a move, steps a move: the knob's
slow detents, a controller's stream, the knob's fast turn with several steps a detent):
  tr    a sine at key 33 (55 Hz), resonance 100 (the resonance sweep: cutoff 70): what the moves leave above 150 Hz
        (an 8th-order high pass: the ringing and clicks of the steps; a smooth sweep leaves little), dB re the sine
Latency: a jump of 20 steps (cutoff 40 -> 60, resonance 0) after a pause on a sine at key 84 (1047 Hz, on the slope):
its level per 1/4 ms turned into a cutoff by a static calibration (cutoffs 38..62 held); the time from the jump's
block (the engine's own queue latency left out: the same in both modes) until 90 % of the 20 steps are there."""
import argparse
import os
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "perf"))
import hostcases  # noqa: E402

FS = 46875.0
BLK = 128
LAT = 2                                 # (the engine's queue latency in blocks, SY_OPT_LATENCY: the same in both modes)


def patch(sets):
    p = list(hostcases.BASE)
    for k, v in sets.items():
        p[k] = v
    return bytes(p)


def render(eng, sets, ev, env, keep=None):
    with tempfile.TemporaryDirectory() as d:
        pf, wf = os.path.join(d, "p.raw"), os.path.join(d, "o.wav")
        open(pf, "wb").write(patch(sets))
        r = subprocess.run([eng, wf, "preset", pf] + ev, env=dict(os.environ, **env), capture_output=True)
        if r.returncode:
            sys.exit("render: %s" % r.stderr.decode()[-300:])
        if keep:
            Path(keep).write_bytes(Path(wf).read_bytes())
        w = wave.open(wf)
        b = np.frombuffer(w.readframes(w.getnframes()), dtype=np.uint8).reshape(-1, 3).astype(np.int32)
    v = b[:, 0] | b[:, 1] << 8 | b[:, 2] << 16
    x = (((v << 8) >> 8).astype(np.float64) / 8388608.0).reshape(-1, 2)
    return x[:, 0]


def sweep_ev(key, cc, v0, v1, every, step, pre=60, post=60):
    ev = ["midi", "0xB0", str(cc), str(v0), "on", str(key), "100", "run", str(pre)]
    v, n = v0, pre
    while v != v1:
        v = min(v1, v + step)
        ev += ["midi", "0xB0", str(cc), str(v), "run", str(every)]
        n += every
    ev += ["run", str(post)]
    return ev, pre * BLK, (n + LAT) * BLK


def transient(x, a, b):
    """what is left above 150 Hz over x[a:b], dB re all of x[a:b]"""
    from scipy.signal import butter, sosfiltfilt
    y = sosfiltfilt(butter(8, 150, "highpass", fs=FS, output="sos"), x)
    return 10 * np.log10(np.sum(y[a:b] ** 2) / np.sum(x[a:b] ** 2) + 1e-30)


def level(x, f0):
    """the sine's level per sample: |x| smoothed over one period, twice"""
    n = max(4, int(round(FS / f0)))
    k = np.ones(n) / n
    return np.convolve(np.convolve(np.abs(x), k, "same"), k, "same")


SPEEDS = [("knob 1/123b", 123, 1), ("ctl 1/37b", 37, 1), ("ctl 1/7b", 7, 1), ("ctl 1/2b", 2, 1), ("ctl 1/1b", 1, 1),
          ("knob 4/37b", 37, 4), ("knob 8/12b", 12, 8)]
# (the knob: app.c knob_steps, 20 detents a turn: slow one value a detent at 3 detents/s and less; ~10 detents/s 4
# values a detent; 30 detents/s 8.5 values a detent. A controller: a value a message, 10 .. 366 messages/s)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("engine")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--wav", default=None, help="keep the renders here (ORIGINAL / SMOOTH)")
    ap.add_argument("--quick", action="store_true", help="the cutoff sweeps alone")
    a = ap.parse_args()
    base = dict(e.split("=", 1) for e in a.env)
    modes = [("ORIGINAL", dict(base, SY_FLT_SMOOTH="0")), ("SMOOTH", dict(base, SY_FLT_SMOOTH="1"))]
    print("zipper: dB re the sine (lower: fewer step transients)")
    print("%-22s %-12s %9s %9s %7s" % ("signal", "speed", "ORIGINAL", "SMOOTH", "delta"))
    sigs = [("cutoff tr", {17: 0, 42: 100}, 33, 40, 50, 100, transient),
            ("reso tr", {17: 0, 40: 70}, 33, 42, 40, 100, transient)]
    for name, sets, key, cc, v0, v1, fn in sigs:
        if a.quick and cc == 42:
            continue
        for sp, every, step in SPEEDS:
            r = []
            for m, env in modes:
                ev, s0, s1 = sweep_ev(key, cc, v0, v1, every, step)
                keep = None
                if a.wav and sp in ("knob 1/123b", "knob 8/12b"):
                    keep = os.path.join(a.wav, "%s_%s_%s.wav" % (name.split()[0], sp.replace(" ", "").replace("/", "-"), m))
                x = render(a.engine, sets, ev, env, keep)
                r.append(fn(x, s0, s1))
            print("%-22s %-12s %9.1f %9.1f %7.1f" % (name, sp, r[0], r[1], r[1] - r[0]))
    # latency: a 20-step jump after a pause
    key, f0 = 84, 1046.5
    cal = {}
    for c in range(38, 63):
        x = render(a.engine, {17: 0, 40: c}, ["on", str(key), "100", "run", "60"], modes[0][1])
        cal[c] = np.median(level(x[40 * BLK:], f0))
    cs = np.array(sorted(cal))
    ls = np.array([cal[c] for c in cs])
    if not np.all(np.diff(ls) > 0):
        print("latency: the calibration is not monotonic", ls)
        return
    print("latency (cutoff 40 -> 60 after a pause, sine key 84): ms from the jump's block to 10 / 50 / 90 % of it")
    for (m, env), tag in zip(modes, ("ORIGINAL", "SMOOTH")):
        jb = 60
        x = render(a.engine, {17: 0, 40: 40}, ["on", str(key), "100", "run", str(jb), "midi", "0xB0", "40", "60",
                                                "run", "40"], env)
        c = np.interp(level(x, f0), ls, cs)
        t0 = (jb + LAT) * BLK
        out = []
        for q in (0.1, 0.5, 0.9):
            i = t0 - 4 * BLK + np.argmax(c[t0 - 4 * BLK:] >= 40 + 20 * q)
            out.append((i - t0) / FS * 1000)
        print("  %-9s %6.1f %6.1f %6.1f" % (tag, *out))


if __name__ == "__main__":
    main()
