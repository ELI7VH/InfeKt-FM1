# SPDX-License-Identifier: GPL-3.0-only
"""modkind_rv.py Q [ROOM DECAY ...] [--out J]: is a reverb's modulation periodic (an LFO) or random? Measured as a black
box (rvlib.py; Q = full, half, half16 of the development engine, nfull, nhalf, nhalf16 of the own one).

An impulse is sent at the time T after the engine's start (T = 0 .. 3 s in 10 ms steps, each a render of its own). A
time-invariant reverb answers every impulse the same; a modulated one answers with its paths' delays as they are at T.
Per window of the response (W: 15-45 ms, 60-120 ms, 150-250 ms after the impulse) and side, the response at T is
cross-correlated with the one at T = 0: the lag of the peak (frames, parabolic, within +-48) traces the paths' mean
delay over T (the modulation's waveform as it runs from the engine's start), the peak's height how alike they stay.
Of the lag traces: their spectrum (lines at a rate and its harmonics: periodic; broad: random), the autocorrelation's
first peak (the period, its height: 1 periodic, low random), the left and right traces' correlation, and whether the
trace is repeatable (a second set of renders at T + 5 ms continues the same curve: the modulation free-runs from the
engine's start, not from the input). Prints a summary; --out writes the traces."""
import sys, json
import numpy as np
import rvlib as R

FS = R.FS
WIN = ((0.015, 0.045), (0.06, 0.12), (0.15, 0.25))
TS = np.arange(0, 3.0, 0.01)


def responses(q, room, decay, ts):
    exe, env = R.engines(q)
    items = []
    for t in ts:
        at = int(round(t * FS)) + 256
        n = at + int(0.3 * FS)
        x = np.zeros((n, 2)); x[at, :] = 0.5
        items.append((R.patch(room=room, decay=decay), [('run', R.mlab.units(n / FS + 0.05))], x, 30, env))
    ys = R.mlab.render_many(exe, items, workers=4)
    return [y[int(round(t * FS)) + 256:int(round(t * FS)) + 256 + int(0.28 * FS)] for t, y in zip(ts, ys)]


def lagtrace(ys, ch, w):
    a, b = int(w[0] * FS), int(w[1] * FS)
    ref = ys[0][a:b, ch]
    lags, peaks = [], []
    for y in ys:
        s = y[a - 64:b + 64, ch]
        c = np.correlate(s, ref, 'valid')            # (lag -64 .. 64)
        k = np.arange(len(c)) - 64
        m = np.abs(k) <= 48
        i = int(np.argmax(np.where(m, c, -np.inf)))
        if 0 < i < len(c) - 1:
            y0, y1, y2 = c[i - 1], c[i], c[i + 1]
            d = 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2 + 1e-30)
        else:
            d = 0.0
        lags.append(k[i] + d)
        peaks.append(c[i] / (np.sqrt(np.dot(ref, ref) * np.dot(y[a:b, ch], y[a:b, ch])) + 1e-30))
    return np.array(lags), np.array(peaks)


def analyse(tr, fs=100.0):
    x = tr - tr.mean()
    X = np.abs(np.fft.rfft(x * np.hanning(len(x)), 8 * len(x))) ** 2
    f = np.fft.rfftfreq(8 * len(x), 1 / fs)
    band = (f > 0.15) & (f < 20)
    pk = float(f[band][np.argmax(X[band])])
    near = band & (np.abs(f - pk) < 0.08)
    harm = sum(X[band & (np.abs(f - h * pk) < 0.08)].sum() for h in range(2, 6))
    line = float((X[near].sum() + harm) / X[band].sum())          # (the share in the line and its harmonics)
    ac = np.correlate(x, x, 'full')[len(x) - 1:]; ac /= ac[0] + 1e-30
    j0 = np.argmax(ac < 0) if (ac < 0).any() else 1
    j = j0 + int(np.argmax(ac[j0:len(x) // 2])) if j0 < len(x) // 2 else 0
    h2 = X[band & (np.abs(f - 2 * pk) < 0.08)].sum() / (X[near].sum() + 1e-30)
    h3 = X[band & (np.abs(f - 3 * pk) < 0.08)].sum() / (X[near].sum() + 1e-30)
    return dict(rms=float(np.sqrt(np.mean(x ** 2))), pp=float(x.max() - x.min()), rate=pk, line=line,
                period=j / fs, acpk=float(ac[j]) if j else 0.0, h2=float(h2), h3=float(h3))


if __name__ == '__main__':
    q = sys.argv[1]
    args = [a for a in sys.argv[2:] if not a.startswith('--')]
    out = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else None
    if out in args: args.remove(out)
    sets = [(int(args[i]), int(args[i + 1])) for i in range(0, len(args), 2)] or [(1, 112), (3, 112)]
    res = {}
    for room, decay in sets:
        ys = responses(q, room, decay, TS)
        ys2 = responses(q, room, decay, TS[:60] + 0.005)
        r = {}
        for wi, w in enumerate(WIN):
            tl, pl = lagtrace(ys, 0, w); tr, pr = lagtrace(ys, 1, w)
            al, ar = analyse(tl), analyse(tr)
            t2, _ = lagtrace([ys[0]] + ys2, 0, w)
            # (repeatable: the T + 5 ms set lies on the first set's curve)
            mid = 0.5 * (tl[:60] + tl[1:61])
            rep = float(np.corrcoef(t2[1:], mid)[0, 1]) if np.std(mid) > 1e-6 else float('nan')
            lr = float(np.corrcoef(tl, tr)[0, 1]) if np.std(tl) > 1e-6 and np.std(tr) > 1e-6 else float('nan')
            r['w%d' % wi] = dict(L=al, R=ar, lr=lr, rep=rep, corr_min=float(min(pl.min(), pr.min())),
                                 trace_l=[float(v) for v in tl], trace_r=[float(v) for v in tr])
            print('%s room %d decay %3d  %3.0f-%3.0f ms  L: rms %.2f fr, pp %.1f, peak %.2f Hz, line+harm %.2f, '
                  'period %.2f s (ac %.2f), h2 %.2f h3 %.2f | R: rate %.2f line %.2f | L/R %+.2f | repeat %.2f | '
                  'alike >= %.2f' % (q, room, decay, w[0] * 1e3, w[1] * 1e3, al['rms'], al['pp'], al['rate'], al['line'],
                                     al['period'], al['acpk'], al['h2'], al['h3'], ar['rate'], ar['line'], lr, rep,
                                     min(pl.min(), pr.min())), flush=True)
        res['r%d_d%d' % (room, decay)] = r
    if out:
        json.dump(res, open(out, 'w'))
