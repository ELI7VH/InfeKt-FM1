# SPDX-License-Identifier: GPL-3.0-only
"""meas_dist14.py ENGINE OUT.json: the static curves of distortion types 1-4 measured as a black box: the input
straight into the effects (input mode 3, select right: the distortion sees the input sample itself), a slow ramp
-1..+1 in, out against in; at the intensities 0, 16, ..., 112, 127 (the curves between them are their linear blend)
and type 0 for the path's linear gain after the distortion. Writes OUT.npz (x: the 65536 input values, y_T_I: the
curves); fit_dist14.py fits them."""
import sys
import numpy as np
import mlab

N = 1 << 16
NODES = (0, 16, 32, 48, 64, 80, 96, 112, 127)


def patch(t, it):
    p = mlab.init_preset()
    p[mlab.INPUT_MODE] = 3; p[mlab.INPUT_SELECT] = 2; p[mlab.CHORUS_MIX] = 0; p[mlab.FX_SEND] = 0; p[mlab.FX_MODE] = 0
    p[mlab.DIST_TYPE] = t; p[mlab.DIST_INT] = it
    return p


def curves(exe, types=(1, 2, 3, 4), nodes=NODES):
    ramp = np.linspace(-1, 1, N)
    q = np.round(ramp * 8388608.0).clip(-8388608, 8388607) / 8388608.0
    inp = np.zeros((N + 8192, 2)); inp[2048:2048 + N, :] = q[:, None]
    keys = [(0, 0)] + [(t, it) for t in types for it in nodes]
    res = mlab.render_many(exe, [(patch(t, it), [('run', (N + 8192) // 128)], inp) for t, it in keys])
    y0 = res[0][:, 1]
    lat = int(np.argmax(np.correlate(y0[:N + 4096], inp[:N + 4096, 1], 'valid')[:2048])) if False else None
    # (the latency: where type 0's output follows the ramp best)
    best = None
    for L in range(0, 1024):
        seg = y0[2048 + L:2048 + L + N]
        if len(seg) < N: break
        c = np.dot(seg, q) / np.dot(q, q)
        e = np.sum((seg - c * q) ** 2)
        if best is None or e < best[0]: best = (e, L, c)
    _, lat, gain = best
    out = {'latency': np.array(lat), 'gain': np.array(gain), 'x': q}
    for (t, it), a in zip(keys[1:], res[1:]):
        out['y_%d_%d' % (t, it)] = a[2048 + lat:2048 + lat + N, 1] / gain
    return out


if __name__ == '__main__':
    R = curves(sys.argv[1])
    np.savez_compressed(sys.argv[2], **R)
    print('latency', R['latency'], 'gain', R['gain'])
