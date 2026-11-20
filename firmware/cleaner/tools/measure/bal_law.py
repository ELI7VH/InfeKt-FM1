# SPDX-License-Identifier: GPL-3.0-only
"""bal_law.py [ENGINE]: the filter balance per routing at every FBAL value (0-127), rendered on ENGINE as a black box (default:
the reference, MLAB_REFENG), and the cleaner's law (synth.c BAL_LAW,
PROVENANCE.md 10) against it. One saw note (C3, 1 s), the init patch, cutoff 2 unlinked where it matters:
    sA  serial, filter 1's share: filter 1 an open low pass, filter 2 closed (cutoff 2 at 0)
    sB  serial, the oscillators' share into filter 2 (FBAL > 64): filter 1 in mode 4 (silent in a voice), filter 2 open
    sD  serial, FBAL 0-64: filter 1 open, filter 2 a high pass at 790 Hz: the 131 Hz band filter 1's share, the
        2.5-10 kHz bands both (constant when the shares add to 1)
    pA, pB  parallel, as sA, sB;  xA split, filter 1 (left);  xB split, filter 2 (right, osc 2 alone)
Each share is its level against the full path's (sA at 0, sB at 127; split against the parallel law's ends). The
shares near 0 read the closed filter's leak (-31 dB) along with them: the law is checked where the share is above
-24 dB."""
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mlab

ENG = sys.argv[1] if len(sys.argv) > 1 else os.environ['MLAB_REFENG']
EV = [('on', 48, 100), ('run', mlab.units(1.0))]
A0 = int(0.2 * mlab.FS)


def pat(c, fb):
    p = mlab.init_preset(); p[mlab.PB + 32] = 0      # (cutoff 2 unlinked)
    for k, v in c.items(): p[getattr(mlab, k)] = v
    p[mlab.F_BAL] = fb
    return p


def P(n):
    return (2 * n if n <= 16 else 32 + 4 * (n - 16) if n <= 32 else 96 + 5 * (n - 32)) / 256


def law(route, fb):
    """(a, b, c) as synth.c's BAL_LAW"""
    n = 64 - fb if fb <= 64 else fb - 64
    p = P(n) + (5 * (fb - 126) / 256 if fb > 126 else 0)
    if route <= 1:
        return (p, 1 - p, 0.) if fb <= 64 else (0., 1., p)
    g = 2.523 if route == 3 else 1.
    return (g * (0.5 + 0.20711 * p), g * 0.5 * (1 - p), 1.) if fb <= 64 else (g * 0.5 * (1 - p), g * (0.5 + 0.20711 * p), 1.)


def band(x, lo, hi):
    f = np.fft.rfftfreq(len(x), 1 / mlab.FS); S = np.abs(np.fft.rfft(x.mean(axis=1) * np.hanning(len(x)))) ** 2
    return 10 * np.log10(S[(f >= lo) & (f < hi)].sum() + 1e-30)


def main():
    F = list(range(128))
    C = {'sA': dict(F_ROUTE=0, F1_MODE=0, CUTOFF=127, CUTOFF2=0), 'sB': dict(F_ROUTE=0, F1_MODE=4, CUTOFF2=127),
         'pA': dict(F_ROUTE=2, F1_MODE=0, CUTOFF=127, CUTOFF2=0), 'pB': dict(F_ROUTE=2, F1_MODE=4, CUTOFF2=127),
         'xA': dict(F_ROUTE=3, F1_MODE=0, CUTOFF=127, CUTOFF2=0), 'xB': dict(F_ROUTE=3, OSC_BAL=127, CUTOFF2=127),
         'sD': dict(F_ROUTE=0, F1_MODE=0, CUTOFF=127, F2_MODE=1, CUTOFF2=64)}
    jobs = [(pat(c, fb), EV) for c in C.values() for fb in F]
    xs = mlab.render_many(ENG, jobs)
    L = {}
    for i, nm in enumerate(C):
        X = [x[A0:] for x in xs[i * 128:(i + 1) * 128]]
        if nm == 'sD': L[nm] = np.array([[band(x, 110, 150), band(x, 2400, 10500)] for x in X])
        else: L[nm] = np.array([10 * np.log10(np.mean(x ** 2) + 1e-30) for x in X])
    full = L['sA'][0]
    print('serial sD: the high bands over FBAL 0-64 (a + b constant?): max - min %.2f dB' % np.ptp(L['sD'][:65, 1]))
    worst = 0.
    for nm, route, k, ref in (('sA', 0, 0, full), ('sB', 0, 1, full), ('pA', 2, 0, full), ('pB', 2, 1, full),
                              ('xA', 3, 0, L['xA'][0] - 20 * np.log10(law(3, 0)[0])),
                              ('xB', 3, 1, L['xB'][127] - 20 * np.log10(law(3, 127)[1]))):
        meas = 10 ** ((L[nm] - ref) / 20)
        mdl = np.array([law(route, fb)[k] * (law(route, fb)[2] if nm == 'sB' else 1.) for fb in F])
        ok = (mdl > 10 ** (-24 / 20)) & (meas > 10 ** (-24 / 20))
        e = 20 * np.log10(meas[ok] / mdl[ok])
        worst = max(worst, np.abs(e).max())
        print('%s: %3d values above -24 dB, model - measured max |%.3f| dB, median %.3f dB' % (nm, ok.sum(), np.abs(e).max(), np.median(np.abs(e))))
    print('worst %.3f dB' % worst)


if __name__ == '__main__':
    main()
