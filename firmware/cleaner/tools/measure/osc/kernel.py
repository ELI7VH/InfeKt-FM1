# SPDX-License-Identifier: GPL-3.0-only
"""kernel.py: the step response, the emphasis, the naive wave's delay and osc 1's restart phase, measured on
black-box renders of the target (osclib.py); writes osc_fit.json "kernel" (c1, c2, lat), "level" (ea, eb) and the
clock's t0.

1. A saw on osc 1 alone (osc 2 at balance 0), keys 78-96 (P 112 .. 40 ticks a cycle: clock.py; above, level.py), 1.4 s
   each after its start; every line n f1 (n = 0 .. 2.2 P: the harmonics, the images, folded at fs) read by least
   squares (osclib.lines at f1 from osclib.f1_precise). The model (osclib): S_n = g X_(n mod P) T(n / P) sinc^2(n / P)
   times a phase, X the naive saw's DFT on the P ticks (its step a tick from clock.py's bits), T the taps of the step
   response (C(1) = c1, C(2) = c2, symmetric over five ticks); each line through the emphasis eb - ea z^-1 at its
   folded frequency and the post chain L (osclib.post_chain: the input route of the same patch, white noise). Lines
   down to 1 / 1000 of the fundamental; the magnitudes alone (each key's start phase is unknown): per key the gain,
   together c1, c2, ea (eb = 1 + ea: the emphasis' scale is the gain's), least squares relative to each key's
   fundamental, from four starts.
2. What the magnitudes cannot show, by rendering the own oscillator (cbuild, the taps of 1) with candidates against
   the target, saws and squares at keys 36-96 with the target's start phases: the naive wave's delay lat (ticks) and
   osc 1's restart phase t0 (a power of two or 0): the smallest worst-case null.
3. ea, c1, c2 refined the same way (a search on each in turn, the span a quarter each round, four rounds)."""
import numpy as np
from scipy.optimize import least_squares
import osclib as O
import mlab

FS = O.FS


def clock_p(f1, C):
    q = f1 / (C['f0'] * FS); o = int(np.floor(np.log2(q))); x = q / 2 ** o
    return int(round(C['seg_n'][sum(x >= e for e in C['seg_e'])] / 2 ** o))


def model(par, keys, Lf, C, shape='saw', rise=1):
    c1, c2, lat, ea, eb, t0, d = par[:7]
    out = []
    for j, (f1, P, ns, A) in enumerate(keys):
        g, phi = par[7 + 2 * j], par[8 + 2 * j]
        step = ((2 ** C['ph_bits'] - 1) // P) << (32 - C['ph_bits'])
        a = step / 2 ** 32
        if shape == 'saw':
            X = O.saw_x(P, a, t0)
        else:                                       # the square (shape 127, the init width: duty 0.5 + 64 / 254)
            jj = np.arange(P); t = t0 + jj * a
            q1 = 64 * P / 254; tkm = np.floor(q1); tkf = q1 - tkm; dd = 0.5 + tkm / P
            e = t * P - P / 2 + tkm
            pz = np.where(e <= -0.5, 2 * dd, np.where(e >= 0.5, 2 * dd - 2, 2 * dd - 2 * tkf))
            if rise: pz[0] = 2 * dd - 2              # (the restart's tick: still the low side)
            X = np.fft.fft(pz) / P
        n = np.asarray(ns)
        S = g * X[n % P] * O.taps(c1, c2, n / P) * np.exp(-2j * np.pi * n * lat / P) * np.sinc(n / P) ** 2 \
            * np.exp(2j * np.pi * n * phi)
        fo, neg = O.fold(n * f1)
        S = np.where(neg, np.conj(S), S)
        E = eb - ea * np.exp(-2j * np.pi * fo / FS)
        out.append(S * E * O.interp_c(fo, *Lf) * np.exp(-2j * np.pi * fo * d / FS))
    return out


def measure(tgt, shape, keyset, C):
    keys = []
    for k in keyset:
        p = O.patch(sh1=64 if shape == 'saw' else 127)
        x = mlab.render(tgt, p, [('on', k, 100), ('run', mlab.units(1.8))])[:, 0][12000:12000 + 65536]
        f1 = 440 * 2 ** ((k - 69) / 12)
        f1 = O.f1_precise(x, O.f1_precise(x[:16384], f1))
        P = clock_p(f1, C)
        ns = np.arange(0, int(2.2 * P) + 1)
        f1, A, res = O.lines(x, f1, ns, fit_f1=False)
        keep = np.isfinite(A) & (np.abs(A) > 1e-3 * np.abs(A[1]))
        keys.append((f1, P, ns[keep], A[keep]))
        print('  key %d: f1 %.3f P %d, %d lines, residual %.1f dB' % (k, f1, P, keep.sum(), 20 * np.log10(res)))
    return keys


def fit(keys, Lf, C, shape='saw', rise=1, x0=None):
    def resid(par):
        M = model(par, keys, Lf, C, shape, rise)
        r = []
        for (f1, P, ns, A), m in zip(keys, M):
            s = np.abs(A[ns == 1][0])
            r.append((m - A) / s)
        r = np.concatenate(r)
        return np.r_[r.real, r.imag]
    if x0 is None:
        x0 = [0.06, 0.3, 3.0, 0.5, 1.5, 2 ** -12, 0.0]
        for f1, P, ns, A in keys:
            x0 += [np.abs(A[ns == 1][0]) / 0.6, 0.0]
    # the per-key phases first (a coarse grid each), then all together
    best = least_squares(resid, x0, x_scale='jac', max_nfev=4000)
    return best


def main():
    tgt, new = O.engines()
    F = O.load()
    C = F['clock']
    Lf = O.post_chain()
    keyset = list(range(78, 97, 2))         # (below pitch 97.8: the high pitches' level and emphasis apart, level.py)
    print('saw, keys', keyset)
    keys = measure(tgt, 'saw', keyset, C)
    # 1. the magnitudes alone (no phase in them): c1, c2, ea, eb, t0 and each key's gain, on the lines' log levels
    nk = len(keys)

    def rmag(q):                       # (the emphasis at 0 Hz taken as 1, eb = 1 + ea: its scale is the gain's)
        par = [q[0], q[1], 3.0, q[2], 1 + q[2], q[3], 0.0]
        for j in range(nk): par += [q[4 + j], 0.0]
        M = model(par, keys, Lf, C)
        return np.concatenate([(np.abs(m) - np.abs(A)) / np.abs(A[ns == 1][0]) for (f1, P, ns, A), m in zip(keys, M)])
    best = None
    for c1g, c2g in [(0.05, 0.3), (0.1, 0.25), (0.02, 0.4), (0.15, 0.35)]:
        q0 = [c1g, c2g, 0.4, 1e-3] + [np.abs(A[ns == 1][0]) / 0.5 for f1, P, ns, A in keys]
        lo = [-0.5, -0.5, -1.0, -0.05] + [0] * nk; hi = [0.5, 0.5, 2.0, 0.05] + [np.inf] * nk
        r1 = least_squares(rmag, q0, bounds=(lo, hi), x_scale='jac', max_nfev=3000)
        if best is None or r1.cost < best.cost: best = r1
    q = best.x
    print('1. magnitudes: c1 %.6f c2 %.6f ea %.5f t0 %.3e; residual rms %.2e of the fundamental'
          % (q[0], q[1], q[2], q[3], np.sqrt(np.mean(best.fun ** 2))))
    q = [q[0], q[1], q[2], 1 + q[2], q[3]] + list(q[4:])
    c1, c2, ea, eb, t0m = q[0], q[1], q[2], q[3], q[4]
    print('fit: c1 %.6f (1/%.3f) c2 %.6f (x15 %.4f) ea %.5f eb %.5f (ea / eb %.4f)' % (c1, 1 / c1, c2, 15 * c2, ea, eb, ea / eb))
    # 2. what the lines' magnitudes do not show (each key's start phase takes the delay; the restart's phase is in the
    # DC alone): the delay lat and the restart's phase t0 by rendering the own oscillator (cbuild, the taps above) with
    # each candidate against the target, the null of saws and squares at keys 36-96 (start phases as the target's)
    import cbuild
    cases = [(O.patch(sh1=sh), k) for sh in (64, 127) for k in (36, 60, 84, 96)]
    ref = [mlab.render(tgt, p, [('on', k, 100), ('run', mlab.units(0.6))])[:, 0][6000:] for p, k in cases]

    def null(flags):
        exe = cbuild.build('osck', flags, all_units=True)
        return max(mlab.diff_db(a, mlab.render(exe, p, [('on', k, 100), ('run', mlab.units(0.6))])[:, 0][6000:])
                   for (p, k), a in zip(cases, ref))
    base = ['-DCO_C1=%.7ff' % c1, '-DCO_C2=%.7ff' % c2, '-DCO_EA=%.7ff' % ea, '-DCO_EB=%.7ff' % eb]
    res = {}
    for lat in (2.0, 2.5, 3.0, 3.5, 4.0):
        res[lat] = null(base + ['-DCO_LAT=%.2ff' % lat, '-DCO_T0=%.9gf' % 2 ** -12])
    lat = min(res, key=res.get)
    print('  the delay: null (worst case, dB) per candidate', {k: round(v, 1) for k, v in res.items()}, '->', lat)
    res = {}
    for e in range(8, 17):
        res[2 ** -e] = null(base + ['-DCO_LAT=%.2ff' % lat, '-DCO_T0=%.9gf' % 2 ** -e])
    res[0.0] = null(base + ['-DCO_LAT=%.2ff' % lat, '-DCO_T0=0.f'])
    t0 = min(res, key=res.get)
    print('  the restart phase: null per candidate', {('2^-%d' % round(-np.log2(k)) if k else '0'): round(v, 1)
                                                      for k, v in res.items()}, '->', t0, '(the DC line alone: %.2e)' % t0m)
    # 3. the emphasis and the taps refined the same way (the magnitudes' fit leaves them within ~1 %): a search on
    # each in turn, twice, the smallest worst-case null
    def flags(c1_, c2_, ea_):
        return ['-DCO_C1=%.7ff' % c1_, '-DCO_C2=%.7ff' % c2_, '-DCO_EA=%.7ff' % ea_, '-DCO_EB=%.7ff' % (1 + ea_),
                '-DCO_LAT=%.2ff' % lat, '-DCO_T0=%.9gf' % t0]
    for it in range(4):
        for name, span in (('ea', 0.02), ('c1', 0.002), ('c2', 0.004)):
            cur = {'ea': ea, 'c1': c1, 'c2': c2}
            best = None
            for v in np.linspace(cur[name] - span / 4 ** it, cur[name] + span / 4 ** it, 9):
                cur[name] = v
                n_ = null(flags(cur['c1'], cur['c2'], cur['ea']))
                if best is None or n_ < best[0]: best = (n_, v)
            if name == 'ea': ea = best[1]
            elif name == 'c1': c1 = best[1]
            else: c2 = best[1]
            print('  %s -> %.6f (null %.1f dB)' % (name, best[1], best[0]))
    eb = 1 + ea
    d = 0.0
    kern = {'c1': float(c1), 'c2': float(c2), 'lat': float(lat), 'd': float(d)}
    O.save('kernel', kern)
    lv = O.load().get('level', {})
    lv.update({'ea': float(ea), 'eb': float(eb)})
    O.save('level', lv)
    C['t0'] = float(t0)
    O.save('clock', C)


if __name__ == '__main__':
    main()
