# SPDX-License-Identifier: GPL-3.0-only
"""osclib.py: the oscillator fits' common parts (tools/measure/osc/). Everything is measured on renders of an engine
run as a black box through the lab's command line (mlab.py): TGT, the target, is the cleaner's host engine built with
the development engine's oscillator (-DCLN_OSC=0, the tick clock, the same as the development engine's host engine there); NEW the cleaner's
own (-DCLN_OSC=1, the default). Nothing of either's source or tables is read.

The model the fits use (sy_cln_osc.inc, PROVENANCE.md): osc 1's output is periodic, P clock ticks a cycle; its line n
(the n-th harmonic of f1) is
    S_n = X_(n mod P) * T(n / P) * exp(-2 pi i n lat / P) * sinc^2(n / P)
X the P-point DFT of the naive wave on the ticks, T(x) = s2 + 2 s1 cos(2 pi x) + 2 s0 cos(4 pi x) the step response's
taps (s0 = C(1), s1 = C(2) - C(1), s2 = 1 - 2 C(2)), lat the naive wave's delay (ticks); sampled at fs the line lands at
n f1 folded, where the emphasis E(f) = eb - ea exp(-2 pi i f / fs) and the post chain L(f) (filter 1 open, the amp:
measured here through the audio input) take it.
    f1_precise(x, f)        a line's frequency from its phase's slope
    lines(x, f1, ns)        the complex amplitudes of the lines n f1 (folded) in a render, by least squares
    post_chain()            L(f) (the input route of the same patch: white noise in, the cross spectrum)
    saw_x(P, a, t0)         the naive saw's DFT on P ticks (phase a a tick, t0 after the restart)"""
import os, sys, json
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import mlab, cbuild

FS = mlab.FS
PB = 128
FIT = os.path.join(HERE, 'osc_fit.json')


def engines():
    """(target, own): the cleaner's host engine with the development engine's oscillator and with its own"""
    tgt = os.environ.get('OSC_TGT') or cbuild.build('osc_tick', ['-DCLN_OSC=0'], all_units=True)
    new = os.environ.get('OSC_NEW') or cbuild.build('osc_own', [], all_units=True)
    return tgt, new


def patch(sh1=64, w1=0, pw1=64, sh2=64, w2=0, pw2=64, bal=0, ring=0, semi1=64, semi2=64, det=64, iph=0, vol=64, kf1=96):
    p = mlab.init_preset()
    p[17], p[18], p[19], p[20], p[21] = sh1, pw1, w1, semi1, kf1
    p[22], p[23], p[24], p[25], p[26] = sh2, pw2, w2, semi2, det
    p[33], p[38], p[36] = bal, ring, vol
    p[PB + 35] = iph
    return p


def fold(f):
    """a frequency (Hz, any) folded to [0, fs/2]; and whether it came in mirrored (its line conjugated)"""
    a = np.mod(f, FS)
    neg = a > FS / 2
    return np.where(neg, FS - a, a), neg


def lines(x, f1, ns, fit_f1=True):
    """least-squares complex amplitudes of the lines n f1 (folded, conjugated where mirrored) in x; f1 refined first by
    the residual (a golden search over +-0.02 Hz) when fit_f1. -> (f1, amps, residual rms re signal rms)"""
    x = np.asarray(x, float)
    t = np.arange(len(x)) / FS
    w = np.hanning(len(x))

    def solve(f):
        fo, neg = fold(np.asarray(ns, float) * f)
        keep = np.ones(len(ns), bool)
        order = np.argsort(fo)
        for i in range(1, len(order)):          # (lines closer than the resolution: the later one dropped)
            if fo[order[i]] - fo[order[i - 1]] < 1.5 * FS / len(x):
                keep[order[i]] = False
        cols = []
        for i in np.where(keep)[0]:
            if fo[i] < 1e-9:
                cols += [np.ones_like(t)]
            else:
                cols += [np.cos(2 * np.pi * fo[i] * t), np.sin(2 * np.pi * fo[i] * t)]
        A = np.stack(cols, 1)
        c, *_ = np.linalg.lstsq(A * w[:, None], x * w, rcond=None)
        r = x * w - (A * w[:, None]) @ c
        amps = np.full(len(ns), np.nan + 0j)
        j = 0
        for i in np.where(keep)[0]:
            if fo[i] < 1e-9:
                amps[i] = c[j]; j += 1
            else:
                z = c[j] - 1j * c[j + 1]; j += 2
                amps[i] = np.conj(z) if neg[i] else z
        return float(np.sqrt(np.mean(r ** 2) / np.mean((x * w) ** 2))), amps

    if fit_f1:
        lo, hi = f1 - 0.02 * max(1, f1 / 1000), f1 + 0.02 * max(1, f1 / 1000)
        g = (np.sqrt(5) - 1) / 2
        for _ in range(40):
            a, b = hi - g * (hi - lo), lo + g * (hi - lo)
            if solve(a)[0] < solve(b)[0]: hi = b
            else: lo = a
        f1 = (lo + hi) / 2
    res, amps = solve(f1)
    return f1, amps, res


def f1_precise(x, f, k=1):
    """the frequency of the k-th line near k f (Hz), from its phase's slope: the line demodulated, averaged in chunks
    of whole periods, its unwrapped phase fitted with a straight line (twice)"""
    x = np.asarray(x, float)
    t = np.arange(len(x)) / FS
    for _ in range(3):
        z = x * np.exp(-2j * np.pi * k * f * t)
        L = min(int(round(FS / f * max(4, int(f / 20)))), len(x) // 8)   # (8 chunks at least)
        n = len(z) // L
        c = z[:n * L].reshape(n, L).mean(1)
        ph = np.unwrap(np.angle(c))
        tc = (np.arange(n) + 0.5) * L / FS
        f = f + np.polyfit(tc, ph, 1)[0] / (2 * np.pi * k)
    return f


def post_chain(p=None, note=60, n=2 ** 16):
    """L(f) of the patch's post chain (filter, amp): its input route (input mode 1) with white noise in; -> (f, L)"""
    p = bytearray(p if p is not None else patch())
    p[101] = 1
    rng = np.random.default_rng(1)
    inp = np.zeros((n + 8192, 2))
    inp[:, 0] = inp[:, 1] = rng.standard_normal(n + 8192) * 0.05
    tgt, _ = engines()
    y = mlab.render(tgt, p, [('on', note, 100), ('run', mlab.units((n + 8192) / FS))], inp=inp)
    k = 4096
    x = inp[4096:4096 + n, 0]; z = y[4096:4096 + n, 0]
    Sxy = 0; Sxx = 0
    for s in range(0, n - 2 * k, k // 2):
        X = np.fft.rfft(x[s:s + 2 * k] * np.hanning(2 * k)); Z = np.fft.rfft(z[s:s + 2 * k] * np.hanning(2 * k))
        Sxy = Sxy + Z * np.conj(X); Sxx = Sxx + np.abs(X) ** 2
    return np.fft.rfftfreq(2 * k, 1 / FS), Sxy / Sxx


def interp_c(f, F, L):
    """L at f, interpolated in magnitude and unwrapped phase"""
    m = np.interp(f, F, np.abs(L)); ph = np.interp(f, F, np.unwrap(np.angle(L)))
    return m * np.exp(1j * ph)


def saw_x(P, a, t0):
    """the P-point DFT (1/P) of the naive falling saw 1 - 2 (t0 + j a), j = 0 .. P-1"""
    j = np.arange(P)
    return np.fft.fft(1 - 2 * (t0 + j * a)) / P


def taps(c1, c2, x):
    s0, s1, s2 = c1, c2 - c1, 1 - 2 * c2
    return s2 + 2 * s1 * np.cos(2 * np.pi * x) + 2 * s0 * np.cos(4 * np.pi * x)


def load():
    return json.load(open(FIT)) if os.path.exists(FIT) else {}


def save(part, val):
    F = load()
    F[part] = val
    json.dump(F, open(FIT, 'w'), indent=1)
