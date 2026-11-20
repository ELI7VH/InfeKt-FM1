# SPDX-License-Identifier: GPL-3.0-only
"""fit_shim.py [fit|tail|share|cmp]: FX > SHIMMER on the own reverb against the development engine's as a black box (rvlib.py, HALF): a
noise burst low-passed at 500 Hz (0.4 s) into the reverb, the octave bands 1-8 kHz (where only the shimmer puts energy)
per 0.25 s window over 8 s, shimmer on less the same window with it off (dB). fit: the reverb's energy gain by room
(SP_EK, TAIL FREE, OCT) and the lists' factors (SP_FAC, room 3) by a secant search on the mean band level over
the windows 1-4 s; tail: TAIL LIMITED's gain (SP_TLK) and SWELL's ramp (SP_SWT) by a grid each (SP_TLR, the peak's fall, by the
listening case 07's shimmer share over 2-9 s: share); cmp: the table for every setting. The constants in fit_shim.json (scratch)."""
import os, sys, json
import numpy as np
from scipy import signal
import rvlib as R
import rvana as A
FS = R.FS
ST = os.path.join(R.SCR, 'fit_shim.json')
SPN = ['FAC1', 'FAC2', 'FAC3', 'FAC4', 'FAC5', 'EK0', 'EK1', 'EK2', 'EK3', 'RND', 'DMIN', 'GOV', 'GUP', 'TLK', 'SWT', 'TLR']
BANDS = (1000, 2000, 4000, 8000)
SECS = 8


def burst():
    rng = np.random.default_rng(7)
    x = rng.standard_normal(int(0.4 * FS))
    x = signal.sosfilt(signal.butter(4, 500, fs=FS, output='sos'), x)
    x *= 0.25 / np.sqrt(np.mean(x ** 2)) * 0.5
    out = np.zeros((int(SECS * FS), 2)); out[:len(x), 0] = out[:len(x), 1] = x
    return out


def bands(y, win=0.25):
    n = int(win * FS); out = []
    bs = [A.bandpass(y, f) for f in BANDS]
    for s in range(0, len(y) - n, n):
        out.append([10 * np.log10((b[s:s + n] ** 2).sum() + 1e-20) for b in bs])
    return np.array(out)          # (windows, bands)


def run(exe, env, cases):
    x = burst()
    jobs = []
    for p, sh in cases:
        e = dict(env); e['SY_SHIM'] = sh
        jobs.append((p, [('run', R.mlab.units(SECS))], x, 30, e))
    return [bands(y[640:]) for y in R.mlab.render_many(exe, jobs)]


def env_new(P):
    import fit_rv
    F = json.load(open(fit_rv.ST)) if os.path.exists(fit_rv.ST) else {}
    e = fit_rv.env_of(F, True)
    if P:
        e['CLN_RVS_TUNE'] = ','.join('%d=%.7g' % (SPN.index(k), v) for k, v in P.items())
    return e


CASES = [((r, d), '%d,%d,%d,%d,0,0,%d,%d' % (a, l, g, t, tail, sw)) for (r, d, a, l, g, t, tail, sw) in [
    (3, 100, 127, 0, 100, 64, 1, 0), (3, 100, 64, 0, 100, 64, 1, 0), (0, 100, 127, 0, 100, 64, 1, 0),
    (1, 100, 127, 0, 100, 64, 1, 0), (2, 100, 127, 0, 100, 64, 1, 0), (3, 80, 127, 0, 100, 64, 1, 0),
    (3, 112, 127, 0, 100, 64, 1, 0), (3, 100, 127, 1, 100, 64, 1, 0), (3, 100, 127, 2, 100, 64, 1, 0),
    (3, 100, 127, 3, 100, 64, 1, 0), (3, 100, 127, 4, 100, 64, 1, 0), (3, 100, 127, 0, 20, 64, 1, 0),
    (3, 100, 127, 0, 127, 127, 1, 0), (3, 100, 127, 0, 100, 0, 1, 0), (3, 100, 127, 0, 100, 64, 0, 0),
    (3, 100, 127, 0, 100, 64, 0, 57), (3, 100, 127, 0, 100, 64, 0, 127), (0, 120, 100, 1, 60, 96, 0, 57)]]


def table(P, cases=CASES):
    po = [R.patch(room=r, decay=d) for (r, d), _ in cases]
    old_on = run(R.OLD, {'SY_RVQ': '0'}, [(p, sh) for p, (_, sh) in zip(po, cases)])
    old_off = run(R.OLD, {'SY_RVQ': '0'}, [(p, '0,0,100,64,0,0,1,0') for p in po])
    new_on = run(R.NEW, env_new(P), [(p, sh) for p, (_, sh) in zip(po, cases)])
    new_off = run(R.NEW, env_new(P), [(p, '0,0,100,64,0,0,1,0') for p in po])
    return [(a - b, c - d, a, c) for a, b, c, d in zip(old_on, old_off, new_on, new_off)]


def summary(t, w=slice(4, 16)):
    o, n, oa, na = t
    return float(np.mean(oa[w])), float(np.mean(na[w]))


def share(P):
    """listen_set.py's case 07 (a pad, SHIMMER 100 with TAIL LIMITED and SWELL 57): per second from 2.5 s the level, the
    shimmer's own part (on less off: the renders are deterministic) against it (dB) and that part's graininess (the
    std of its 5 ms envelope about the 100 ms trend at 2 kHz, dB), the development engine's and the own one's"""
    import listen_set as L
    nm, q, pf, ev, inp, sh = L.CASES[6]
    off = '0' + sh[sh.index(','):]
    for lab, exe, env in (('target', R.OLD, {'SY_RVQ': '0'}), ('own', R.NEW, env_new(P))):
        y1 = R.mlab.render(exe, pf(), ev, env=dict(env, SY_SHIM=sh)); y0 = R.mlab.render(exe, pf(), ev, env=dict(env, SY_SHIM=off))
        d = y1 - y0; out = []
        for t0 in (2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5):
            a, b = int(t0 * FS), int((t0 + 1) * FS)
            lt = 10 * np.log10((y1[a:b] ** 2).sum() + 1e-30); ld = 10 * np.log10((d[a:b] ** 2).sum() + 1e-30)
            bb = A.bandpass(d[a - 5000:b + 5000], 2000).sum(1)[5000:-5000]
            le = 10 * np.log10(np.convolve(bb ** 2, np.ones(234) / 234, 'same') + 1e-30)
            am = np.std((le - np.convolve(le, np.ones(4687) / 4687, 'same'))[2400:-2400])
            out.append('%.1f s %6.1f %5.1f %4.1f' % (t0, lt, ld - lt, am))
        print('%-6s' % lab, ' | '.join(out), flush=True)


if __name__ == '__main__':
    P = json.load(open(ST)) if os.path.exists(ST) else {}
    what = sys.argv[1] if len(sys.argv) > 1 else 'cmp'
    if what == 'fit':
        K = float(os.environ.get('SHIM_STEP', '0.6'))   # (a smaller step where the loop's level is steep in E)
        for it in range(int(os.environ.get('SHIM_IT', '4'))):
            res = table(P, CASES[:11])
            o = np.array([summary(t)[0] for t in res]); n = np.array([summary(t)[1] for t in res])
            d = o - n                      # (dB: the old louder by d)
            # rooms (OCT, decay 100, AMOUNT 127): cases 0, 2, 3, 4 -> EK3, EK0, EK1, EK2
            for ci, r in ((0, 3), (2, 0), (3, 1), (4, 2)):
                P['EK%d' % r] = P.get('EK%d' % r, 1.0) * float(10 ** (-d[ci] / 10 * K))
            for ci, l in ((7, 2), (8, 3), (9, 4), (10, 5)):
                P['FAC%d' % l] = P.get('FAC%d' % l, 1.0) * float(10 ** (d[ci] / 10 * K))
            print('it', it, 'diff dB', np.round(d, 2).tolist(), flush=True)
            json.dump(P, open(ST, 'w'))
    if what == 'share':
        share(P); sys.exit(0)
    if what == 'tail':   # (TAIL LIMITED's gain: case 14; SWELL's ramp: case 15, at 57; a grid each: the loop's
        # level is too steep in either for a secant; case 16, SWELL 127, is the reverb's own floor in these bands)
        for key, grid, ci in (('TLK', (0.9, 1.0, 1.1, 1.2, 1.3), 14), ('SWT', (2.0, 3.0, 4.0, 5.0, 6.0), 15)):
            best = None
            for v in grid:
                Q = dict(P); Q[key] = v
                o, n = summary(table(Q, CASES[ci:ci + 1])[0])
                print('tail', key, v, 'diff dB %.2f' % (o - n), flush=True)
                if best is None or abs(o - n) < best[0]: best = (abs(o - n), v)
            P[key] = best[1]
            json.dump(P, open(ST, 'w'))
    res = table(P)
    for (c, sh), t in zip(CASES, res):
        o, n, oa, na = t
        print('room %d decay %3d SHIM %-22s on-off 1-4 s: old %6.1f new %6.1f | abs old %6.1f new %6.1f | late (5-8 s) old %6.1f new %6.1f'
              % (c[0], c[1], sh, np.mean(o[4:16]), np.mean(n[4:16]), np.mean(oa[4:16]), np.mean(na[4:16]),
                 np.mean(oa[20:32]), np.mean(na[20:32])))
