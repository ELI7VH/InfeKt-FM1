# SPDX-License-Identifier: GPL-3.0-only
"""meas_sat5.py ENGINE OUT.json: saturation type 5 (a hard clip) measured as a black box: the input through the filters
without a note (input mode 2, filters open, saturation 5), a slow ramp -1..+1 in, at every drive (osc volume) 64..127:
the clip's slope (out against in where |out| is below half the limit) and its limit (the median of the flat ends),
both against the path's linear gain (saturation off). fit_sat5.py makes the cleaner's law from the development engine's and the
cleaner's own measurement."""
import sys, json
import numpy as np
import mlab

N = 1 << 15


def patch(sat, dv):
    p = mlab.init_preset()
    p[mlab.INPUT_MODE] = 2; p[mlab.INPUT_SELECT] = 2; p[mlab.OSC_VOL] = dv; p[mlab.CHORUS_MIX] = 0; p[mlab.FX_SEND] = 0
    p[mlab.CUTOFF] = 127; p[mlab.CUTOFF2] = 64; p[mlab.F1_RESO] = 0; p[mlab.F2_RESO] = 0; p[mlab.F_BAL] = 0
    p[mlab.SAT_TYPE] = sat
    return p


def run(exe):
    ramp = np.linspace(-1, 1, N)
    inp = np.zeros((N + 8192, 2)); inp[4096:4096 + N] = ramp[:, None]
    keys = [(0, 64)] + [(5, dv) for dv in range(64, 128)]
    res = mlab.render_many(exe, [(patch(s, dv), [('run', (N + 8192) // 128)], inp) for s, dv in keys])
    y0 = res[0][:, 1]
    best = None
    for L in range(0, 2048):
        seg = y0[4096 + L:4096 + L + N]
        c = np.dot(seg, ramp) / np.dot(ramp, ramp); e = np.sum((seg - c * ramp) ** 2)
        if best is None or e < best[0]: best = (e, L, c)
    _, lat, gain = best
    out = {'latency': lat, 'gain': gain, 'drive': [], 'slope': [], 'limit': []}
    for (s, dv), a in zip(keys[1:], res[1:]):
        y = a[4096 + lat:4096 + lat + N, 1] / gain
        lim = 0.5 * (np.median(y[-N // 8:]) - np.median(y[:N // 8]))
        m = np.abs(y) < 0.5 * lim
        sl = np.polyfit(ramp[m], y[m], 1)[0]
        out['drive'].append(dv); out['slope'].append(float(sl)); out['limit'].append(float(lim))
    return out


if __name__ == '__main__':
    R = run(sys.argv[1])
    json.dump(R, open(sys.argv[2], 'w'))
    for i in range(0, 64, 8): print(R['drive'][i], '%.4f %.5f' % (R['slope'][i], R['limit'][i]))
