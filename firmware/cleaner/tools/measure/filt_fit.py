# SPDX-License-Identifier: GPL-3.0-only
"""filt_fit.py HYB LAW PROBE OUT.json: filter 1 of the development engine measured as a black box against the cleaner's own law.
Both engines render the input through the filters without a note (input mode 2, filter 1 alone: balance 0, low pass,
saturation off), a periodic noise in (two periods, the second analysed: the steady state, Y = H X per bin); LAW is the cleaner built with -DCLN_LAW_ONLY (its own filter law, no correction),
PROBE that law's numbers (probe_law: the frequency and damping it sets per cutoff and resonance). The two responses
differ only in filter 1's frequency and damping (the rest of the path is the same code), so their ratio is
    H_hyb / H_law = SVF(g, k) / SVF(g0, k0)      SVF: the TPT low pass, the bilinear 1 / (s^2 + k s + 1) at g = tan(pi f / fs)
and (g, k) of the development engine is fitted on it (complex, every bin above the floor). Per cutoff value 0..127 and resonance
0 / 32 / 64 / 96 / 127: f, k of the development engine, f0, k0 of the law, the fit's residual. fit_filt.py makes the correction."""
import sys, json
import numpy as np
from scipy.optimize import least_squares
import mlab


RESOS = (0, 32, 64, 96, 112, 120, 124, 127)


def patch(cut, reso):
    p = mlab.init_preset()
    p[mlab.INPUT_MODE] = 2; p[mlab.INPUT_SELECT] = 2; p[mlab.OSC_VOL] = 64; p[mlab.CHORUS_MIX] = 0; p[mlab.FX_SEND] = 0
    p[mlab.CUTOFF] = cut; p[mlab.CUTOFF2] = 64; p[mlab.F1_RESO] = reso; p[mlab.F2_RESO] = 0; p[mlab.F_BAL] = 0
    p[mlab.F1_MODE] = 0; p[mlab.SAT_TYPE] = 0
    return p


def svf(w, g, k):
    s = 1j * np.tan(w / 2) / g
    return 1 / (s * s + k * s + 1)


def fit(hh, hl, g0, k0):
    N = len(hh)
    Hh, Hl = np.fft.rfft(hh), np.fft.rfft(hl)   # (the same input: their ratio is the filters' ratio)
    f = np.fft.rfftfreq(N, 1 / mlab.FS); w = 2 * np.pi * f / mlab.FS
    m = (f > 4) & (f < 21500) & (np.abs(Hl) > np.abs(Hl).max() * 1e-4) & (np.abs(Hh) > np.abs(Hh).max() * 1e-4)
    Q, ww = Hh[m] / Hl[m], w[m]
    S0 = svf(ww, g0, k0)

    def res(q):
        d = svf(ww, np.exp(q[0]), q[1]) / S0 - Q
        wt = np.maximum(np.abs(Q), 1e-3) ** 0.5
        return np.concatenate([d.real / wt, d.imag / wt])
    r = least_squares(res, [np.log(g0), k0], bounds=([-14, 0], [8, 4]), x_scale=[0.01, max(k0, 1e-3) * 0.01])
    g, k = np.exp(r.x[0]), r.x[1]
    e = np.abs(svf(ww, g, k) / S0 - Q) / np.abs(Q)
    return float(np.arctan(g) * mlab.FS / np.pi), float(k), float(np.sqrt(np.mean(e ** 2)))


if __name__ == '__main__':
    hyb, law, probe, out = sys.argv[1:5]
    L = {}
    for ln in open(probe):
        c, r, f, k = ln.split(); L[int(c), int(r)] = (float(f), float(k))
    keys = [(c, r) for r in RESOS for c in range(128)]
    R = []
    for part in range(0, len(keys), 64):
        ks = keys[part:part + 64]
        jobs = []
        for c, r in ks:
            N = 1 << (18 if c < 40 else 16)
            amp = 0.2 * min(1.0, 2.0 * L[c, r][1])   # (the resonance's gain: the noise lower, nothing clips)
            nz = np.clip(np.random.default_rng(N).standard_normal(N) * amp, -0.95, 0.95)
            P = 4 if (c < 40 and r >= 112) else 2   # (the long ringing: more periods before the one analysed)
            x = np.tile(nz, P)
            jobs.append((patch(c, r), [('run', P * N // 128 + 4)], np.stack([x, x], 1)))
        ah = mlab.render_many(hyb, jobs); al = mlab.render_many(law, jobs)
        for (c, r), x, y in zip(ks, ah, al):
            N = 1 << (18 if c < 40 else 16)
            f0, k0 = L[c, r]
            g0 = np.tan(np.pi * f0 / mlab.FS)
            P = 4 if (c < 40 and r >= 112) else 2
            f, k, e = fit(x[(P - 1) * N:P * N, 0], y[(P - 1) * N:P * N, 0], g0, k0)
            R.append({'cut': c, 'reso': r, 'f': f, 'k': k, 'f0': f0, 'k0': k0, 'resid': e})
    json.dump(R, open(out, 'w'))
    for q in R[::9]: print('%3d %3d  f/f0 %.5f  k/k0 %.5f  resid %.1e' % (q['cut'], q['reso'], q['f'] / q['f0'], q['k'] / max(q['k0'], 1e-9), q['resid']))
