# SPDX-License-Identifier: GPL-3.0-only
"""glide.py ROOM T0 T1 [Q]: the pre-delay's glide as a black box: impulses every P frames (decay 0), A 114 changed
from T0 to T1 by a controller after 20 blocks; per impulse the delay of its response (cross-correlation with the
response at T0); renders offset by OFF frames interleave the impulses (a delay sample every OFF frames)."""
import sys
import numpy as np
from scipy import signal
import rvlib as R
FS = R.FS
room, t0, t1 = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]); q = sys.argv[4] if len(sys.argv) > 4 else 'full'
P = 8192 * 3; N = 12; K = 24; OFF = P // K
exe, env = R.engines(q)
jobs = []
for k in range(K):
    x = np.zeros((P * N + P, 2)); x[np.arange(N) * P + 256 + k * OFF, :] = 0.5
    jobs.append((R.patch(mode=2, room=room, time=t0, decay=0), [('run', 20), ('midi', 0xB0, 114, t1), ('run', len(x) // 128 - 20)], x, 30, env))
ys = R.mlab.render_many(exe, jobs)
tp = ys[0][640 + 256 * t0:640 + 256 * t0 + 2048, 1]
pts = []
for k, y in enumerate(ys):
    for i in range(N):
        s = i * P + 640 + k * OFF
        seg = y[s:s + 24000, 1]
        c = signal.correlate(seg, tp, 'valid')
        pts.append(((i * P + 256 + k * OFF - 20 * 128) / FS, int(np.argmax(c)) + 256 * t0, float(c.max() / np.dot(tp, tp))))
pts.sort()
for t, d, c in pts:
    if -0.05 < t < 3.0:
        print('%.3f %d %.2f' % (t, d - 256 * t0 - 0, c))
