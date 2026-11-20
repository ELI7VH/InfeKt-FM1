# SPDX-License-Identifier: GPL-3.0-only
"""f1stale.py [ENGINE ...]: filter 1's modes 4 and 6 in a voice, rendered on each ENGINE as a black box (default: the
reference, MLAB_REFENG, and the cleaner's host engine; PROVENANCE.md 14).
The init patch, one note C3 held 1 s, the level of 0.2-0.9 s (dB, L+R mean power) and the share of the power on the
lines of fs / 128 ("blk": a block repeated). Tables:
    dist   mode 6 against the distortion type (B 100 0-12, intensity 64) per routing and balance
    col    modes 4 and 6 against the noise colour (B 39) and volume (B 37), the cutoff, the osc volume
    split  mode 4 in split (filter 1 left with osc 1): its flat pass against the osc volume"""
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mlab

ENGS = sys.argv[1:] or [os.environ['MLAB_REFENG'], mlab.CLN]
EV = [('on', 48, 100), ('run', mlab.units(1.0)), ('off', 48), ('run', 2)]
F1, ROUTE, FBAL, NVOL, NCOL, OVOL, CUT1, DIST = 51, 53, 48, 37, 39, 36, 40, mlab.PB + 100


def pat(c):
    p = mlab.init_preset()
    for k, v in c.items(): p[k] = v
    return p


def meas(x):
    v = x[int(0.2 * mlab.FS):int(0.9 * mlab.FS)]
    lv = 10 * np.log10((v ** 2).mean() + 1e-20)
    u = v[:len(v) // 128 * 128, 0]
    S = np.abs(np.fft.rfft(u)) ** 2
    m = np.arange(len(S)) % (len(u) // 128) == 0
    return lv, 10 * np.log10(S[m].sum() / (S.sum() + 1e-30) + 1e-20)


def table(name, cases):
    print('==', name)
    xs = {e: mlab.render_many(e, [(pat(c), EV) for c in cases]) for e in ENGS}
    for j, c in enumerate(cases):
        print('%-44s' % c, '  '.join('%7.1f dB blk %6.1f' % meas(xs[e][j]) for e in ENGS))


table('dist', [{F1: 6, ROUTE: r, FBAL: fb, DIST: t, DIST + 1: 64} for r in (0, 1, 2, 3) for fb in (0, 64, 127)
               for t in (0, 1, 6, 11, 12)])
table('col', [{F1: m, NVOL: nv, NCOL: c} for m in (4, 6) for nv in (0, 1, 127) for c in (0, 49, 63, 64, 65, 127)]
      + [{F1: m, NVOL: 1, NCOL: 49, CUT1: k} for m in (4, 6) for k in (30, 64, 100)]
      + [{F1: 4, NVOL: 1, NCOL: 49, OVOL: v} for v in (0, 64, 96, 127)] + [{F1: 6, NVOL: 1, NCOL: 49, DIST: 1}])
table('split', [{F1: m, ROUTE: 3, FBAL: 0, OVOL: v} for m in (4, 6) for v in (32, 64, 100, 127)])
