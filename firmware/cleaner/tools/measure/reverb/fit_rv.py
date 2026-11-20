# SPDX-License-Identifier: GPL-3.0-only
"""fit_rv.py [STEPS]: the own reverb's fitted constants (sy_cln_rv.c CP_*) from black-box renders of the development engine's
reverb (rvlib.py OLD; FULL and HALF) against the own one's measurement build (rvbuild.sh new_full, CLN_RV_TUNE).
Steps (default all, in order): t60 (the decay table CP_T60 and the rooms' loop factors CP_K), damp (the damping
absorption by room: the 8 kHz octave's extra decay, dB/s), half (HALF's), gain (the wet level), colour (the
colour's corners). The constants so far in fit_rv.json (scratch); prints the C table at the end."""
import os, sys, json
import numpy as np
import rvlib as R
import rvana as A

NAMES = (['DG1', 'DG2', 'DG3', 'DG4', 'GM0', 'GM1', 'GM2', 'GM3', 'LFO_HZ', 'MOD_PP', 'DAMP0', 'DAMP1', 'DAMP2', 'DAMP3',
          'K0', 'K1', 'K2', 'K3', 'GAIN', 'GLIDE', 'COL_LP0', 'COL_LP1', 'COL_HP0', 'COL_HP1', 'DAMPH0', 'DAMPH1', 'DAMPH2',
          'DAMPH3', 'LFOPH', 'LSC'] + ['T%d_%d' % (r, i) for r in range(4) for i in range(18)] + ['GR%d' % r for r in range(4)]
         + ['TAP%d' % r for r in range(4)] + ['ER%d' % r for r in range(4)] + ['MODR%d' % r for r in range(4)] + ['GH']
         + ['GL%d_%d' % (r, i) for r in range(4) for i in range(18)] + ['FBS', 'MOD2', 'ROT', 'ROTPH'])
SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'engine', 'sy_cln_rv.c')
NODES = [8 * i for i in range(16)] + [124, 126]
ST = os.path.join(R.SCR, 'fit_rv.json')
MID = (250, 500, 1000, 2000)


def source_values():
    """the constants as the source has them (its FIT table; RVT0: 18 zeros)"""
    import re
    s = open(SRC).read()
    t = s[s.index('/* FIT BEGIN'):s.index('/* FIT END */')]
    t = re.sub(r'/\*.*?\*/', '', t, flags=re.S).replace('RVT0', ','.join(['0.f'] * 18))
    v = [float(x.rstrip('f')) for x in re.findall(r'-?[\d.]+(?:e-?\d+)?f', t)]
    assert len(v) == len(NAMES), (len(v), len(NAMES))
    return dict(zip(NAMES, v))


def load():
    return json.load(open(ST)) if os.path.exists(ST) else source_values()


def env_of(P, half=False):
    e = {'SY_RVQ': '0' if half else '1'}
    if P:
        e['CLN_RV_TUNE'] = ','.join('%d=%.7g' % (NAMES.index(k), v) for k, v in P.items())
    return e


def secs(t, d=0):
    """a render long enough for T30 (the development engine's longest decays bend: their tails slower than their starts)"""
    return float(90 if d >= 120 else 60 if d >= 104 else min(32, max(1.5, 0.75 * t + 1)))


def tmid(y):
    t = A.t60_bands(y[640:], MID)
    v = [t[f]['t30'] if np.isfinite(t[f]['t30']) else t[f]['t20'] for f in MID]
    return float(np.exp(np.mean(np.log(v))))


def render(exe, env, items):
    return R.mlab.render_many(exe, [(p, [('run', R.mlab.units(s))], R.impulse(s), 30, env) for p, s in items])


def old_cache(key, fn):
    c = os.path.join(R.SCR, 'fit_old.json')
    d = json.load(open(c)) if os.path.exists(c) else {}
    if key not in d:
        d[key] = fn(); json.dump(d, open(c, 'w'))
    return d[key]


def t60_old():
    tt = [0.15, 0.29, 0.45, 0.60, 0.74, 0.95, 1.17, 1.33, 1.58, 1.85, 2.25, 2.85, 3.6, 4.9, 8.0, 12.0, 18.0, 40.0]
    items = [(R.patch(room=r, decay=d), secs(t, d)) for r in range(4) for d, t in zip(NODES, tt)]
    ys = render(R.OLD, {'SY_RVQ': '1'}, items)
    return [tmid(y) for y in ys]


def step_t60(P):
    old = np.array(old_cache('t60', t60_old)).reshape(4, 18)
    for it in range(3):
        items = [(R.patch(room=r, decay=d), secs(P['T%d_%d' % (r, i)], d)) for r in range(4) for i, d in enumerate(NODES)]
        new = np.array([tmid(y) for y in render(R.NEW, env_of(P), items)]).reshape(4, 18)
        lr = np.log(old / new)
        for r in range(4):
            for i in range(18): P['T%d_%d' % (r, i)] *= float(np.exp(lr[r, i]))
        print('t60 it', it, 'max |err| %.2f %%' % (100 * np.abs(lr).max()), 'mean %.2f %%' % (100 * np.abs(lr).mean()), flush=True)
        if np.abs(lr).max() < 0.005:
            break


def hf(y, bands=(4000, 8000, 16000)):
    t = A.t60_bands(y[640:], MID + bands)
    m = np.exp(np.mean(np.log([t[f]['t30'] for f in MID])))
    return [60 / t[f]['t30'] - 60 / m for f in bands]   # (extra decay, dB/s)


def step_damp(P, half=False):
    """per room the damping corner (HALF: its factor) by a secant search in log2 on the 8 kHz octave's extra decay
    (dB/s over the mid octaves' rate), the mean of decays 96, 112, 120"""
    D = (96, 112, 120)
    key = 'hf_half' if half else 'hf'
    old = np.array(old_cache(key, lambda: [hf(y) for y in render(R.OLD, {'SY_RVQ': '0' if half else '1'},
                                                                  [(R.patch(room=r, decay=d), 14) for r in range(4) for d in D])])).reshape(4, 3, 3)
    nm = ['DAMPH%d' % r if half else 'DAMP%d' % r for r in range(4)]
    tgt = np.log(old[:, :, 1].mean(1))
    hist = [[] for _ in range(4)]
    for it in range(5):
        new = np.array([hf(y) for y in render(R.NEW, env_of(P, half), [(R.patch(room=r, decay=d), 14) for r in range(4) for d in D])]).reshape(4, 3, 3)
        cur = np.log(np.maximum(new[:, :, 1].mean(1), 1e-3))
        print(key, 'it', it, '8k old', np.round(old[:, :, 1].mean(1), 2).tolist(), 'new', np.round(new[:, :, 1].mean(1), 2).tolist(),
              '4k', np.round(old[:, :, 0].mean(1), 2).tolist(), np.round(new[:, :, 0].mean(1), 2).tolist(), flush=True)
        if np.abs(cur - tgt).max() < 0.005:
            break
        for r in range(4):
            x = np.log2(P[nm[r]]); hist[r].append((x, cur[r]))
            if len(hist[r]) >= 2 and abs(hist[r][-1][1] - hist[r][-2][1]) > 1e-3:
                (x0, y0), (x1, y1) = hist[r][-2], hist[r][-1]
                xn = x1 + (tgt[r] - y1) * (x1 - x0) / (y1 - y0)
            else:
                xn = x + (tgt[r] - cur[r]) / np.log(2)   # (the extra decay ~ the constant)
            P[nm[r]] = float(2 ** np.clip(xn, x - 1.5, x + 1.5))


def levels(y):
    t = A.t60_bands(y[640:])
    return [t[f]['lev'] for f in t]


def step_gain(P):
    items = [(R.patch(room=r, decay=d), 4) for r in range(4) for d in (32, 64, 96)]
    old = np.array(old_cache('lev', lambda: [levels(y) for y in render(R.OLD, {'SY_RVQ': '1'}, items)]))
    new = np.array([levels(y) for y in render(R.NEW, env_of(P), items)])
    d = (old - new)[:, 1:8].reshape(4, 3, 7).mean((1, 2))
    for r in range(4): P['GR%d' % r] = P.get('GR%d' % r, 1.0) * float(10 ** (d[r] / 20))
    d = d.mean()
    print('gain: %.3f dB' % d, 'per room', np.round((old - new)[:, 1:8].reshape(4, 3, 7).mean((1, 2)), 2).tolist())


def envl(y):
    e = np.array([10 * np.log10(np.mean(y[k:k + 200] ** 2) + 1e-14) for k in range(0, 4000, 200)])
    return e - e.max()


def step_taps(P):
    """per room the first taps' gain, the further early taps' and the lines' ends' (the late sum's): the early envelope
    (200-frame windows over 85 ms, decay 0 and 64) nearest the development engine's"""
    D = (0, 64)
    old = np.array(old_cache('envl', lambda: [list(envl(y[640:].sum(1))) for y in render(R.OLD, {'SY_RVQ': '1'},
                                                [(R.patch(room=r, decay=d), 1.2) for r in range(4) for d in D])])).reshape(4, 2, 20)
    T0 = (0.5, 0.7, 1.0, 1.4, 2.0, 2.8); GM = (0.06, 0.1, 0.15, 0.22, 0.32); ER = (0.3, 0.6, 1.0, 1.5, 2.2)
    for r in range(4):
        grid = [(t0, gm, er) for t0 in T0 for gm in GM for er in ER]
        items = []
        for t0, gm, er in grid:
            Q = dict(P); Q['TAP%d' % r] = t0; Q['GM%d' % r] = gm; Q['ER%d' % r] = er
            items += [(R.patch(room=r, decay=d), [('run', R.mlab.units(1.2))], R.impulse(1.2), 30, env_of(Q)) for d in D]
        ys = R.mlab.render_many(R.NEW, items, workers=4)
        best = None
        for k, g in enumerate(grid):
            # (the shape: each render's envelope against the development engine's, their mean offset is the level's, fitted apart)
            e = []
            for i in range(2):
                d = envl(ys[2 * k + i][640:].sum(1)) - old[r, i]
                e.append(np.mean((d - d.mean()) ** 2))
            e = float(np.mean(e))
            if best is None or e < best[0]: best = (e,) + g
        P['TAP%d' % r], P['GM%d' % r], P['ER%d' % r] = best[1], best[2], best[3]
        print('taps room', r, 'first %.2f late %.2f early %.2f' % best[1:], 'rms %.2f dB' % np.sqrt(best[0]), flush=True)


def step_wobble(P):
    """per room the LFO's depth: the tail's line spread and IF deviation (wobble.py, the medians over eight sines,
    decay 112) nearest the development engine's (FULL)"""
    import wobble
    old = old_cache('wobble', lambda: {str(r): list(map(float, v)) for r, v in wobble.run('full', (0, 1, 2, 3)).items()})
    for r in range(4):
        best = None
        for f in (1.0, 1.4, 1.8, 2.3, 2.9, 3.6):
            Q = dict(P); Q['LFOR%d' % r] = f
            y = R.mlab.render(R.NEW, R.patch(room=r, decay=112), [('run', R.mlab.units(7))], inp=wobble.tones(), env=env_of(Q))
            m = wobble.measure(y[640:])
            e = (np.log(m[0] / old[str(r)][0])) ** 2 + (np.log(m[1] / old[str(r)][1])) ** 2
            if best is None or e < best[0]: best = (e, f, m)
        P['LFOR%d' % r] = best[1]
        print('wobble room', r, 'depth x%.2f' % best[1], 'spread / IF dev / LFO share new', np.round(best[2], 2).tolist(),
              'old', np.round(old[str(r)], 2).tolist(), flush=True)


def midlev(y):
    t = A.t60_bands(y[640:], MID)
    return float(np.mean([t[f]['lev'] for f in MID]))


def step_levels(P):
    """by room and decay node the wet level (CP_GL, dB): the mid octaves' energy of the response as the development engine's"""
    items = [(R.patch(room=r, decay=d), secs(P['T%d_%d' % (r, i)], d)) for r in range(4) for i, d in enumerate(NODES)]
    old = np.array(old_cache('levn', lambda: [midlev(y) for y in render(R.OLD, {'SY_RVQ': '1'}, items)])).reshape(4, 18)
    for it in range(2):
        new = np.array([midlev(y) for y in render(R.NEW, env_of(P), items)]).reshape(4, 18)
        d = old - new
        for r in range(4):
            for i in range(18): P['GL%d_%d' % (r, i)] = P.get('GL%d_%d' % (r, i), 0.0) + float(d[r, i])
        print('levels it', it, 'max |diff| %.2f dB' % np.abs(d).max(), flush=True)
    normalise(P)


def normalise(P):
    """each room's median wet level (CP_GL, dB) moved into its gain (CP_GR): the product the same, GL about 0"""
    for r in range(4):
        m = float(np.median([P['GL%d_%d' % (r, i)] for i in range(18)]))
        for i in range(18): P['GL%d_%d' % (r, i)] -= m
        P['GR%d' % r] = P.get('GR%d' % r, 1.0) * float(10 ** (m / 20))


def step_halfgain(P):
    """HALF's wet gain: the mid octaves' level of HALF as the development engine's HALF (rooms 0-3, decays 32, 64, 96)"""
    items = [(R.patch(room=r, decay=d), 4) for r in range(4) for d in (32, 64, 96)]
    old = np.array(old_cache('levh', lambda: [midlev(y) for y in render(R.OLD, {'SY_RVQ': '0'}, items)]))
    new = np.array([midlev(y) for y in render(R.NEW, env_of(P, True), items)])
    d = float(np.mean(old - new))
    P['GH'] = P.get('GH', 1.0) * 10 ** (d / 20)
    print('half gain: %.3f dB' % d, 'spread', np.round(old - new, 2).tolist())


COLS = (0, 8, 16, 24, 32, 40, 48, 56, 63, 65, 72, 80, 88, 96, 104, 112, 120, 127)


def colour_curves(exe, env):
    ys = render(exe, env, [(R.patch(room=3, decay=32, colour=c), 1.5) for c in COLS + (64,)])
    _, ref = R.mlab.band_db(ys[-1][640:640 + 9000], 2048)
    return [list(R.mlab.band_db(y[640:640 + 9000], 2048)[1] - ref) for y in ys[:-1]]


def step_colour(P):
    old = np.array(old_cache('colour', lambda: colour_curves(R.OLD, {'SY_RVQ': '1'})))
    cs = 1000 * 2 ** (np.arange(-16, 14) / 3)
    from scipy.optimize import minimize_scalar
    # (per colour value the one-pole's corner that fits its curve best; then the line of log2 corners)
    def curve(fc, hp):
        z = np.exp(2j * np.pi * cs / R.FS)
        g = np.tan(np.pi * fc / R.FS); a = g / (1 + g)
        Hlp = a * (1 + 1 / z) / (1 - (1 - 2 * a) / z)
        H = 1 - Hlp if hp else Hlp
        return 20 * np.log10(np.abs(H) + 1e-12)
    m = (cs > 40) & (cs < 18000)
    lo, hi = [], []
    for c, o in zip(COLS, old):
        hp = c > 64
        ok = m & (o > -45)
        f = lambda l2: np.mean((curve(2 ** l2, hp)[ok] - o[ok]) ** 2)
        r = minimize_scalar(f, bounds=(3, 15), method='bounded')
        (hi if hp else lo).append((c, r.x, f(r.x)))
    cl = np.array([c for c, _, _ in lo]); vl = np.array([v for _, v, _ in lo])
    ch = np.array([c for c, _, _ in hi]); vh = np.array([v for _, v, _ in hi])
    kl = np.polyfit(cl, vl, 1); kh = np.polyfit(ch, vh, 1)
    P['COL_LP0'] = float(np.polyval(kl, 0)); P['COL_LP1'] = float(np.polyval(kl, 63))
    P['COL_HP0'] = float(np.polyval(kh, 65)); P['COL_HP1'] = float(np.polyval(kh, 127))
    print('colour: low pass log2 fc', np.round(vl, 2).tolist(), 'line resid %.3f' % np.std(vl - np.polyval(kl, cl)))
    print('        high pass', np.round(vh, 2).tolist(), 'line resid %.3f' % np.std(vh - np.polyval(kh, ch)))


if __name__ == '__main__':
    P = load()
    steps = sys.argv[1].split(',') if len(sys.argv) > 1 else ['taps', 't60', 'damp', 'half', 't60', 'gain', 'colour']
    for s in steps:
        {'t60': step_t60, 'damp': step_damp, 'half': lambda P: step_damp(P, True), 'gain': step_gain,
         'colour': step_colour, 'taps': step_taps, 'wobble': step_wobble, 'levels': step_levels, 'halfgain': step_halfgain, 'normalise': normalise}[s](P)
        json.dump(P, open(ST, 'w'), indent=0)
    print('\n'.join('%s %.6g' % (k, P[k]) for k in NAMES))
