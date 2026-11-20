# SPDX-License-Identifier: GPL-3.0-only
"""fit_rvlf.py: the own reverb's low-frequency bank (sy_cln_rv.c RV_LF, engine/sy_cln_rvlf.h) fitted to the reference's
wet LF transfer, measured as a black box (PROVENANCE.md section 17). Only rendered audio is used.
    fit_rvlf.py measure            noise (64 s) into the FX input of the reference (RV_REF) and of the chain
                                   build (RV_CHAIN: rvbuild.sh with CLN_XF=-DRV_LF_TEST, the bank a plain path, the
                                   network silent), rooms 0-3, decays 12 / 32 / 64 / 102 / 120: H = Sxy / Sxx, P = Syy / Sxx
    fit_rvlf.py fit ROOM OUT.json  the modes: peaks of the reference's |H|^2 at decay 102 (12-275 Hz), frequencies refined
                                   by variable projection against decays 64 and 102, the weights by linear least squares
                                   (the target: the reference's H x sqrt(1 - |high pass|^2) / the chain, its incoherent
                                   share added back by the smoothed P / |H|^2), the gain per decay node by power
    fit_rvlf.py cal IN.json OUT.json  the gain nodes corrected by the realized level (RV_NEW16: the engine built with
                                   the bank, 25-200 Hz)
    fit_rvlf.py gen IN.json        writes engine/sy_cln_rvlf.h
Engines: the lab's command line (mlab.py); SY_RVQ=0 SY_NOLEND=1 for the own engine (HALF, the 16-bit ring)."""
import os, sys, json, pickle, hashlib
import numpy as np
from scipy.signal import butter, find_peaks, sosfreqz
from scipy.optimize import least_squares
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import mlab  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SCR = os.environ.get('RV_SCR', '/tmp/infekt/rvbal')
ENG = {'ref': (os.environ['RV_REF'], {}),
       'chain': (os.environ.get('RV_CHAIN', os.path.join(SCR, 'lft', 'new_multi')), {'SY_NOLEND': '1', 'SY_RVQ': '0'}),
       'new': (os.environ.get('RV_NEW16', os.path.join(SCR, 'n15', 'new_multi')), {'SY_NOLEND': '1', 'SY_RVQ': '0'})}
FS = 46875.; FS2 = FS / 16; FSL = FS / 64; FC_AA = 260.; FC_X = 220.
NF = 1 << 17; SECS = 64; DN = (12, 32, 64, 102, 120)
F_ALL = np.fft.rfftfreq(NF, 1 / FS); LF = (F_ALL >= 10) & (F_ALL <= 460); F = F_ALL[LF]


def patch(room, decay, colour=64):
    p = mlab.init_preset()
    p[mlab.INPUT_MODE] = 3; p[mlab.INPUT_SELECT] = 0; p[mlab.CHORUS_MIX] = 0
    p[112] = 2; p[113] = 127; p[114] = 0; p[115] = 0; p[116] = decay; p[117] = room; p[119] = colour
    return p


def meas(e, room, decay):
    """the transfer H (complex, left / right) and the power response P, 10-460 Hz in 0.36 Hz bins (cached)"""
    exe, env = ENG[e]
    key = hashlib.md5(repr(('rvlf', e, exe if e == 'ref' else os.path.getmtime(exe), room, decay)).encode()).hexdigest()[:16]
    cf = os.path.join(SCR, 'cache', 'rvlf_' + key + '.pkl')
    if os.path.exists(cf): return pickle.load(open(cf, 'rb'))
    xn = np.random.default_rng(23).standard_normal(int(SECS * FS)) * 0.05
    y = mlab.render(exe, patch(room, decay), [('run', mlab.units(SECS))], inp=np.stack([xn, 0 * xn], 1), env=env)
    n = min(len(y), len(xn)); hop = NF // 2; w = np.hanning(NF); Sxx = 0; Sxy = [0, 0]; Syy = [0, 0]
    for i in range(int(6 * FS), n - NF, hop):
        X = np.fft.rfft(xn[i:i + NF] * w)[LF]; Sxx = Sxx + np.abs(X) ** 2
        for c in (0, 1):
            Y = np.fft.rfft(y[i:i + NF, c] * w)[LF]; Sxy[c] = Sxy[c] + Y * np.conj(X); Syy[c] = Syy[c] + np.abs(Y) ** 2
    r = (np.array([Sxy[c] / Sxx for c in (0, 1)]), np.array([Syy[c] / Sxx for c in (0, 1)]))
    os.makedirs(os.path.dirname(cf), exist_ok=True); pickle.dump(r, open(cf, 'wb')); return r


def sos(o, fc, fs, kind):
    s = butter(o, fc, kind, fs=fs, output='sos'); return [list(r[:3] / r[3]) + list(r[4:6] / r[3]) for r in s]


def cic():
    b = np.ones(16); h = np.convolve(np.convolve(b, b), b); return h / h.sum()


def t60(room, d):
    """the own reverb's mid T60 at a decay (sy_cln_rv.c rv_par_make, its CP_T60 table)"""
    import fit_rv
    v = fit_rv.source_values(); T = [v['T%d_%d' % (room, i)] for i in range(18)]
    x = d / 8 if d < 120 else 15 + (d - 120) / 4 if d < 124 else 16 + (d - 124) / 2
    j = min(int(x), 16); return 2 ** (np.log2(T[j]) + (x - j) * (np.log2(T[j + 1]) - np.log2(T[j])))


def hp2(f):
    s = np.array(sos(4, FC_X, FS, 'high')); s = np.hstack([s[:, :3], np.ones((2, 1)), s[:, 3:]])
    return np.abs(sosfreqz(s, worN=f, fs=FS)[1]) ** 2


def smooth(x, n=21): return np.convolve(x, np.ones(n) / n, 'same')


def target(room, d):
    H, P = meas('ref', room, d); C, _ = meas('chain', room, d)
    comp = np.sqrt(np.array([smooth(P[c]) / smooth(np.abs(H[c]) ** 2) for c in (0, 1)]))
    T = np.sqrt(np.maximum(1 - hp2(F), 0))[None, :] * H * comp / C[0][None, :]
    return T, 2 ** (-9.965784 / (FSL * t60(room, d)))


def basis(f, fk, rad):
    z = np.exp(-2j * np.pi * f / FSL)[:, None]; p = rad * np.exp(2j * np.pi * fk[None, :])
    return 1 / (1 - p * z), 1 / (1 - np.conj(p) * z)


def solve(f, Ts, fk):
    rows = []; ys = [[], []]
    for T, rad in Ts:
        A, B = basis(f, fk, rad); M = np.hstack([A + B, 1j * (A - B)])
        rows.append(np.vstack([M.real, M.imag]))
        for c in (0, 1): ys[c].append(np.concatenate([T[c].real, T[c].imag]))
    Mr = np.vstack(rows); R = []; res = []
    for c in (0, 1):
        y = np.concatenate(ys[c]); x, *_ = np.linalg.lstsq(Mr, y, rcond=None)
        R.append(x[:len(fk)] + 1j * x[len(fk):]); res.append(Mr @ x - y)
    return np.array(R), np.concatenate(res)


def fit(room, N=112, lo=12., hi=330., fmax=275., iters=12):
    band = (F >= lo) & (F <= hi); f = F[band]
    Ts = [(T[:, band], rad) for T, rad in (target(room, d) for d in (64, 102))]
    H102, _ = meas('ref', room, 102); Pq = (np.abs(H102) ** 2).sum(0)
    mm = (F > lo - 2) & (F < hi + 8); pk, _ = find_peaks(10 * np.log10(Pq[mm]), prominence=0.3)
    fk = np.sort(F[mm][pk][np.argsort(-Pq[mm][pk])[:N]]) / FSL
    fk = least_squares(lambda x: solve(f, Ts, x)[1], fk, max_nfev=iters, x_scale=0.05 / FSL, diff_step=1e-5).x
    fk = fk[fk * FSL <= fmax]                       # (the modes above fmax dropped: the crossover's region)
    band = (F >= lo) & (F <= fmax + 15.); f = F[band]
    R, _ = solve(f, [(T[:, band], rad) for T, rad in (target(room, d) for d in (64, 102))], fk)
    g = []
    for d in DN[:-1]:
        T, rad = target(room, d); T = T[:, band]
        A, B = basis(f, fk, rad); Mo = np.array([A @ r + B @ np.conj(r) for r in R])
        g.append(float(np.sqrt(np.sum(np.abs(T) ** 2) / np.sum(np.abs(Mo) ** 2))))
    g.append(g[-1])                                 # (120: its modes narrower than the bins)
    W = np.stack([2 * R[0].real, -2 * R[0].imag, 2 * R[1].real, -2 * R[1].imag], 1)
    return dict(f=list(map(float, fk)), w=W.tolist(), g=g)


def cal(m):
    b = (F > 25) & (F < 200)
    for r in range(4):
        m[str(r)]['g'] = [g * float(np.sqrt(meas('ref', r, d)[1][:, b].sum() / meas('new', r, d)[1][:, b].sum()))
                          for g, d in zip(m[str(r)]['g'], DN)]
    return m


def cf(x):
    t = '%.9g' % x
    return (t if ('.' in t or 'e' in t) else t + '.') + 'f'


def c_init(a):
    a = np.asarray(a, dtype=float)
    return '{%s}' % ', '.join(cf(x) for x in a) if a.ndim == 1 else '{%s}' % ', '.join(c_init(x) for x in a)


def gen(modes, out):
    L = ['/* SPDX-License-Identifier: GPL-3.0-only */',
         '/* GENERATED by cleaner/tools/measure/reverb/fit_rvlf.py gen: the reverb\'s low-frequency path (sy_cln_rv.c RV_LF).',
         ' * Filters: DESIGN. Modes: FITTED to black-box renders of the reference (its wet LF transfer per room, PROVENANCE.md',
         ' * section 17); per mode its frequency (cycles a sample at 46,875 / 64 Hz, x 2^16) and its weights on the',
         ' * resonator\'s real and imaginary part, left and right (x the room\'s scale / 32767); the bank\'s gain at the decay',
         ' * nodes RVLF_DN (straight between) */', '#ifndef SY_CLN_RVLF_H', '#define SY_CLN_RVLF_H',
         'static const float RVLF_CIC[46] = %s;' % c_init(cic()),
         'static const float RVLF_AA[3][5] = %s;' % c_init(sos(6, FC_AA, FS2, 'low')),
         'static const float RVLF_HPF[2][5] = %s;' % c_init(sos(4, FC_X, FS, 'high')),
         'static const float RVLF_HPH[2][5] = %s;' % c_init(sos(4, FC_X, FS / 2, 'high'))]
    for r in range(4): modes.setdefault(str(r), dict(f=[], w=[], g=[1.] * 5))
    NMAX = max([len(modes[str(r)]['f']) for r in range(4)] + [1])
    L += ['#define RVLF_NMAX %d' % NMAX,
          'static const uint8_t RVLF_N[4] = {%s};' % ', '.join(str(len(modes[str(r)]['f'])) for r in range(4))]
    sc, rows = [], []
    for r in range(4):
        W = np.array(modes[str(r)]['w']); s = float(np.abs(W).max()) if len(W) else 0.; sc.append(s)
        rr = [[int(round(f * 65536)) & 0xffff] + [int(round(x / s * 32767)) for x in w] for f, w in zip(modes[str(r)]['f'], W)]
        rows.append(rr + [[0] * 5] * (NMAX - len(rr)))
    L += ['static const float RVLF_SC[4] = %s;' % c_init(sc),
          'static const uint8_t RVLF_DN[5] = {%s};   /* (the gains\' decay nodes) */' % ', '.join(map(str, DN)),
          'static const float RVLF_G[4][5] = %s;' % c_init([modes[str(r)]['g'] for r in range(4)]),
          'static const uint16_t RVLF_F[4][RVLF_NMAX] = {%s};' % ', '.join('{%s}' % ', '.join(str(x[0]) for x in rr) for rr in rows),
          'static const int16_t RVLF_W[4][RVLF_NMAX][4] = {%s};' % ', '.join(
              '{%s}' % ', '.join('{%s}' % ', '.join(str(v) for v in x[1:]) for x in rr) for rr in rows), '#endif', '']
    open(out, 'w').write('\n'.join(L))


if __name__ == '__main__':
    a = sys.argv[1:]
    if a[0] == 'measure':
        for e in ('ref', 'chain'):
            for r in range(4):
                for d in DN: meas(e, r, d)
    elif a[0] == 'fit':
        r = int(a[1]); m = json.load(open(a[2])) if os.path.exists(a[2]) else {}
        m[str(r)] = fit(r); json.dump(m, open(a[2], 'w'))
    elif a[0] == 'cal':
        json.dump(cal(json.load(open(a[1]))), open(a[2], 'w'))
    elif a[0] == 'gen':
        gen(json.load(open(a[1])), os.path.join(HERE, '..', '..', '..', 'engine', 'sy_cln_rvlf.h'))
