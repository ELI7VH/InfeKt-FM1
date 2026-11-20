# SPDX-License-Identifier: GPL-3.0-only
"""filt_top.py: CLN_FILT_K at resonance 127 and cutoff 126, where filt_fit.py's fit does not hold (the resonance
right at the top of the band), refined on the null against the development engine (noise through the static path at that
cutoff and resonance 127): each entry scanned, the header rewritten with the best. Run after fit_filt.py."""
import re, os
import numpy as np
import mlab, cbuild
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'cmp_comp.py')).read().split('cases = {}')[0])

HDR = os.path.join(cbuild.CL, 'engine', 'sy_cln_filt.h')


def set_k(txt, c, v):
    a = txt.index('static const float CLN_FILT_K[CLN_FILT_NR][128]')
    body = txt[a:]
    rows = body.split('}, {')
    third = rows[-1]
    vals = re.findall(r'-?\d+\.\d+f', third)
    vals[c] = '%.5ff' % v
    it = iter(vals)
    rows[-1] = re.sub(r'-?\d+\.\d+f', lambda m: next(it), third)
    return txt[:a] + '}, {'.join(rows)


def get_k(txt, c):
    a = txt.index('static const float CLN_FILT_K[CLN_FILT_NR][128]')
    return float(re.findall(r'-?\d+\.\d+f', txt[a:].split('}, {')[-1])[c][:-1])


if __name__ == '__main__':
    orig = open(HDR).read()
    txt = orig
    for c in (126,):
        job = (static({mlab.CUTOFF: c, mlab.F1_RESO: 127}), [('run', N // 128)], st(noise * 0.05))
        yh = mlab.render(mlab.HYB, *job)
        k0 = get_k(txt, c)
        best = None
        for v in k0 * np.linspace(0.85, 1.15, 31):
            open(HDR, 'w').write(set_k(txt, c, v))
            y = mlab.render(cbuild.build('ftop'), *job)
            e = mlab.diff_db(yh[2048:], y[2048:])
            if best is None or e < best[0]: best = (e, v)
        for v in best[1] * np.linspace(0.99, 1.01, 21):
            open(HDR, 'w').write(set_k(txt, c, v))
            e = mlab.diff_db(yh[2048:], mlab.render(cbuild.build('ftop'), *job)[2048:])
            if e < best[0]: best = (e, v)
        txt = set_k(txt, c, best[1])
        print('cutoff %d: %.5f -> %.5f, null %.1f dB' % (c, k0, best[1], best[0]))
    open(HDR, 'w').write(txt)
