# SPDX-License-Identifier: GPL-3.0-only
"""ph_null.py: the phase hand-over's constants of the cleaner, found by null tests against the development engine (a black box):
  CLN_PH_FIRST1..3  the phases the first note after the start takes (osc 1, 2, 3): the cleaner built with 0, the
                    first note's phases measured on both (ph_meas.fit_phases), the difference as the first guess, then
                    each refined on the null (the residual's energy is a parabola in each phase)
  SUB_C             the sub's start phase against half osc 1's: the same on a first note with the sub
Then the hand-over's order is checked bit for bit on note sequences that draw no random phase (every voice takes a
kept phase: A on/off, B on/off, ..., chords released and played again, a held voice stolen). Prints the -D flags."""
import sys
import numpy as np
import mlab, cbuild
from ph_meas import patch, fit_phases, osc_fr, hz

M = 1 << 32


def null_e(a, b):
    n = min(len(a), len(b)); return float(np.sum((a[:n] - b[:n]) ** 2))


def flags(p, subc=0):
    return ['-DCLN_PH_FIRST1=%du' % p[0], '-DCLN_PH_FIRST2=%du' % p[1], '-DCLN_PH_FIRST3=%du' % p[2], '-DSUB_C=%du' % subc]


def first(exe, P):
    return mlab.render(exe, P, [('run', 2), ('on', 57, 100), ('run', 40)])


if __name__ == '__main__':
    H = mlab.HYB
    P = patch()
    yh = first(H, P)
    e0 = cbuild.build('ph0', flags([0, 0, 0]))
    # (the first guess past the note's start transient: 2048 frames in)
    ph_h, _ = fit_phases(yh[:, 0], 2 * 128 + 2048, osc_fr(57)); ph_c, _ = fit_phases(first(e0, P)[:, 0], 2 * 128 + 2048, osc_fr(57))
    p = [int(((a - b) % 1.0) * M) % M for a, b in zip(ph_h, ph_c)]
    print('first guess', [hex(x) for x in p])
    import os
    if os.environ.get('PH_FIRST'): p = [int(v, 16) for v in os.environ['PH_FIRST'].split(',')]
    else:
     for it, step in enumerate((1 << 24, 1 << 20, 1 << 16, 1 << 12, 1 << 8)):
        for k in range(3):
            es = []
            for d in (-step, 0, step):
                q = list(p); q[k] = (q[k] + d) % M
                es.append(null_e(yh, first(cbuild.build('phs', flags(q)), P)))
            den = es[0] - 2 * es[1] + es[2]
            sh = 0.5 * (es[0] - es[2]) / den if den > 0 else 0.0
            p[k] = (p[k] + int(round(np.clip(sh, -2, 2) * step))) % M
        e = null_e(yh, first(cbuild.build('phs', flags(p)), P))
        print('pass %d: %s  null %.3g (signal %.3g)' % (it, [hex(x) for x in p], e, float(np.sum(yh ** 2))))
    # the sub: the guess from its fundamental's phase past the start, then the parabola
    S = patch(sub=True)
    ys = first(H, S)
    y0 = first(cbuild.build('phs', flags(p, 0)), S)
    fh = (hz(57), hz(57) / 2)
    gh, _ = fit_phases(ys[:, 0], 2 * 128 + 2048, fh); g0, _ = fit_phases(y0[:, 0], 2 * 128 + 2048, fh)
    sc = int(((gh[1] - g0[1]) % 1.0) * M) % M
    cand = [(sc + k * (M // 64)) % M for k in range(64)]   # (a coarse scan first: the square's edges are not smooth)
    sc = min(cand, key=lambda c: null_e(ys, first(cbuild.build('phs', flags(p, c)), S)))
    for step in (1 << 25, 1 << 24, 1 << 23, 1 << 22, 1 << 20, 1 << 18, 1 << 16, 1 << 14, 1 << 12, 1 << 10):
        es = [null_e(ys, first(cbuild.build('phs', flags(p, (sc + d) % M)), S)) for d in (-step, 0, step)]
        sc = (sc + (int(np.argmin(es)) - 1) * step) % M
    print('SUB_C %s null %.3g' % (hex(sc), null_e(ys, first(cbuild.build('phs', flags(p, sc)), S))))
    print(' '.join(flags(p, sc)))
