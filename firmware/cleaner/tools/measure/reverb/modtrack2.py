# SPDX-License-Identifier: GPL-3.0-only
"""modtrack2.py ROOM [Q] [W0] [OUT.npy]: the reverb's delay modulation as a black box: impulses every 0.2 s (decay 0),
renders offset by 5 ms each (40), so the response's first windows sampled every 5 ms over 30 s; per impulse the lag of
the response window [W0, W0 + 512) (frames after the input) against the first impulse's (cross-correlation, parabolic
peak). Prints the lag series' spectrum peaks; saves (time, lag) of the right and the left channel."""
import sys
import numpy as np
from scipy import signal
import rvlib as R
FS = R.FS
room = int(sys.argv[1]); q = sys.argv[2] if len(sys.argv) > 2 else 'full'
W0 = int(sys.argv[3]) if len(sys.argv) > 3 else 1024
P = int(0.2 * FS); N = 150; K = 40; OFF = int(0.005 * FS)
exe, env = R.engines(q)
jobs = []
for k in range(K):
    x = np.zeros((P * N + P + K * OFF, 2)); x[np.arange(N) * P + 256 + k * OFF, :] = 0.5
    jobs.append((R.patch(decay=0, room=room), [('run', R.mlab.units(len(x) / FS))], x, 30, env))
ys = R.mlab.render_many(exe, jobs)


def lag(a, b, L=40):
    c = signal.correlate(a, b, 'full'); m = len(b) - 1; s = c[m - L:m + L + 1]; k = np.argmax(s)
    if 0 < k < len(s) - 1:
        return k - L + (s[k - 1] - s[k + 1]) / (2 * (s[k - 1] - 2 * s[k] + s[k + 1]))
    return k - L


ref = ys[0][640 + W0:640 + W0 + 512]
T, LR, LL = [], [], []
for k, y in enumerate(ys):
    for i in range(N):
        s = i * P + 640 + k * OFF + W0
        seg = y[s:s + 512]
        T.append((i * P + k * OFF) / FS); LR.append(lag(seg[:, 1], ref[:, 1])); LL.append(lag(seg[:, 0], ref[:, 0]))
o = np.argsort(T); T = np.array(T)[o]; LR = np.array(LR)[o]; LL = np.array(LL)[o]
if len(sys.argv) > 4:
    np.save(sys.argv[4], np.stack([T, LR, LL]))
for nm, v in (('R', LR), ('L', LL)):
    F, Pw = signal.welch(v - v.mean(), fs=200, nperseg=2048)
    pk = signal.find_peaks(Pw)[0]; pk = pk[np.argsort(Pw[pk])[::-1][:6]]
    print(nm, 'pp %.1f std %.2f' % (np.ptp(v), v.std()), 'spectrum peaks Hz', [round(float(F[p]), 3) for p in pk])
