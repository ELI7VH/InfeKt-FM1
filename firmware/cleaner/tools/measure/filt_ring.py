# SPDX-License-Identifier: GPL-3.0-only
"""filt_ring.py [HYB CLN]: filter 1 at high resonance, the development engine against the cleaner (black boxes): an impulse
through the static path (input mode 2, filter 1 alone) at resonance 96..127 and cutoffs 0..127, the ringing read
from the response: its frequency (the spectrum's peak, interpolated), the peak's height (dB) and the decay (the
envelope's slope, dB/s); prints the largest differences and every case above 1 % (frequency, decay rate) or 0.1 dB."""
import sys
import numpy as np
import mlab
exec(open(__file__.replace('filt_ring.py', 'cmp_comp.py')).read().split('cases = {}')[0])

H, C = (sys.argv[1], sys.argv[2]) if len(sys.argv) > 2 else (mlab.HYB, mlab.CLN)
NN = 1 << 16
imp = np.zeros((NN, 2)); imp[0] = 0.02
keys = [(c, r) for r in (96, 104, 112, 120, 124, 127) for c in list(range(0, 128, 4)) + [126, 127]]
jobs = [(static({mlab.CUTOFF: c, mlab.F1_RESO: r}), [('run', NN // 128)], imp) for c, r in keys]
A, B = mlab.render_many(H, jobs), mlab.render_many(C, jobs)


def ring(h):
    X = np.abs(np.fft.rfft(h * np.hanning(len(h)), 1 << 18)); f = np.fft.rfftfreq(1 << 18, 1 / mlab.FS)
    k = int(np.argmax(X[5:])) + 5
    a, b, c = np.log(X[k - 1:k + 2] + 1e-30); d = 0.5 * (a - c) / (a - 2 * b + c)
    fr = f[k] + d * (f[1] - f[0])
    env = np.abs(h); w = 256; e = np.array([env[i:i + w].max() for i in range(0, len(h) - w, w)])
    m = (e > e.max() * 1e-3) & (np.arange(len(e)) > 1)
    sl = np.polyfit(np.arange(len(e))[m] * w / mlab.FS, 20 * np.log10(e[m] + 1e-30), 1)[0] if m.sum() > 3 else 0.0
    return fr, 20 * np.log10(X.max()), sl


worst = [0, 0, 0]; bad = []
for (c, r), x, y in zip(keys, A, B):
    fh, ph, dh = ring(x[:, 0]); fc, pc, dc = ring(y[:, 0])
    df = abs(fc / fh - 1); dp = abs(pc - ph); dd = abs(dc / dh - 1) if dh < -1 else 0.0
    worst = [max(worst[0], df), max(worst[1], dp), max(worst[2], dd)]
    if df > 0.01 or dp > 0.1 or dd > 0.01: bad.append('c%d r%d: f %.2f%% peak %.2f dB decay %.2f%%' % (c, r, 100 * df, dp, 100 * dd))
print('%d cases: ringing frequency within %.3f %%, peak %.3f dB, decay rate %.2f %%' % (len(keys), 100 * worst[0], worst[1], 100 * worst[2]))
for b in bad: print('  ' + b)
