# SPDX-License-Identifier: GPL-3.0-only
"""level.py: the high pitches' level and emphasis, measured on black-box renders of the target (osclib.py); writes
osc_fit.json "level" (p0, k; ea, eb kept from kernel.py).

A saw on osc 1 alone (osc 2 silent at the same pitch), keys 78-127 and the bend between them; per key the lines read
and fitted as kernel.py does (the step response, the delay, t0, the post chain's delay held at kernel.py's), but with
the key's own gain and emphasis (only their product's scale G = g eb and the ratio rho = ea / eb show). The law
tested: g = m g0, ea = ea0 + (1 - m), eb = max(1, eb0 - (1 - m)) (the emphasis' lows fall to 2m - 1, its top stays);
m read from G / G0 and, independently, from rho; m against the pitch (69 + 12 log2(f1 / 440)) a straight line from 1
at p0 down by k a semitone (a least-squares fit where 0.3 < m < 0.95 and the two m agree within 0.05). Then p0, k
refined by rendering the own oscillator with candidates against the target (sines and saws, keys 98-127: the largest
level difference; p0 +-2, k +-0.004, the span a third each round, five rounds). Last each key's m solved from its
sine's level against the own engine's (m known there, the emphasis by the law) and the line fitted again."""
import numpy as np
from scipy.optimize import least_squares
import osclib as O
import kernel as Kf
import mlab

FS = O.FS


def main():
    tgt, new = O.engines()
    F = O.load()
    C, Kn = F['clock'], F['kernel']
    Lf = O.post_chain()
    res = []
    keyset = list(range(78, 128))
    keys = Kf.measure(tgt, 'saw', keyset, C)
    ea0 = F['level']['ea']; eb0 = F['level']['eb']
    for k, key in zip(keyset, keys):
        f1, P, ns, A = key

        def resid(par):                 # (the gain and the emphasis' scale are one: G = g eb, rho = ea / eb)
            q = [Kn['c1'], Kn['c2'], Kn['lat'], par[2], 1.0, C['t0'], Kn['d'], par[0], par[1]]
            m = Kf.model(q, [key], Lf, C)[0]
            r = (m - A) / np.abs(A[ns == 1][0])
            return np.r_[r.real, r.imag]
        best = None
        for phi in np.linspace(0, 1, 37):
            s_ = least_squares(resid, [np.abs(A[ns == 1][0]), phi, ea0 / eb0], max_nfev=400)
            if best is None or s_.cost < best.cost: best = s_
        G, phi, rho = best.x
        p = 69 + 12 * np.log2(f1 / 440)
        res.append((k, p, G, rho, np.sqrt(np.mean(best.fun ** 2))))
    G0 = np.median([r[2] for r in res if r[1] < 94]); rho0 = np.median([r[3] for r in res if r[1] < 94])
    # the law to test: g = m g0, ea = ea0 + dm, eb = max(1, eb0 - dm), dm = 1 - m: G / G0 = m eb / eb0, rho = ea / eb
    def law(m):
        dm = 1 - m; eb = np.maximum(1, eb0 - dm); return m * eb / eb0, (ea0 + dm) / eb
    mm = np.linspace(0.02, 1, 4901); LG, LR = law(mm)
    print(' key  pitch    G/G0    rho   m(gain) m(emph)  residual')
    ms, me = [], []
    for k, p, G, rho, rr in res:
        a1 = mm[np.argmin(np.abs(LG - G / G0))]; a2 = mm[np.argmin(np.abs(LR - rho))]
        ms.append(a1); me.append(a2)
        print('%4d %7.3f %7.4f %7.4f %7.4f %7.4f  %.1e' % (k, p, G / G0, rho, a1, a2, rr))
    ps = np.array([r[1] for r in res]); ms = np.array(ms); me = np.array(me)
    sel = (ms < 0.95) & (ms > 0.3) & (np.abs(me - ms) < 0.05)   # (falling, where the gain's and the emphasis' m agree)
    a, b = np.polyfit(ps[sel], ms[sel], 1)            # m = a p + b = 1 - k (p - p0)
    k = -a; p0 = (b - 1) / k
    print('m (from the gain) = 1 - %.6f (pitch - %.3f): residual %.4f max; the emphasis\' m against the gain\'s: %.4f max'
          ' (rho at low keys %.4f, ea0 / eb0 %.4f)' % (k, p0, np.max(np.abs(ms[sel] - (1 - k * (ps[sel] - p0)))),
                                                    np.max(np.abs(me[sel] - ms[sel])), rho0, ea0 / eb0))
    # 2. refined by rendering the own oscillator with candidates (cbuild) against the target: sines and saws at keys
    # 98-127, the largest level difference (dB) the smallest; p0 and k in turn
    import cbuild
    cases = [(O.patch(sh1=sh), kk) for sh in (0, 64) for kk in range(98, 128)]
    ev = lambda kk: [('on', kk, 100), ('run', mlab.units(0.4))]
    ref = [mlab.rms(mlab.render(tgt, p, ev(kk))[:, 0][6000:]) for p, kk in cases]

    def worst(p0_, k_):
        exe = cbuild.build('oscl', ['-DCO_HI_P0=%.5ff' % p0_, '-DCO_HI_K=%.7ff' % k_], all_units=True)
        return max(abs(20 * np.log10(mlab.rms(mlab.render(exe, p, ev(kk))[:, 0][6000:]) / a)) for (p, kk), a in zip(cases, ref))
    for it in range(5):
        best = None
        for v in np.linspace(p0 - 2.0 / 3 ** it, p0 + 2.0 / 3 ** it, 9):
            w_ = worst(v, k)
            if best is None or w_ < best[0]: best = (w_, v)
        p0 = best[1]
        best = None
        for v in np.linspace(k - 0.004 / 3 ** it, k + 0.004 / 3 ** it, 9):
            w_ = worst(p0, v)
            if best is None or w_ < best[0]: best = (w_, v)
        k = best[1]
        print('  round %d: p0 %.4f k %.6f, the largest level difference %.4f dB' % (it, p0, k, best[0]))
    # 3. each key's m from its sine against the own engine's (m known there): the fundamental's level is m |E_m(f1)|
    # (the emphasis by the law), solved for the target's m; the line fitted again on those (0.3 < m < 0.97)
    exe = cbuild.build('oscl', ['-DCO_HI_P0=%.5ff' % p0, '-DCO_HI_K=%.7ff' % k], all_units=True)
    PS, MT = [], []
    for kk in range(98, 128):
        p = O.patch(sh1=0)
        xa = mlab.render(tgt, p, ev(kk))[:, 0][6000:]; xb = mlab.render(exe, p, ev(kk))[:, 0][6000:]
        f1 = O.f1_precise(xa, 440 * 2 ** ((kk - 69) / 12))
        A = np.abs(O.lines(xa, f1, [1], fit_f1=False)[1][0]); B = np.abs(O.lines(xb, f1, [1], fit_f1=False)[1][0])
        pp = 69 + 12 * np.log2(f1 / 440)
        mo = min(1, max(0, 1 - k * (pp - p0)))

        def lev(m):
            dm = 1 - m; w = 2 * np.pi * f1 / FS
            return m * abs(max(1, eb0 - dm) - (ea0 + dm) * np.exp(-1j * w))
        mm_ = np.linspace(0.01, 1, 9901)
        L_ = np.array([lev(m) for m in mm_])
        mt = mm_[np.argmin(np.abs(L_ - lev(mo) * A / B))]
        PS.append(pp); MT.append(mt)
    PS, MT = np.array(PS), np.array(MT)
    sel = (MT > 0.3) & (MT < 0.97)
    a, b = np.polyfit(PS[sel], MT[sel], 1)
    k = -a; p0 = (b - 1) / k
    print('3. m per key from the sines: m = 1 - %.6f (pitch - %.4f), residual %.5f max; the largest level difference '
          'now %.4f dB' % (k, p0, np.max(np.abs(MT[sel] - (1 - k * (PS[sel] - p0)))), worst(p0, k)))
    lv = F.get('level', {})
    lv.update({'p0': float(p0), 'k': float(k)})
    O.save('level', lv)


if __name__ == '__main__':
    main()
