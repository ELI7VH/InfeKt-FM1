# SPDX-License-Identifier: GPL-3.0-only
"""meas_dist89.py ENGINE OUT.json: distortion types 8 (a quantiser) and 9 (a rate reducer) measured as a black box:
the input straight into the effects (input mode 3, select right), at every intensity 0..127:
  8  a slow ramp -0.5..+0.5 in: the step of the output's staircase (the median of its nonzero jumps, against the
     path's linear gain)
  9  a slow ramp in: the hold, the mean number of frames between the output's changes over 2^16 frames (the reducer
     takes a new sample whenever its phase passes 1: 1 / its phase step)
fit_dist89.py fits the laws."""
import sys, json
import numpy as np
import mlab

N = 1 << 16


def patch(t, it):
    p = mlab.init_preset()
    p[mlab.INPUT_MODE] = 3; p[mlab.INPUT_SELECT] = 2; p[mlab.CHORUS_MIX] = 0; p[mlab.FX_SEND] = 0; p[mlab.FX_MODE] = 0
    p[mlab.DIST_TYPE] = t; p[mlab.DIST_INT] = it
    return p


def run(exe):
    ramp = np.linspace(-0.5, 0.5, N)
    inp = np.zeros((N + 8192, 2)); inp[4096:4096 + N] = ramp[:, None]
    keys = [(0, 0)] + [(t, it) for t in (8, 9) for it in range(128)]
    res = mlab.render_many(exe, [(patch(t, it), [('run', (N + 8192) // 128)], inp) for t, it in keys])
    y0 = res[0][:, 1]
    best = None
    for L in range(0, 2048):
        seg = y0[4096 + L:4096 + L + N]
        c = np.dot(seg, ramp) / np.dot(ramp, ramp); e = np.sum((seg - c * ramp) ** 2)
        if best is None or e < best[0]: best = (e, L, c)
    _, lat, gain = best
    out = {'latency': lat, 'gain': gain, 'step8': [], 'hold9': []}
    for (t, it), a in zip(keys[1:], res[1:]):
        y = a[4096 + lat - 256:4096 + lat + N, 1] / gain
        d = np.diff(y); nz = np.abs(d) > 1e-7
        if t == 8:
            out['step8'].append(float(np.median(np.abs(d[nz]))) if nz.any() else 0.0)
        else:
            ch = np.flatnonzero(nz)
            out['hold9'].append(float((ch[-1] - ch[0]) / max(len(ch) - 1, 1)) if len(ch) > 1 else 0.0)
    return out


if __name__ == '__main__':
    R = run(sys.argv[1])
    json.dump(R, open(sys.argv[2], 'w'))
    for i in range(0, 128, 16): print(i, '%.6f %.3f' % (R['step8'][i], R['hold9'][i]))
