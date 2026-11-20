#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""nulltest.py OLD_ENGINE NEW_ENGINE [--max-db -100] [--cases a,b|main|bank|all|rv] [--jobs 4] [--quiet]

The null test of the CPU work (package M1): renders the case set of hostcases.py (the main cases and
the 32 test-bank patches, read from $TESTBANK) with both host engines on one note sequence (a single note, its release,
then an 8-note chord held and released; REVERB HALF where the case says so) and prints, per case, the peak and rms of
the difference in dBFS ("bit-identical" when the two WAV files hold the same samples). Exit status 1 when any peak is
above --max-db (default -100 dBFS), 2 on a render error. The renders are deterministic (the engine's random phases and
noises are seeded; two runs of one binary are bit-identical), so a difference is the engine's.
Self-test: nulltest.py E E must say bit-identical everywhere."""
import argparse
import math
import os
import subprocess
import sys
import tempfile
import wave
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hostcases  # noqa: E402


def read24(path):
    w = wave.open(path)
    raw = w.readframes(w.getnframes())
    ch, sw = w.getnchannels(), w.getsampwidth()
    if sw != 3:
        raise ValueError("%s: expected 24-bit" % path)
    b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
    v = b[:, 0] | b[:, 1] << 8 | b[:, 2] << 16
    return (((v << 8) >> 8).astype(np.float64) / 8388608.0).reshape(-1, ch), raw


def render(engine, case, tmp, tag):
    pf = os.path.join(tmp, tag + ".raw")
    open(pf, "wb").write(case.patch())
    wav = os.path.join(tmp, tag + ".wav")
    env = dict(os.environ, **case.env)
    env.pop("INPUT", None)
    r = subprocess.run([engine, wav, "preset", pf] + hostcases.seq_events(case), env=env, capture_output=True)
    if r.returncode:
        raise RuntimeError("%s: %s rc %d %s" % (case.name, engine, r.returncode, r.stderr.decode()[-300:]))
    x, raw = read24(wav)
    os.unlink(wav)
    return x, raw


def db(x):
    return 20 * math.log10(x) if x > 0 else float("-inf")


def one(old, new, case, tmp):
    a, ra = render(old, case, tmp, case.name + "_o")
    b, rb = render(new, case, tmp, case.name + "_n")
    if a.shape != b.shape:
        return case.name, None, None, None, "length %d vs %d" % (len(a), len(b))
    d = a - b
    pk = float(np.abs(d).max())
    rms = float(np.sqrt((d ** 2).mean()))
    ref = float(np.sqrt((a ** 2).mean()))
    return case.name, pk, rms, ref, ("bit-identical" if ra == rb else "")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("old"); ap.add_argument("new")
    ap.add_argument("--max-db", type=float, default=-100.0)
    ap.add_argument("--cases", default="all"); ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--quiet", action="store_true", help="print only the cases that are not bit-identical, and the summary")
    a = ap.parse_args()
    allc = hostcases.all_cases(bank=True)
    rvc = hostcases.rv_cases()
    if a.cases == "all":
        cs = allc
    elif a.cases == "rv":
        cs = rvc
    elif a.cases == "main":
        cs = [c for c in allc if not c.name.startswith("bank_")]
    elif a.cases == "bank":
        cs = [c for c in allc if c.name.startswith("bank_")]
    else:
        cs = [c for c in allc + rvc if c.name in a.cases.split(",")]
    if not any(c.name.startswith("bank_") for c in cs) and a.cases in ("all", "bank"):
        print("note: no test bank found at %s (set TESTBANK)" % hostcases.TESTBANK, file=sys.stderr)
    tmp = tempfile.mkdtemp(prefix="nulltest", dir="/dev/shm" if os.path.isdir("/dev/shm") else None)
    try:
        with ThreadPoolExecutor(a.jobs) as ex:
            res = list(ex.map(lambda c: one(a.old, a.new, c, tmp), cs))
    except RuntimeError as e:
        print("render error:", e, file=sys.stderr)
        return 2
    finally:
        subprocess.run(["rm", "-rf", tmp])
    bad = ident = 0
    worst = float("-inf")
    print("%-22s %10s %10s %10s  %s" % ("case", "peak dBFS", "rms dBFS", "ref rms", ""))
    for name, pk, rms, ref, note in res:
        if pk is None:
            print("%-22s %s" % (name, note)); bad += 1; continue
        if note == "bit-identical":
            ident += 1
            if not a.quiet:
                print("%-22s %10s %10s %10.1f  bit-identical" % (name, "-inf", "-inf", db(ref)))
            continue
        worst = max(worst, db(pk))
        over = db(pk) > a.max_db
        bad += over
        print("%-22s %10.1f %10.1f %10.1f  %s" % (name, db(pk), db(rms), db(ref), "ABOVE %.0f" % a.max_db if over else ""))
    print("%d cases: %d bit-identical, %d differing (worst peak %s dBFS), %d above %.0f dBFS" % (
        len(res), ident, len(res) - ident, "%.1f" % worst if worst > float("-inf") else "-inf", bad, a.max_db))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
