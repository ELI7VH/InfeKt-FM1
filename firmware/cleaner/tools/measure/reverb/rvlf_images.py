#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""rvlf_images.py ENGINE [--max-db 6] [--fs 44100,46875] [--env K=V,...]: the reverb LF path's interpolation images
(sy_cln_rv.c RVLF_SPLINE) measured on a patch of the project's own.

The INIT patch (fm1/init_patch.h) with osc 1 a sine (no harmonics: every tone above the note is an artefact), a key
below the LF path's 220 Hz split (55: 196 Hz) played and released, the reverb (mode 2, send 80, decay 96) in FULL and
HALF. In the tail after the note's release (the dry voice gone) the spectrum around k fs/16 +- f0 (k = 1..3: the LF
path's points come at fs/16) is looked at: the image's level re the note (f0), and its prominence, the mean power
density within 12 Hz of it against the median density 60..400 Hz away on both sides (a line stands out, the
reverb's own diffuse floor does not). Straight lines between the points (up to 1.18C37) leave lines 20-35 dB above
the floor at -45..-50 dB re f0; the cubic B-spline none. Exit status 1 when any prominence is above --max-db,
2 on a render error."""
import argparse
import os
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

import numpy as np
from scipy.signal import welch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "perf"))
import hostcases  # noqa: E402

KEY = 55


def patch():
    c = hostcases.Case("rvlf_img", "", {"OSC1_SHAPE": 0, "OSC1_WAVE": 0, "AE_S": 127, "AE_R": 40, "FX_MODE": 2,
                                        "FX_SEND": 80, "FX_TIME": 8, "FX_DECAY": 96, "FX_DEPTH": 3, "FX_COLOUR": 64},
                       [KEY])
    return c.patch()


def read24(path):
    w = wave.open(path)
    b = np.frombuffer(w.readframes(w.getnframes()), dtype=np.uint8).reshape(-1, 3).astype(np.int32)
    v = (b[:, 0] | b[:, 1] << 8 | b[:, 2] << 16) << 8 >> 8
    return v.reshape(-1, 2) / 8388608.0


def measure(engine, fs, env, tmp):
    pf = os.path.join(tmp, "p.raw")
    Path(pf).write_bytes(patch())
    wav = os.path.join(tmp, "o.wav")
    nb = lambda s: str(int(s * fs / 128))
    args = [engine, wav, "preset", pf, "run", "4", "on", str(KEY), "100", "run", nb(0.6), "off", str(KEY),
            "run", nb(4.0)]
    r = subprocess.run(args, env=dict(os.environ, SY_FS=str(fs), **env), capture_output=True)
    if r.returncode:
        print("render error:", r.stderr.decode()[-300:])
        sys.exit(2)
    x = read24(wav)[int(1.4 * fs):int(3.4 * fs), 0]          # (the tail: the dry voice's release over)
    fr, p = welch(x, fs, nperseg=16384)
    f0 = 440.0 * 2 ** ((KEY - 69) / 12)
    band = lambda f: p[(fr > f - 12) & (fr < f + 12)]
    ref = band(f0).sum()
    out = []
    for f in (k * fs / 16 + s * f0 for k in (1, 2, 3) for s in (-1, 1)):
        d = np.abs(fr - f)
        floor = np.median(p[(d > 60) & (d < 400)])
        out.append((f, 10 * np.log10(band(f).sum() / ref + 1e-30), 10 * np.log10(band(f).mean() / floor + 1e-30)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("engine")
    ap.add_argument("--max-db", type=float, default=6.0, help="the largest prominence allowed (dB over the floor)")
    ap.add_argument("--fs", default="44100,46875")
    ap.add_argument("--env", default="", help="more environment for the renders (e.g. SY_NOLEND=1 for the 16-bit ring)")
    a = ap.parse_args()
    extra = dict(kv.split("=", 1) for kv in a.env.split(",") if kv)
    worst = -999.0
    with tempfile.TemporaryDirectory() as tmp:
        for fs in (int(v) for v in a.fs.split(",")):
            for name, rvq in (("FULL", "1"), ("HALF", "0")):
                im = measure(os.path.abspath(a.engine), fs, dict(extra, SY_RVQ=rvq), tmp)
                w = max(i[2] for i in im)
                worst = max(worst, w)
                print("fs %5d %s: (Hz, dB re f0, dB over the floor) %s" % (fs, name, "  ".join("%.0f %.1f %+.1f" % i for i in im)))
    ok = worst <= a.max_db
    print("%s: the most prominent image %+.1f dB over the floor (limit %+.1f)" % ("pass" if ok else "FAIL", worst, a.max_db))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
