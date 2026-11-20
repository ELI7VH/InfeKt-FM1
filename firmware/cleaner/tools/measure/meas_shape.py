# SPDX-License-Identifier: GPL-3.0-only
"""meas_shape.py ENGINE OUT.json: the oscillator's morph from saw to square (shape 64..127) measured as a black box:
osc 1 alone, pulse width 0 (duty 0.5), filters open, key 45; the 2nd harmonic against the fundamental R. The shape is
(1 - c) saw + c square (both of the saw's height; synth.c osc_set), so h1 ~ 1 + c, h2 ~ (1 - c) / 2: c = (1 - 2R) / (1 + 2R).
fit_shape.py fits c over the shape."""
import sys, json
import numpy as np
import mlab


def patch(sh):
    p = mlab.init_preset()
    p[mlab.OSC1_SHAPE] = sh; p[mlab.OSC1_PW] = 0; p[mlab.OSC_BAL] = 0; p[mlab.CUTOFF] = 127; p[mlab.CUTOFF2] = 127
    p[mlab.CHORUS_MIX] = 0; p[mlab.FX_SEND] = 0; p[59] = 0; p[60] = 0; p[61] = 127; p[63] = 0; p[97] = 0
    return p


def run(exe):
    shapes = list(range(60, 128))
    res = mlab.render_many(exe, [(patch(s), [('on', 45, 100), ('run', 340)]) for s in shapes])
    f0 = 440 * 2 ** ((45 - 69) / 12)
    out = {'shape': shapes, 'R': [], 'c': []}
    for a in res:
        y = a[8192:8192 + (1 << 15), 0] * np.blackman(1 << 15)
        X = np.abs(np.fft.rfft(y)); f = np.fft.rfftfreq(1 << 15, 1 / mlab.FS)
        h = [X[np.abs(f - k * f0) < 8].max() for k in (1, 2)]
        R = h[1] / h[0]
        out['R'].append(float(R)); out['c'].append(float((1 - 2 * R) / (1 + 2 * R)))
    return out


if __name__ == '__main__':
    R = run(sys.argv[1])
    json.dump(R, open(sys.argv[2], 'w'))
    print(np.round(R['c'][::4], 4))
