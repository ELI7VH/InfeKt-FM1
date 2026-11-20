# SPDX-License-Identifier: GPL-3.0-only
"""dens_rv.py Q OUT.json [GROUPS] | dens_rv.py --cmp OLD.json NEW.json: the reverb's density measured as a black box
(rvlib.py; Q = full, half, half16 of the development engine's renderer, nfull, nhalf, nhalf16 of the cleaner's; CLN_RV_TUNE in the
environment passes on to the own one). Per response:
  ned     echo density (Abel & Huang, 10 ms windows) 5, 10, 20, 40, 80, 160 ms after the first arrival
  early   the share of the energy in the first 80 ms (dB)
  per     the envelope's periodicity: the largest autocorrelation of the detrended log envelope (5.5 ms smoothing,
          85 ms trend) at a lag of 43-850 ms, and that lag (a tank whose sound circulates as a packet: near 1)
  grain   the tail's graininess: the standard deviation (dB) of the 2 ms envelope about its 50 ms trend, 50-800 ms
Groups (default all):
  ir      modes 2, 3, 4 x rooms 0-3 x decay 40 / 96 / 120 (modes 3, 4: A 115 64, A 114 24), an impulse
  clock   modes 3, 4, room 1, decay 104, A 115 64, the delay clock (B 20) 3 / 6 / 10 at tempo 100 and 140
  fbrep   modes 3, 4, room 1, decay 0 / 104, A 115 64, A 114 24: the repeats against each other (each repeat's
          correlation with the one before at the best lag within 300 frames, its sign; the repeats' spread 10-90 %
          energy, ms)
  cases   listen_set.py's eight cases (the whole output: the dry part with it), from the last note off: ned at
          10-800 ms, grain and per of the tail
--cmp: per metric the development engine's and the own one's medians and the largest difference, the worst case named."""
import sys, json
import numpy as np
from scipy import signal
import rvlib as R
import rvana as A
FS = R.FS


def periodicity(y, t0=0, secs=3.0):
    e = np.convolve((y ** 2).sum(1), np.ones(256) / 256, 'same')[t0:t0 + int(secs * FS)]
    le = np.log(e + 1e-12); le -= np.convolve(le, np.ones(4001) / 4001, 'same')
    le = le[2500:-2500]
    if len(le) < 42000:
        return 0.0, 0.0
    ac = signal.correlate(le, le, 'full', method='fft')[len(le) - 1:]; ac /= ac[0]
    k = 2000 + int(np.argmax(ac[2000:40000]))
    return float(k / FS * 1000), float(ac[k])


def grain(y, t0, a=0.05, b=0.8):
    s = y[t0 + int(a * FS):t0 + int(b * FS)].sum(1)
    n = 94
    k_ = len(s) // n; e = (s[:k_ * n].reshape(k_, n) ** 2).mean(1)
    le = 10 * np.log10(e + 1e-20)
    k = 25; tr = np.convolve(le, np.ones(k) / k, 'valid'); d = le[k // 2:k // 2 + len(tr)] - tr
    return float(np.std(d))


def ir(y):
    fa = A.first_arrival(y, -40)
    t, d = A.echo_density(y[:, 0] + y[:, 1], 0.01)
    t0 = fa / FS
    e = (y ** 2).sum(1)
    pl, pa = periodicity(y, fa)
    return dict(fa=fa, ned={str(ms): float(np.interp(t0 + ms / 1000, t, d)) for ms in (5, 10, 20, 40, 80, 160)},
                early=float(10 * np.log10(e[fa:fa + int(0.08 * FS)].sum() / (e.sum() + 1e-30) + 1e-30)),
                per=pa, per_ms=pl, grain=grain(y, fa))


def g_ir(exe, env):
    items, keys = [], []
    for m in (2, 3, 4):
        for r in (0, 1, 2, 3):
            for d in (40, 96, 120):
                fb = 64 if m > 2 else 0
                items.append((R.patch(mode=m, room=r, decay=d, fb=fb, time=24 if fb else 0), [('run', R.mlab.units(4))],
                              R.impulse(4), 30, env))
                keys.append('m%d_r%d_d%d' % (m, r, d))
    return {k: ir(y[640:]) for k, y in zip(keys, R.mlab.render_many(exe, items))}


def g_clock(exe, env):
    items, keys = [], []
    for m in (3, 4):
        for bpm in (100, 140):
            for c in (3, 6, 10):
                p = R.patch(mode=m, room=1, decay=104, fb=64, clock=c); p[R.mlab.PB + 16] = bpm - 63
                items.append((p, [('run', R.mlab.units(4))], R.impulse(4), 30, env))
                keys.append('m%d_t%d_c%d' % (m, bpm, c))
    return {k: ir(y[640:]) for k, y in zip(keys, R.mlab.render_many(exe, items))}


def g_fbrep(exe, env):
    res = {}
    for m in (3, 4):
        for d in (0, 104):
            y = R.run(exe, R.patch(mode=m, room=1, decay=d, time=24, fb=64), R.impulse(2), 2, env)[640:]
            P = 256 * 24 + 1
            fa = A.first_arrival(y, -40)
            fa = fa - P if m == 3 and fa > P else fa   # (mode 3: the first sound is the first repeat)
            rr, sp = [], []
            for k in range(1, 4):
                a = y[fa + k * P - 100:fa + (k + 1) * P - 100, 0]
                b = y[fa + (k + 1) * P - 400:fa + (k + 2) * P + 200, 0]
                if len(b) < len(a) + 600:
                    break
                c = signal.correlate(b, a, 'valid')
                nb = np.sqrt(np.convolve(b ** 2, np.ones(len(a)), 'valid'))
                c = c / (np.sqrt((a ** 2).sum()) * nb + 1e-30)
                j = int(np.argmax(np.abs(c))); rr.append(float(c[j]))
                es = (y[fa + k * P - 200:fa + (k + 1) * P - 200] ** 2).sum(1); cs = np.cumsum(es) / (es.sum() + 1e-30)
                sp.append(float((np.searchsorted(cs, 0.9) - np.searchsorted(cs, 0.1)) / FS * 1000))
            res['m%d_d%d' % (m, d)] = dict(r=float(np.mean(rr)), spread=float(np.mean(sp)))
    return res


def g_cases(exe_env_of):
    import listen_set as L
    res = {}
    for i, (nm, q, pf, ev, inp, sh) in enumerate(L.CASES):
        exe, env = exe_env_of(q)
        env = dict(env)
        if sh:
            env['SY_SHIM'] = sh
        y = R.mlab.render(exe, pf(), ev, inp=inp() if inp else None, env=env)
        # (the last note off, or the drum case's last hit, then the tail)
        t = 0.0; toff = None
        for e in ev:
            if e[0] == 'run': t += e[1] * R.mlab.BLOCK / FS
            if e[0] == 'off': toff = t
        if toff is None: toff = 2.25
        s = int(toff * FS)
        tt, d = A.echo_density(y[:, 0] + y[:, 1], 0.01)
        res['%02d_%s' % (i + 1, nm)] = dict(ned={str(ms): float(np.interp(toff + ms / 1000, tt, d)) for ms in (10, 40, 100, 200, 400, 800)},
                                           grain=grain(y, s, 0.1, 1.5), per=periodicity(y, s, 3.0)[1])
    return res


def cmp(a, b):
    def line(nm, xs, ys, keys):
        xs, ys = np.array(xs), np.array(ys); d = ys - xs; i = int(np.argmax(np.abs(d)))
        print('%-34s development engine %6.2f own %6.2f  median |diff| %5.2f  max %+6.2f (%s)' % (nm, np.median(xs), np.median(ys), np.median(np.abs(d)), d[i], keys[i]))
    for g in ('ir', 'clock'):
        if g not in a or g not in b: continue
        ks = [k for k in a[g] if k in b[g]]
        for ms in ('5', '10', '20', '40', '80', '160'):
            line('%s ned %s ms' % (g, ms), [a[g][k]['ned'][ms] for k in ks], [b[g][k]['ned'][ms] for k in ks], ks)
        for m in ('early', 'per', 'grain'):
            line('%s %s' % (g, m), [a[g][k][m] for k in ks], [b[g][k][m] for k in ks], ks)
    if 'fbrep' in a and 'fbrep' in b:
        ks = list(a['fbrep'])
        for m in ('r', 'spread'):
            line('fbrep %s' % m, [a['fbrep'][k][m] for k in ks], [b['fbrep'][k][m] for k in ks], ks)
    if 'cases' in a and 'cases' in b:
        for k in a['cases']:
            x, y = a['cases'][k], b['cases'][k]
            print('%-30s ned %s | %s  grain %.2f | %.2f  per %.2f | %.2f' % (k, ' '.join('%.2f' % v for v in x['ned'].values()),
                  ' '.join('%.2f' % v for v in y['ned'].values()), x['grain'], y['grain'], x['per'], y['per']))


if __name__ == '__main__':
    if sys.argv[1] == '--cmp':
        cmp(json.load(open(sys.argv[2])), json.load(open(sys.argv[3])))
        sys.exit(0)
    q, out = sys.argv[1], sys.argv[2]
    groups = sys.argv[3].split(',') if len(sys.argv) > 3 else ['ir', 'clock', 'fbrep', 'cases']
    import os
    tune = {'CLN_RV_TUNE': os.environ['CLN_RV_TUNE']} if os.environ.get('CLN_RV_TUNE') else {}
    exe, env = R.engines(q); env = dict(env, **tune) if q.startswith('n') else env
    res = {}
    for g in groups:
        if g == 'cases':
            new = q.startswith('n')
            res[g] = g_cases(lambda qq: (lambda e: (e[0], dict(e[1], **tune)))(R.engines(('n' if new else '') + qq)))
        else:
            res[g] = {'ir': g_ir, 'clock': g_clock, 'fbrep': g_fbrep}[g](exe, env)
        print(g, 'done', flush=True)
    json.dump(res, open(out, 'w'), indent=0)
