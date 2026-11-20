# SPDX-License-Identifier: GPL-3.0-only
"""filt_top2.py CUT RESO [RESO ...]: CLN_FILT_K at one cutoff value and the given resonance nodes (CLN_FILT_KR) refined on
the null against the development engine, as filt_top.py does for cutoff 126 at 127: noise through the static path at that cutoff
and each resonance, each entry scanned in turn (factor 0.85..1.15, then +-1 %), the header rewritten with the best.
For the cutoffs where fit_filt.py's fit did not hold and its neighbours' line was taken (122 at 114..126)."""
import re, os, sys
import numpy as np
import mlab, cbuild
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'cmp_comp.py')).read().split('cases = {}')[0])

HDR = os.path.join(cbuild.CL, 'engine', 'sy_cln_filt.h')
KR = (0, 64, 96, 112, 120, 124, 127)


def rows_of(txt):
    a = txt.index('static const float CLN_FILT_K[CLN_FILT_NR][128]')
    return a, txt[a:].split('}, {')


def set_k(txt, j, c, v):
    a, rows = rows_of(txt)
    vals = re.findall(r'-?\d+\.\d+f', rows[j])
    vals[c] = '%.5ff' % v
    it = iter(vals)
    rows[j] = re.sub(r'-?\d+\.\d+f', lambda m: next(it), rows[j])
    return txt[:a] + '}, {'.join(rows)


def get_k(txt, j, c):
    a, rows = rows_of(txt)
    return float(re.findall(r'-?\d+\.\d+f', rows[j])[c][:-1])


def err(c, r, txt):
    open(HDR, 'w').write(txt)
    job = (static({mlab.CUTOFF: c, mlab.F1_RESO: r}), [('run', N // 128)], st(noise * 0.05))
    return mlab.diff_db(mlab.render(mlab.HYB, *job)[2048:], mlab.render(cbuild.build('ftop2'), *job)[2048:])


if __name__ == '__main__':
    c = int(sys.argv[1])
    txt = open(HDR).read()
    for r in [int(x) for x in sys.argv[2:]]:
        j = KR.index(r)
        k0 = get_k(txt, j, c)
        best = (err(c, r, txt), k0)
        for grid in (np.linspace(0.85, 1.15, 31), np.linspace(0.99, 1.01, 21)):
            for v in best[1] * grid:
                e = err(c, r, set_k(txt, j, c, v))
                if e < best[0]: best = (e, v)
        txt = set_k(txt, j, c, best[1])
        print('cutoff %d resonance %d: %.5f -> %.5f, null %.1f dB' % (c, r, k0, best[1], best[0]))
    open(HDR, 'w').write(txt)
