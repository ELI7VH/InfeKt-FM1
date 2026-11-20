#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
"""drjit_sweep.py HOST [N] [edit]: the drums' timing at pattern changes (after 0.99), measured by the host
simulator (mk.sh) over N changes (default 200) at random times for each tempo, set of patterns and main-loop load:
busy 0 none (the host's 16 ms frame alone), 1 the main loop held 5-80 ms now and then (a frame, the screen, the store's
reads), 2 also 100-300 ms (a flash erase). Each pattern: time division, LENGTH (0..2), the last step's ALIGN (drpat).
edit (after 1.00): an LT step tapped on or off now and then between the changes (into the
journal; the pattern chosen loaded with its edits into the spare, an edit while the spare waits found against the
store and the journal). Prints hostcmd.c's drjit per run and a sum per load; exit 1 on a late hit, a dropped step or
BD off the grid."""
import os, random, re, subprocess, sys, tempfile

PATS = ["2,0,0;2,1,0;2,2,0", "2,0,7;3,1,0;2,0,-3", "3,0,0;4,2,6;2,1,0", "4,0,7;4,1,7;3,2,0"]


def script(seed, bpm, n, busy, pats, edit=False):
    r = random.Random(seed)
    o = ["wait 1500", "listen", "tap SEQ", "wait 300", "tap SEQ", "wait 300", "viewis 11"]
    for i, p in enumerate(pats.split(";")):
        o.append("drpat %d %s" % (i + 1, p.replace(",", " ")))
    o += ["bpm %d" % bpm, "press PLAY", "release PLAY", "wait 700", "qrun 2 1", "drjr"]
    cur, k = 0, len(pats.split(";"))
    for _ in range(n):
        t = r.randint(150, 3000)
        if busy and r.random() < 0.8:
            a = r.randint(0, t - 1)
            d = r.randint(5, 80) if busy == 1 or r.random() < 0.85 else r.randint(100, 300)
            o += ["wait %d" % a, "qbusy 1", "wait %d" % d, "qbusy 0", "wait %d" % max(1, t - a)]
        else:
            o.append("wait %d" % t)
        cur = r.choice([p for p in range(k) if p != cur])
        o.append("drq %d" % (cur + 1))
        if edit and r.random() < 0.6:
            o += ["wait %d" % r.randint(1, 400), "tapkey b2", "tapkey w%d" % r.randint(0, 15)]
    return "\n".join(o + ["wait 4000", "drjit", "jrok"]) + "\n"


def main():
    host, n = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 200
    edit = len(sys.argv) > 3 and sys.argv[3] == "edit"
    bad, tot = 0, {}
    with tempfile.TemporaryDirectory() as d:
        for busy in (0, 1, 2):
            for bpm in (90, 120, 160, 180):
                for p in PATS:
                    f = os.path.join(d, "s.txt")
                    open(f, "w").write(script(bpm + 7 * busy, bpm, n, busy, p, edit))
                    out = subprocess.run([host, f, d], capture_output=True, text=True).stdout
                    line = next((l for l in out.splitlines() if "drjit" in l), "no drjit")
                    if "FAIL jrok" in out or "jrok" not in out:
                        print("journal: RAM and store + journal differ")
                        bad = 1
                    print("busy %d bpm %3d %-20s %s" % (busy, bpm, p, line.strip()))
                    v = dict(re.findall(r"\b(changes|late|max|missing|offgrid) (-?[\d.]+)", line.replace("mean late", "")))
                    t = tot.setdefault(busy, [0, 0, 0.0, 0, 0])
                    t[0] += int(v.get("changes", 0))
                    t[1] += int(v.get("late", 0))
                    t[2] = max(t[2], float(v.get("max", 0)))
                    t[3] += max(0, int(v.get("missing", 0)))
                    t[4] += int(v.get("offgrid", 0))
    for busy, t in sorted(tot.items()):
        print("busy %d: changes %d, late hits %d (latest %.1f ms), steps dropped %d, BD off the grid %d" % (busy, *t))
        bad |= t[1] > 0 or t[3] > 0 or t[4] > 0
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
