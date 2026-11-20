# SPDX-License-Identifier: GPL-3.0-only
"""wobble.py [Q...]: the pitch wobble of the reverb's tail as a black box: eight sines at once (300 .. 4800 Hz, not
harmonic) for 1 s into rooms 0 and 3 at decay 112, then from 1.2 to 6 s per sine: the tail's spectral spread (the
power-weighted RMS width of its line, in cents of its frequency) and its instantaneous frequency's modulation (the IF's
RMS deviation in cents, and the share of the IF's spectrum at 0.8-1.6 Hz: the LFO). Prints the medians over the
sines, per room and quality."""
import sys
import numpy as np
from scipy import signal
import rvlib as R
import rvana as A
FS = R.FS
F = (301.0, 467.0, 733.0, 1031.0, 1489.0, 2203.0, 3301.0, 4813.0)


def tones(secs=7, on=1.0):
    x = np.zeros((int(secs * FS), 2)); n = int(on * FS); t = np.arange(n) / FS
    s = sum(np.sin(2 * np.pi * f * t + k) for k, f in enumerate(F)) * 0.05
    x[:n, 0] = x[:n, 1] = s * np.minimum(1, np.minimum(t, on - t) / 0.005)
    return x


def measure(y):
    out = []
    a, b = int(1.2 * FS), int(6.0 * FS)
    for c in (0, 1):
        x = y[a:b, c]
        X = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
        fr = np.fft.rfftfreq(len(x), 1 / FS)
        for f in F:
            m = (fr > f * 0.97) & (fr < f * 1.03)
            p = X[m] / X[m].sum(); cf = 1200 * np.log2(fr[m] / f)
            mu = (p * cf).sum(); spread = np.sqrt((p * (cf - mu) ** 2).sum())
            fi, en = A.inst_freq(y[:, c], f, bw=0.02 * f)
            g = fi[a:b]; w = en[a:b]; k = w > 0.1 * np.median(w)
            cents = 1200 * np.log2(np.maximum(g, 1) / f)
            cents = np.where(k, cents, np.median(cents[k]))
            dev = float(np.sqrt(np.mean((cents[k] - np.median(cents[k])) ** 2)))
            cc = signal.decimate(signal.decimate(cents - cents.mean(), 10), 10)
            Fm, P = signal.welch(cc, fs=FS / 100, nperseg=min(1024, len(cc)))
            lfo = float(P[(Fm > 0.8) & (Fm < 1.6)].sum() / P[(Fm > 0.2) & (Fm < 20)].sum())
            out.append((spread, dev, lfo))
    return np.median(np.array(out), 0)


def run(q, rooms=(0, 3), decay=112):
    exe, env = R.engines(q)
    ys = R.mlab.render_many(exe, [(R.patch(room=r, decay=decay), [('run', R.mlab.units(7))], tones(), 30, env) for r in rooms])
    return {r: measure(y[640:]) for r, y in zip(rooms, ys)}


if __name__ == '__main__':
    for q in sys.argv[1:] or ('full', 'nfull'):
        for r, (s, d, l) in run(q).items():
            print('%-7s room %d: line spread %5.1f cent, IF deviation %5.1f cent, LFO share %.2f' % (q, r, s, d, l))
