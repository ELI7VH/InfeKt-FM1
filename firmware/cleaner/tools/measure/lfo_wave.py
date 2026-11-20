# SPDX-License-Identifier: GPL-3.0-only
"""lfo_wave.py: the LFO wave table (shapes 6-67, one cycle in 64 points; engine/sy_hyb_lfow.c) measured on the development engine
host engine as a black box (renders only; nothing of its source or tables is read).
Method: LFO 1 (key trigger: phase 0 at the note) moves osc 1's pitch (a sine carrier, filters open). The log of the
carrier's instantaneous frequency (analytic phase) is linear in the LFO value. A render of CYC cycles at a slow rate is
folded onto one cycle (averaged over the cycles after the first), read at the 64 nodes (a short mean around each),
and scaled by the same chain's sine (shape 0, -1..1 at phase 0 = low point). The wave's own mean is kept.
Usage: lfo_wave.py [--check] [--write]   (--check: error against the current table; --write: regenerate the file)"""
import sys, os, json
import numpy as np
import mlab, lfo_meas

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(mlab.ROOT, 'cleaner', 'engine', 'sy_hyb_lfow.c')
FS, N, CYC, RV, DEPTH = mlab.FS, 64, 6, 55, 40
RATE = json.load(open(os.path.join(HERE, 'lfo_rate.json')))[RV]      # Hz (about 0.5)
HEAD = ("/* the LFO shapes 6-67 (the waves), one cycle in 64 points, measured on renders of the development engine\n"
        " * host engine (tools/measure/lfo_wave.py: LFO 1 on osc 1's pitch, folded over several cycles, scaled by the\n"
        " * sine); phase 0 where the sine (shape 0) is at its low point */\n")


def curve(shape):
    P = lfo_meas.patch(1)
    P[68] = shape; P[73] = 1; P[67] = RV; P[74] = DEPTH
    nb = int((CYC + 1) / RATE * FS / mlab.BLOCK)
    y = mlab.render(mlab.HYB, P, [('on', lfo_meas.KEY_T, 100), ('run', nb)])
    n = np.arange(len(y)); c = np.exp(-2j * np.pi * lfo_meas.FT * n / FS)
    W = 201; w = np.hanning(W); w /= w.sum()
    z = np.convolve(y[:, 0] * c, w, 'same')
    f = np.gradient(np.unwrap(np.angle(z + 1e-30))) * FS / (2 * np.pi) + lfo_meas.FT
    lf = np.log(np.maximum(f, 1.0))
    T = FS / RATE                                   # samples per cycle
    out = np.zeros(N)
    for i in range(N):
        acc = []
        for k in range(1, CYC):                     # (skip cycle 0: the start-up)
            t0 = (k + i / N) * T
            a = int(round(t0))
            h = int(T / N * 0.12)
            acc.append(np.mean(lf[a - h:a + h + 1]))
        out[i] = np.mean(acc)
    return out


def main():
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(8) as ex:
        res = list(ex.map(curve, [0] + list(range(6, 68))))
    s = res[0]; A = (s.max() - s.min()) / 2; M = (s.max() + s.min()) / 2
    # (sine at 64 nodes sampled the same way: its extremes lie on nodes 0 and 32)
    tab = np.array([-(r - M) / A for r in res[1:]])    # (the pitch runs against the sine's sign: its phase 0 is the low point)
    import re
    cur = np.array([[float(x) for x in l.split(',') if x.strip()] for l in
                    re.findall(r'\{([^{}]*)\}', re.sub(r'f', '', open(OUT).read().split('= {', 1)[1]))])
    cur = cur.reshape(62, 64)
    err = np.abs(tab - cur).max(1)
    print('max error per wave (of full scale 1): ' + ' '.join('%d:%.4f' % (k + 6, e) for k, e in enumerate(err)))
    print('worst %.4f at shape %d' % (err.max(), err.argmax() + 6))
    if '--write' in sys.argv:
        body = ',\n'.join('    {' + ', '.join('%.3ff' % v for v in row) + '}' for row in tab)
        open(OUT, 'w').write(HEAD + '#include "sy_hyb_lfow.h"\nconst float sy_hyb_lfow[62][64] = {\n' + body + '\n};\n')


if __name__ == '__main__':
    main()
