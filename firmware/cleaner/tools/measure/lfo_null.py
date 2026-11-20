# SPDX-License-Identifier: GPL-3.0-only
"""lfo_null.py: LFO1_PH0 and LFO2_PH0 (the part's LFO phases at the start) refined on the null against the development engine: a
first note alone, LFO 2 on the pan / LFO 1 on osc 1's pitch (lfo_meas.patch), the cleaner built with each candidate:
the residual is a parabola in the phase. Then the added voices' sequence (lfo_meas.seq) checked on the null with
CLN_LFO_PH as set. Start values: lfo_meas.py's first-note shifts."""
import sys
import numpy as np
import mlab, cbuild, lfo_meas


def null_e(a, b):
    n = min(len(a), len(b)); return float(np.sum((a[:n] - b[:n]) ** 2))


def refine(lfo, x0):
    P = lfo_meas.patch(lfo)
    ev = [('run', 6), ('on', lfo_meas.KEY_T, 100), ('run', 120)]
    yh = mlab.render(mlab.HYB, P, ev)
    name = 'LFO%d_PH0' % lfo
    f = lambda x: null_e(yh, mlab.render(cbuild.build('lfoph', ['-D%s=%.7ff' % (name, x)]), P, ev))
    x = x0
    for st in (0.004, 0.001, 0.0002, 0.00004):
        es = [f(x - st), f(x), f(x + st)]
        den = es[0] - 2 * es[1] + es[2]
        x += float(np.clip(0.5 * (es[0] - es[2]) / den, -2, 2)) * st if den > 0 else 0.0
    return x, f(x), float(np.sum(yh ** 2))


if __name__ == '__main__':
    for lfo, x0 in ((1, float(sys.argv[1])), (2, float(sys.argv[2]))):
        x, e, sg = refine(lfo, x0)
        print('LFO%d_PH0 %.5f  null %.2e of %.2e (%.1f dB)' % (lfo, x, e, sg, 10 * np.log10(e / sg + 1e-30)))
    P = lfo_meas.patch(2)
    yh = mlab.render(mlab.HYB, P, lfo_meas.seq()); yc = mlab.render(mlab.CLN, P, lfo_meas.seq())
    print('added voices (CLN_LFO_PH, the built cleaner): null %.1f dB' % (10 * np.log10(null_e(yh, yc) / np.sum(yh ** 2) + 1e-30)))
