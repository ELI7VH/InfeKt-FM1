# SPDX-License-Identifier: GPL-3.0-only
"""v6.py: the oscillators' 6-voice state (synth.c CO_V6, PROVENANCE.md 9d), measured on the reference as a black
box (its audio alone, MLAB_REFENG).

The observation: with 6 or more voices on, every voice's oscillators change their response (the VC6 gain apart). One
audible voice (velocity 127) is rendered alone and with 5 silent companions (velocity 1, velocity -> amp 127: -95 dB),
its harmonic lines tracked (lines()); the ratio of the two is the state's response on that voice.
    python3 v6.py meas      the ratios: osc 1 saw at keys 36-84 and 38-46 (the clock's segments), osc 2 alone
                            (osc balance 127, detune 32) -> v6_lines.json
    python3 v6.py fit       (1) osc 2's taps: the ratio left after a build with osc 1's model for both oscillators
                            (half the ticks, a straight line over a tick, no emphasis: -DCO_V6_C1=0 -DCO_V6_C2=0),
                            fitted as osc 2's step response C1, C2 at the halved clock;
                            (2) the plain oscillators' (sync, FM) 3-tap FIRs: the ratios' mean over the keys, the VC6
                            gain (+0.55 dB) taken off -> osc_fit.json "v6" (then make_hdr.py)
The model itself (half the ticks, one tick's straight line, no emphasis) was found by trying candidate builds against
these ratios: osc 1's lines then agree within 0.05 dB below 19 kHz at keys 36-120, its images too."""
import os, sys, json
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import mlab, cbuild
from scipy.optimize import least_squares

FS = mlab.FS
REFENG = os.environ['MLAB_REFENG']
OUT = os.path.join(HERE, 'v6_lines.json')
NL = 32768
COMP = [30, 31, 32, 33, 34]
CASES = {'osc1': ({}, [36, 48, 60, 72, 84, 38, 40, 42, 44, 46]), 'osc2': ({mlab.OSC_BAL: 127}, [36, 48, 60, 38, 42, 44, 46, 72])}


def patch(mods):
    p = mlab.init_preset(); p[mlab.CUTOFF2] = 127; p[mlab.PB + 60] = 127
    for k, v in mods.items(): p[k] = v
    return p


def spec(x):
    m = x.mean(axis=1); hop = NL // 2; k = max(1, (len(m) - NL) // hop + 1); w = np.hanning(NL)
    return sum(np.abs(np.fft.rfft(m[i * hop:i * hop + NL] * w)) ** 2 for i in range(k)) / k


def lines(P, fguess, top=23400.):
    """the harmonic lines {h: (Hz, dB)}: f0 refined as the lines go up, each peak within 2 bins of h f0"""
    df = FS / NL
    def pk(f, w):
        b = int(round(f / df)); lo = max(1, b - w); i = lo + int(np.argmax(P[lo:b + w + 1]))
        a, c, d = (np.log(P[j] + 1e-40) for j in (i - 1, i, i + 1))
        den = a - 2 * c + d
        return (i + (0.5 * (a - d) / den if den else 0.)) * df, 10 * np.log10(P[i - 3:i + 4].sum() + 1e-40)
    f0 = pk(fguess, 6)[0]; res = {}; h = 1
    while h * f0 < top:
        f, v = pk(h * f0, 2 if h > 1 else 6); res[h] = (f, v)
        if h >= 2 and v > 10 * np.log10(P.max()) - 80: f0 += (f / h - f0) * 0.3
        h += 1
    return res


def ratios(eng, mods, keys):
    """per key: the lines' Hz, alone (dB), with 5 silent companions (dB)"""
    out = {}
    jobs = []
    for T in keys:
        for k in (0, 5):
            jobs.append((patch(mods), [('on', n, 1) for n in COMP[:k]] + [('on', T, 127), ('run', mlab.units(2.0))]))
    xs = mlab.render_many(eng, jobs)
    for i, T in enumerate(keys):
        a, b = int(0.3 * FS), int(2.0 * FS)
        L = [lines(spec(x[a:b]), 440 * 2 ** ((T - 69) / 12) * 1.0017) for x in xs[2 * i:2 * i + 2]]
        hs = sorted(set(L[0]) & set(L[1]))
        f = [L[0][h][0] for h in hs]; v1 = [L[0][h][1] for h in hs]; v6 = [L[1][h][1] for h in hs]
        out[str(T)] = [f, v1, v6]
    return out


def ftick6(f1):
    """the halved clock (ticks a second): P / 2 ticks a cycle of osc 1 (sy_cln_osc.h's law)"""
    o = int(np.floor(np.log2(f1 / (FS / 512)))); r = f1 / (FS / 512) / 2 ** o
    N = 896 if r < 2 ** (1 / 6) else 768 if r < 2 ** (2 / 6) else 640 if r < 2 ** (4 / 6) else 512
    return N * f1 / 2 ** o / 2


def sel(v, top=20500.):
    f, v1, v6 = (np.array(a) for a in v)
    ok = (v1 > v1.max() - 70) & (f < top)
    return f[ok], v1[ok], v6[ok]


def fir3(F, D):
    w = 2 * np.pi * F / FS
    H = lambda c: 20 * np.log10(np.abs(1 - c[0] - c[1] + c[0] * np.exp(-1j * w) + c[1] * np.exp(-2j * w)))
    r = least_squares(lambda c: H(c) - D, [-0.1, 0.05])
    return [1 - r.x.sum(), r.x[0], r.x[1]], float(np.sqrt(np.mean(r.fun ** 2)))


def main():
    if sys.argv[1:2] == ['meas']:
        R = {nm: ratios(REFENG, m, k) for nm, (m, k) in CASES.items()}
        json.dump(R, open(OUT, 'w'))
        for nm in R:
            for T, v in R[nm].items():
                f, v1, v6 = sel(v)
                print(nm, T, ' '.join('%.0f:%+.2f' % (lo, np.mean((v6 - v1)[(f >= lo) & (f < lo + 2000)])) for lo in range(0, 20000, 2000)))
        return
    R = json.load(open(OUT))
    g6 = 20 * np.log10(1.0654)                  # (the VC6 gain, synth.c)
    # (1) osc 2: a build with osc 1's model for both
    exe = cbuild.build('v6c0', ['-DCO_V6_C1=0.f', '-DCO_V6_C2=0.f'], all_units=True)
    m2 = ratios(exe, CASES['osc2'][0], CASES['osc2'][1])
    TH, E = [], []
    for T, v in R['osc2'].items():
        f, v1, v6 = sel(v, 21000)
        fe, _, e6 = sel(m2[T], 21000)
        n = min(len(f), len(fe))
        TH += list(2 * np.pi * f[:n] / ftick6(f[0])); E += list(e6[:n] - v6[:n])
    TH, E = np.array(TH), np.array(E)
    T5 = lambda c: np.abs(1 - 2 * c[1] + 2 * (c[1] - c[0]) * np.cos(TH) + 2 * c[0] * np.cos(2 * TH))
    r = least_squares(lambda c: 20 * np.log10(T5(c)) + E, [0.05, 0.3])
    print('osc 2: C1 %.5f C2 %.5f (rms %.2f dB over %d lines)' % (r.x[0], r.x[1], np.sqrt(np.mean(r.fun ** 2)), len(E)))
    # (2) the plain oscillators
    fit = json.load(open(os.path.join(HERE, 'osc_fit.json')))
    v6 = fit.setdefault('v6', {})
    v6['c1_osc2_lines'], v6['c2_osc2_lines'] = round(float(r.x[0]), 5), round(float(r.x[1]), 5)   # (the taps used: v6load.py taps)
    for nm, key in (('osc1', 'plain1'), ('osc2', 'plain2')):
        F, D = [], []
        for T, v in R[nm].items():
            f, v1, v6r = sel(v); F += list(f); D += list(v6r - v1 - g6)
        h, e = fir3(np.array(F), np.array(D))
        v6[key] = [round(float(x), 5) for x in h]
        print('plain %s: FIR %s (rms %.2f dB)' % (nm, v6[key], e))
    json.dump(fit, open(os.path.join(HERE, 'osc_fit.json'), 'w'), indent=1)


if __name__ == '__main__':
    main()
