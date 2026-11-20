# SPDX-License-Identifier: GPL-3.0-only
"""modtime_rv.py Q OUT.json [GROUPS] | modtime_rv.py --cmp OLD.json NEW.json [--md]: the tail's pitch modulation over
time as a black box (rvlib.py; Q = full, half, half16 of the development engine's renderer, nfull, nhalf, nhalf16 of the
cleaner's; CLN_RV_TUNE in the environment passes on to the own one). The tail is cut into windows by its level against
its start (W: 0-15, 15-30, 30-45 dB down: early, middle, end); per window and per partial the instantaneous frequency
(rvana.inst_freq, a band of 2 %) in cents, and of it:
  dev    the RMS deviation (cents)
  rate   the IF spectrum's peak (Hz, 0.2-20 Hz: the modulation's rate)
  irr    irregularity: 1 - the share of the IF's power within +-30 % of that peak (0: one clean sine, 1: noise)
  fast   the share of the IF's power at 4-20 Hz (fast flutter, grain)
the medians over the partials (and both sides). Groups (default all):
  sines   eight sines (wobble.py) 1 s into modes 2 (rooms 0-3), 4 (room 1), decay 112 and 120
  cases   listen_set.py's eight cases from the last note off, the partials the window's eight largest peaks
          (200 Hz-5 kHz, a third of an octave apart; case 07 with the shimmer)
--cmp: the table of both side by side."""
import sys, os, json
import numpy as np
from scipy import signal
import rvlib as R
import rvana as A
import wobble
FS = R.FS
W = ((0, 15), (15, 30), (30, 45))
WN = ('early', 'middle', 'end')


def env_db(y, t0):
    s = (y[t0:] ** 2).sum(1)
    n = int(0.05 * FS)
    e = np.array([s[i:i + n].mean() for i in range(0, len(s) - n, n)])
    return 10 * np.log10(e + 1e-30), n


def windows(y, t0):
    """(a, b) frames of each W window: where the 50 ms level (smoothed 0.3 s) first falls below the bounds"""
    le, n = env_db(y, t0)
    le = np.convolve(le, np.ones(6) / 6, 'same')
    ref = le[2:8].max()
    out = []
    for lo, hi in W:
        i = np.nonzero(le < ref - lo)[0]; j = np.nonzero(le < ref - hi)[0]
        a = t0 + (int(i[0]) if len(i) else len(le)) * n
        b = t0 + (int(j[0]) if len(j) else len(le)) * n
        out.append((a, b) if b - a > int(0.6 * FS) else None)
    return out


def if_stats(x, f, a, b):
    fi, en = A.inst_freq(x, f, bw=0.02 * f)
    g = fi[a:b]; w = en[a:b]
    k = w > 0.1 * np.median(w)
    if k.mean() < 0.5:
        return None
    c = 1200 * np.log2(np.maximum(g, 1) / f)
    med = np.median(c[k]); c = np.where(k, c, med) - med
    dev = float(np.sqrt(np.mean(c[k] ** 2)))
    cc = signal.decimate(signal.decimate(c, 10), 10)
    fs = FS / 100
    X = np.abs(np.fft.rfft((cc - cc.mean()) * np.hanning(len(cc)), 8 * len(cc))) ** 2
    fr = np.fft.rfftfreq(8 * len(cc), 1 / fs)
    band = (fr > 0.2) & (fr < 20)
    tot = X[band].sum() + 1e-30
    pk = float(fr[band][np.argmax(X[band])])
    irr = 1 - X[band & (fr > 0.7 * pk) & (fr < 1.3 * pk)].sum() / tot
    fast = X[band & (fr > 4)].sum() / tot
    return dev, pk, float(irr), float(fast)


def peaks(y, a, b, n=8):
    m = y[a:b].sum(1)
    f, P = signal.welch(m, FS, nperseg=8192)
    ok = (f > 200) & (f < 5000)
    idx = [i for i in np.argsort(P * ok)[::-1] if ok[i]]
    out = []
    for i in idx:
        if all(abs(np.log2(f[i] / g)) > 1 / 3 for g in out):
            out.append(float(f[i]))
        if len(out) >= n:
            break
    return out


def measure(y, t0, freqs=None):
    res = {}
    for nm, ab in zip(WN, windows(y, t0)):
        if ab is None:
            continue
        a, b = ab
        fs_ = freqs or peaks(y, a, b)
        st = [s for c in (0, 1) for f in fs_ for s in [if_stats(y[:, c], f, a, b)] if s]
        if len(st) < 3:
            continue
        st = np.median(np.array(st), 0)
        res[nm] = dict(t=[a / FS, b / FS], dev=float(st[0]), rate=float(st[1]), irr=float(st[2]), fast=float(st[3]))
    return res


def g_sines(exe, env):
    items, keys = [], []
    for m, r in ((2, 0), (2, 1), (2, 2), (2, 3), (4, 1)):
        for d in (112, 120):
            items.append((R.patch(mode=m, room=r, decay=d), [('run', R.mlab.units(16 if d == 120 else 10))],
                          wobble.tones(16 if d == 120 else 10), 30, env))
            keys.append('m%d_r%d_d%d' % (m, r, d))
    return {k: measure(y[640:], int(1.05 * FS), list(wobble.F)) for k, y in zip(keys, R.mlab.render_many(exe, items))}


def g_cases(new, tune):
    import listen_set as L
    res = {}
    for i, (nm, q, pf, ev, inp, sh) in enumerate(L.CASES):
        exe, env = R.engines(('n' if new else '') + q)
        env = dict(env, **tune)
        if sh:
            env['SY_SHIM'] = sh
        y = R.mlab.render(exe, pf(), ev, inp=inp() if inp else None, env=env)
        t = 0.0; toff = None
        for e in ev:
            if e[0] == 'run': t += e[1] * R.mlab.BLOCK / FS
            if e[0] == 'off': toff = t
        if toff is None: toff = 2.3
        res['%02d_%s' % (i + 1, nm)] = measure(y, int((toff + 0.05) * FS))
    return res


def cmp(a, b, md=False):
    for g in ('sines', 'cases'):
        if g not in a or g not in b:
            continue
        if md:
            print('| %s | window | dev (cent) hyb / own | rate (Hz) | irregularity | fast share |' % g)
            print('|---|---|---|---|---|---|')
        for k in a[g]:
            for w in WN:
                x, y = a[g][k].get(w), b[g].get(k, {}).get(w)
                if not x or not y:
                    continue
                if md:
                    print('| %s | %s | %.1f / %.1f | %.2f / %.2f | %.2f / %.2f | %.2f / %.2f |' % (
                        k, w, x['dev'], y['dev'], x['rate'], y['rate'], x['irr'], y['irr'], x['fast'], y['fast']))
                else:
                    print('%-28s %-6s dev %5.1f %5.1f  rate %5.2f %5.2f  irr %.2f %.2f  fast %.2f %.2f' % (
                        k, w, x['dev'], y['dev'], x['rate'], y['rate'], x['irr'], y['irr'], x['fast'], y['fast']))
        d = [np.log(b[g][k][w]['dev'] / a[g][k][w]['dev']) for k in a[g] for w in WN
             if a[g][k].get(w) and b[g].get(k, {}).get(w)]
        if d:
            print('%s: dev ratio own / development engine median %.2f, worst %.2f' % (g, np.exp(np.median(d)), np.exp(d[int(np.argmax(np.abs(d)))])))


if __name__ == '__main__':
    if sys.argv[1] == '--cmp':
        cmp(json.load(open(sys.argv[2])), json.load(open(sys.argv[3])), '--md' in sys.argv)
        sys.exit(0)
    q, out = sys.argv[1], sys.argv[2]
    groups = sys.argv[3].split(',') if len(sys.argv) > 3 else ['sines', 'cases']
    tune = {'CLN_RV_TUNE': os.environ['CLN_RV_TUNE']} if os.environ.get('CLN_RV_TUNE') else {}
    exe, env = R.engines(q); env = dict(env, **tune) if q.startswith('n') else env
    res = {}
    for g in groups:
        res[g] = g_sines(exe, env) if g == 'sines' else g_cases(q.startswith('n'), tune if q.startswith('n') else {})
        print(g, 'done', flush=True)
    json.dump(res, open(out, 'w'), indent=0)
