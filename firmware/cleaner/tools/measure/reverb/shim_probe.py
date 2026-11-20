# SPDX-License-Identifier: GPL-3.0-only
"""shim_probe.py: FX > SHIMMER measured as a black box: a 440 Hz tone (0.5 s) into the reverb (HALF) with the shimmer
set by SY_SHIM; the energy around 440 x 2^k (k = 0..3) and 660 x 2^k per 100 ms window. shim_tracks(exe, env, patch)
-> (times, {f: dB})"""
import numpy as np
import rvlib as R
FS = R.FS
F0 = 440.0
FREQS = (440.0, 660.0, 880.0, 1320.0, 1760.0, 2640.0, 3520.0)


def tone(secs, f=F0, on=0.5, amp=0.25):
    x = np.zeros((int(secs * FS), 2)); n = int(on * FS); t = np.arange(n) / FS
    x[:n, 0] = x[:n, 1] = amp * np.sin(2 * np.pi * f * t) * np.minimum(1, np.minimum(t, on - t) / 0.005)
    return x


def tracks(y, win=0.1):
    n = int(win * FS); out = {f: [] for f in FREQS}; ts = []
    w = np.hanning(n)
    fr = np.fft.rfftfreq(n, 1 / FS)
    for s in range(0, len(y) - n, n):
        X = np.abs(np.fft.rfft((y[s:s + n, 0] + y[s:s + n, 1]) * w)) ** 2
        for f in FREQS:
            m = (fr > f * 0.97) & (fr < f * 1.03)
            out[f].append(10 * np.log10(X[m].sum() + 1e-20))
        ts.append(s / FS)
    return np.array(ts), {f: np.array(v) for f, v in out.items()}


def shim_run(exe, env, p, secs=8):
    return R.run(exe, p, tone(secs), secs, env=env)[640:]


if __name__ == '__main__':
    import sys
    sh = sys.argv[1] if len(sys.argv) > 1 else '127,0,100,64,0,0,1,0'
    for q in ('half', 'nhalf'):
        exe, env = R.engines(q)
        env = dict(env, SY_SHIM=sh)
        y = shim_run(exe, env, R.patch(room=3, decay=100))
        t, tr = tracks(y)
        print(q, sh)
        for f in FREQS:
            print('  %5d' % f, ' '.join('%5.0f' % v for v in tr[f][::4]))
