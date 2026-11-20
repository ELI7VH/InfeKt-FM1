# SPDX-License-Identifier: GPL-3.0-only
"""v6load.py: when the 6-voice state (synth.c v6_on, V6_LOAD; PROVENANCE.md 9g) sets in, and osc 2's taps in it, read
on the reference as a black box (its audio alone, MLAB_REFENG; MLAB_REFENG to point
elsewhere).

The method (as v6.py): one audible voice (key KEY, velocity 127, velocity -> amp 127, unison off, chorus and send 0)
held while k silent companions (keys 30.., velocity 1: -95 dB) are held too; the mean change of its 6.3-12.7 kHz
third-octave bands against k = 0 marks the state (|change| > 0.12 dB; the state moves them 0.4-2.3 dB). The voices
on when it first moves are the threshold of that patch variant.
    python3 v6load.py thr BASE.raw [ENGINE]   the thresholds of the variants below on a base patch (BASE.raw: the
                                              readings were made on a test patch, a player's patch, not in the repo,
                                              and on the init patch: BASE "init") -> v6load_<base>.json
    python3 v6load.py fit JSON...             the units: a voice's cost by its features, the part's fixed costs, the
                                              threshold; the linear program with the largest relative margin that
                                              puts every threshold read where it was read (6 voices: the count alone)
    python3 v6load.py taps [ENGINE]           osc 2 alone (init, osc balance 127, semitone 0 and +7) in the state, keys
                                              36-72: the third-octave difference engine - reference, 2-20 kHz
The readings (PROVENANCE 9g): 37 variants on a test patch and the init patch, and dev131 (test material) to check."""
import os, sys, json
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import mlab

FS, BLOCK, PB = mlab.FS, mlab.BLOCK, mlab.PB
REFENG = os.environ['MLAB_REFENG']
KEY = int(os.environ.get('KEY', '48'))
# the variants (byte: value) and the features they have for the fit (per voice: o2 osc 2 heard, r1 routing 1, sat,
# mtx a voice's source in the matrix, an the analog modes, noise, ring, o3; per part: rv the reverb modes 2-4, eq, ph)
VAR = {'A': {}, 'B': {112: 0}, 'C': {53: 0}, 'D': {53: 0, 112: 0}, 'E': {33: 0}, 'F': {51: 5}, 'G': {33: 0, 53: 0, 112: 0},
       'H': {33: 0, 53: 0}, 'J': {33: 0, 112: 0}, 'K': {51: 5, 33: 0, 53: 0, 112: 0}, 'L': {51: 5, 53: 0, 112: 0},
       'P': {PB + 92: 64, PB + 96: 64, PB + 97: 0}, 'Q': {112: 0, 49: 1}, 'S': {51: 7, 33: 0, 53: 0, 112: 0},
       'nomtx': {PB + 64: 0, PB + 67: 0, PB + 72: 0}, 'nomtx_b': {PB + 64: 0, PB + 67: 0, PB + 72: 0, 112: 0},
       'src1': {PB + 72: 1}, 'src24': {PB + 72: 24}, 'L_noeq': {51: 5, 53: 0, 112: 0, PB + 92: 64, PB + 96: 64, PB + 97: 0},
       'noise': {112: 0, 37: 64}, 'ring': {112: 0, 38: 64}, 'o3': {112: 0, PB + 41: 2, PB + 42: 100},
       'ph': {112: 0, PB + 85: 64}, 'ph_noeq': {112: 0, PB + 85: 64, PB + 92: 64, PB + 96: 64, PB + 97: 0},
       'rt3': {112: 0, 53: 3}, 'm6': {112: 0, 51: 6}}
VAR_INIT = {'I0': {}, 'I_rv': {112: 2}, 'I_rvmid': {112: 2, PB + 92: 116}, 'I_an_rv': {51: 5, 112: 2}, 'I_m6': {51: 6},
            'I_rt3': {53: 3}, 'I_o2rt1_rv': {33: 64, 53: 1, 112: 2}}


def base(path):
    p = mlab.init_preset() if path == 'init' else bytearray(open(path, 'rb').read())
    p[mlab.CHORUS_MIX] = 0; p[mlab.FX_SEND] = 0; p[PB + 60] = 127; p[97] = 0
    return p


def thirds(x, segs):
    nf = 8192; f = np.fft.rfftfreq(nf, 1 / FS); w = np.hanning(nf); P = 0
    for a, b in segs:
        s = x[a:b]; k = max(1, (len(s) - nf) // (nf // 2) + 1)
        P = P + sum(np.abs(np.fft.rfft(s[i * nf // 2:i * nf // 2 + nf, c] * w)) ** 2 for i in range(k) for c in (0, 1)) / k
    C = 1000 * 2 ** (np.arange(-17, 14) / 3)
    return C, 10 * np.log10(np.array([P[(f >= c * 2 ** (-1 / 6)) & (f < c * 2 ** (1 / 6))].sum() for c in C]) + 1e-30)


def events(k, reps=6, on=1.6, off=4.0):
    ev, sl, t = [], [], 0
    for _ in range(reps):
        ev += [('on', 30 + j, 1) for j in range(k)] + [('run', 2), ('on', KEY, 127), ('run', mlab.units(on))]
        sl.append((t + 2 * BLOCK + int(0.25 * FS), t + (2 + mlab.units(on)) * BLOCK))
        ev += [('off', KEY)] + [('off', 30 + j) for j in range(k)] + [('run', mlab.units(off))]
        t += (2 + mlab.units(on) + mlab.units(off)) * BLOCK
    return ev, sl


def thr(path, eng, ks=range(0, 8)):
    V = VAR_INIT if path == 'init' else VAR
    jobs, keys = [], []
    for nm, mods in V.items():
        p = base(path)
        for b, v in mods.items(): p[b] = v
        for k in ks:
            ev, sl = events(k); jobs.append((p, ev)); keys.append((nm, k, sl))
    xs = mlab.render_many(eng, jobs, workers=4)
    T = {}
    for (nm, k, sl), x in zip(keys, xs): C, T[(nm, k)] = thirds(x, sl)
    hb = (C >= 6000) & (C < 13000); out = {}
    for nm in V:
        d = [float((T[(nm, k)] - T[(nm, 0)])[hb].mean()) for k in ks]
        t = next((k + 1 for k, x in zip(ks, d) if k and abs(x) > 0.12), None)
        out[nm] = {'mods': {str(b): v for b, v in V[nm].items()}, 'delta': d, 'thr': t}
        print('%-10s thr %s  ' % (nm, t) + ' '.join('%d:%+.2f' % (k, x) for k, x in zip(ks, d)), flush=True)
    return out


def feats(p):
    """the features of a patch as v6_on reads them (per voice, per part)"""
    an = p[51] in (5, 7)
    return {'o2': p[33] > 0 or p[38] > 0, 'r1': p[53] == 1, 'sat': p[49] > 0, 'noise': p[37] > 0, 'ring': p[38] > 0,
            'o3': p[PB + 41] > 0, 'mtx': any(p[PB + s] >= 19 for s in (64, 67, 72, 103, 106, 109)), 'an': an,
            'rv': 2 <= p[112] <= 4, 'eq': p[PB + 92] != 64 or p[PB + 95] != 64 or p[PB + 96] != 64 or p[PB + 97] > 0,
            'ph': p[PB + 85] > 0}


def fit(files):
    from scipy.optimize import linprog
    PV = ['b', 'o2', 'r1', 'sat', 'mtx', 'an']; FX = ['rv', 'eq']
    names = PV + FX
    A, bb = [], []
    for fn in files:
        J = json.load(open(fn)); bp = J.pop('_base')
        for nm, r in J.items():
            if r['thr'] is None or r['mods'].get('51') == 6 or r['mods'].get('53') == 3: continue
            if any(r['mods'].get(k) for k in ('37', '38', str(PB + 41), str(PB + 85))): continue   # (one reading each)
            p = base(bp)
            for b, v in r['mods'].items(): p[int(b)] = v
            f = feats(p); f['b'] = True
            def row(n): return np.array([n * f[k] if k in PV else float(f[k]) for k in names])
            t = r['thr']
            A.append(np.r_[row(t - 1), 1]); bb.append(1)                 # load(t - 1) <= 1 - m
            if t < 6: A.append(np.r_[-row(t), 1]); bb.append(-1)           # load(t) >= 1 + m
    A.append(np.r_[np.zeros(len(names)), 1]); bb.append(0.05)
    r = linprog(-np.r_[np.zeros(len(names)), 1], A_ub=np.array(A), b_ub=bb, bounds=[(0, None)] * len(names) + [(None, None)])
    u = r.x[0] / 3
    print('margin %.4f; in units of b / 3 (threshold %.2f): ' % (r.x[-1], 1 / u) + ', '.join('%s %.2f' % (k, x / u) for k, x in zip(names, r.x)))


def taps(eng):
    jobs, keys = [], []
    for key in (36, 48, 60, 72):
        global KEY
        KEY = key
        for semi in (64, 71):
            p = base('init'); p[33] = 127; p[25] = semi
            ev, sl = events(5, reps=6)
            for e in (REFENG, eng): jobs.append((p, ev)); keys.append((key, semi, e, sl))
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(4) as ex:
        xs = list(ex.map(lambda kj: mlab.render(kj[0][2], *kj[1]), zip(keys, jobs)))
    T = {}
    for (key, semi, e, sl), x in zip(keys, xs): C, T[(key, semi, e)] = thirds(x, sl)
    s = C >= 2000
    print('key semi  ' + ' '.join('%6.0f' % c for c in C[s]))
    for key in (36, 48, 60, 72):
        for semi in (64, 71):
            r, e = T[(key, semi, REFENG)], T[(key, semi, eng)]
            m = (r > r.max() - 60) & (C >= 2000) & (C < 4000)   # (aligned on 2-4 kHz: the shape above)
            d = e - r - np.mean((e - r)[m])
            print('%3d %4d  ' % (key, semi - 64) + ' '.join('%+6.2f' % x for x in d[s]))


if __name__ == '__main__':
    if sys.argv[1] == 'thr':
        o = thr(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else REFENG); o['_base'] = sys.argv[2]
        json.dump(o, open(os.path.join(HERE, 'v6load_%s.json' % os.path.basename(sys.argv[2]).split('.')[0]), 'w'), indent=1)
    elif sys.argv[1] == 'fit':
        fit(sys.argv[2:])
    elif sys.argv[1] == 'taps':
        taps(sys.argv[2] if len(sys.argv) > 2 else mlab.CLN)
