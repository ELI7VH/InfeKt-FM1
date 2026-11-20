# SPDX-License-Identifier: GPL-3.0-only
"""fm_index.py: the FM index law (osc 2's peak deviation as a share of its own frequency, per FM amount 0..127 and per
filter-envelope amount 64..127) measured on the development engine host engine as a black box (renders only; nothing of its source
or tables is read). Regenerates cleaner/engine/sy_cln_fm.h (FM_IA[128], FM_IB[64], 1/256 units).
Method: osc 2 (sine) is frequency-modulated by osc 1 (sine, silent in the mix; FM mode 2, bipolar). A sustained tone is
rendered; the carrier f2 and the modulator f1 are read from single-oscillator renders (parabolic FFT peak); the complex
amplitude at every f2 + k f1 (k with a positive frequency) is taken by a Hann-windowed DFT at that exact frequency; the
modulation index beta is the one whose |J_k(beta)| (Bessel) best fits those magnitudes (scale solved linearly). The
index word is beta / (f2 / f1) (the deviation is a share of osc 2's own frequency). Several pitch ratios (semitone
differences D) and notes per value; the median is the value, the spread is reported. The envelope part: amount 0, the
filter envelope held at full (attack 0, sustain 127), envelope amount 64..127; the index there is the table B.
Usage: fm_index.py [--write] [--engine PATH]    (prints the check; --write regenerates sy_cln_fm.h; fm_index.json holds the 128 + 64 values)"""
import sys, os, json
import numpy as np
from scipy.special import jv
from scipy.optimize import minimize_scalar
import mlab

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_H = os.path.join(mlab.ROOT, 'cleaner', 'engine', 'sy_cln_fm.h')
OUT_J = os.path.join(HERE, 'fm_index.json')
FS = mlab.FS
DS = (6, 2, -6)            # osc 2 semitones above osc 1 (ratios 1.414, 1.122, 0.707: no small-integer coincidences)
NOTES = (36, 48, 55)       # osc 1's notes
LEN = 150                  # blocks rendered (3.2 s); the last 2 s are analysed
KMAX = 40


def patch(amt, d, fb=64, mode=2, bal=127, a1=True):
    p = mlab.init_preset()
    for b, v in ((17, 0), (22, 0), (19, 0), (24, 0), (18, 0), (23, 0), (20, 64), (25, 64 + d), (26, 0), (27, amt), (28, 0),
                 (29, 64), (30, fb), (mlab.OSC_BAL, bal), (34, 0), (35, 0), (36, 64), (37, 0), (38, 0),
                 (mlab.CUTOFF, 127), (mlab.CUTOFF2, 127), (mlab.F1_RESO, 0), (mlab.F2_RESO, 0), (mlab.F1_ENV, 64), (mlab.F2_ENV, 64),
                 (54, 0), (56, 127), (57, 64), (59, 0), (60, 0), (61, 127), (63, 0), (mlab.CHORUS_MIX, 0), (mlab.FX_SEND, 0),
                 (94, 0), (97, 0), (67, 0), (79, 0), (mlab.PB + 34, mode)):
        p[b] = v
    return p


def tone(exe, pat, note):
    y = mlab.render(exe, pat, [('on', note, 100), ('run', LEN)])[:, 0]
    return y[-int(2 * FS):]


def peak_freq(y, f0):
    w = np.hanning(len(y)); n = 1 << 21
    S = np.abs(np.fft.rfft(y * w, n)); f = np.fft.rfftfreq(n, 1 / FS)
    m = (f > f0 * 0.9) & (f < f0 * 1.1)
    i = np.argmax(np.where(m, S, 0))
    a, b, c = np.log(S[i - 1:i + 2] + 1e-30)
    return f[i] + 0.5 * (a - c) / (a - 2 * b + c) * (f[1] - f[0])


def comp(y, f):
    n = np.arange(len(y)); w = np.hanning(len(y))
    return abs(np.sum(y * w * np.exp(-2j * np.pi * f * n / FS))) / np.sum(w) * 2


def fit_beta(y, f1, f2, hi=60.0):
    ks = [k for k in range(-KMAX, KMAX + 1) if 30.0 < f2 + k * f1 < 0.45 * FS]
    A = np.array([comp(y, f2 + k * f1) for k in ks]); ks = np.array(ks)
    top = A.max(); sel = A > top * 2e-4          # components above -74 dB
    A, ks = A[sel], ks[sel]

    def cost(b):
        J = np.abs(jv(ks, b)); g = (A @ J) / (J @ J + 1e-30)
        return np.sum((A - g * J) ** 2)
    bs = np.geomspace(0.003, hi, 1500)
    c = [cost(b) for b in bs]; i = int(np.argmin(c))
    r = minimize_scalar(cost, bounds=(bs[max(i - 1, 0)], bs[min(i + 1, len(bs) - 1)]), method='bounded', options={'xatol': 1e-7})
    return r.x, np.sqrt(r.fun / np.sum(A ** 2))


def pitches(exe, d, note):
    f1 = peak_freq(tone(exe, patch(0, d, bal=0), note), 440 * 2 ** ((note - 69) / 12))
    f2 = peak_freq(tone(exe, patch(0, d, bal=127), note), 440 * 2 ** ((note + d - 69) / 12))
    return f1, f2


def measure(exe, amts, fbs, workers=8):
    """index words: A[amt] (static), B[fb] (envelope held, amount 0)"""
    from concurrent.futures import ThreadPoolExecutor
    cases = [(d, n) for d in DS for n in NOTES]
    fr = {c: pitches(exe, *c) for c in cases}
    jobs = [('A', a, c) for a in amts for c in cases] + [('B', b, c) for b in fbs for c in cases]

    def run(j):
        kind, v, (d, n) = j
        pat = patch(v, d) if kind == 'A' else patch(0, d, fb=v)
        f1, f2 = fr[(d, n)]
        y = tone(exe, pat, n)
        beta, res = fit_beta(y, f1, f2)
        return (kind, v, d, n, beta / (f2 / f1), res)
    with ThreadPoolExecutor(workers) as ex:
        return list(ex.map(run, jobs))


def tabulate(rows, kind, vals):
    med, spr, res = [], [], []
    for v in vals:
        r = [x for x in rows if x[0] == kind and x[1] == v]
        iw = np.array([x[4] for x in r]); med.append(float(np.median(iw)))
        spr.append(float((iw.max() - iw.min()) / (np.median(iw) + 1e-9))); res.append(max(x[5] for x in r))
    return np.array(med), np.array(spr), np.array(res)


STEP = 4                   # the law is straight between values 4 apart (checked below against all the measured values)


def nodes(v, x0, n):
    """table nodes at x0 + 4 i (n of them, the last one past the range end: the line through the last measured
    values continued); max error of the linear interpolation against all measured values"""
    v = np.asarray(v, float); x = np.arange(x0, x0 + len(v))
    v = np.where(np.abs(v) < 0.01, 0.0, v)         # (a fit's noise floor, where the true index is 0)
    sn = lambda a: 0.0 if abs(a) < 0.01 else round(float(a), 3)       # (the fit's noise, 1e-4, snapped away)
    t = [sn(v[STEP * i]) for i in range(n - 1)]
    y127 = sn(v[-1])                                  # (the range's last value, the table's last node past it)
    xl = x0 + len(v) - 1                              # (its amount: 127; the last full node's: 124)
    xn = x0 + STEP * (n - 2)
    t.append(round(y127 + (y127 - t[-1]) / (xl - xn) * (x0 + STEP * (n - 1) - xl), 5))
    t = np.array(t)
    ip = np.interp(x, x0 + STEP * np.arange(n), t)
    return t, float(np.max(np.abs(ip - v))), float(np.max(np.abs(ip - v) / np.maximum(v, 0.05)))


def write_header(ta, tb, ea, eb):
    f = lambda t: ', '.join('%.5ff' % a for a in t)
    open(OUT_H, 'w').write("""/* SPDX-License-Identifier: GPL-3.0-only */
/* GENERATED by tools/measure/fm_index.py (do not edit): the FM index law measured on black-box renders of the development engine
 * host engine (Bessel fit of the sidebands of a sine modulating a sine, 3 pitch ratios x 3 notes per value). The index
 * is osc 2's peak deviation as a share of its own frequency; straight lines between nodes 4 amount steps apart
 * (max error against the 128 measured values: %.4f abs, %.2f %% rel for the amount, %.4f abs, %.2f %% rel for the
 * envelope amount). FM_IA: the amount 0, 4 .. 124, 128; FM_IB: the filter envelope's amount 64, 68 .. 124, 128. */
static const float FM_IA[33] = {%s};
static const float FM_IB[17] = {%s};
""" % (ea[0], ea[1] * 100, eb[0], eb[1] * 100, f(ta), f(tb)))


if __name__ == '__main__':
    exe = mlab.HYB
    if '--engine' in sys.argv: exe = sys.argv[sys.argv.index('--engine') + 1]
    amts = list(range(0, 128)); fbs = list(range(64, 128))
    rows = measure(exe, [a for a in amts if a], fbs)
    ia, sa, ra = tabulate(rows, 'A', [a for a in amts if a])
    ib, sb, rb = tabulate(rows, 'B', fbs)
    ia = np.concatenate([[0.0], ia]); sa = np.concatenate([[0.0], sa])
    json.dump({'ia': ia.tolist(), 'ib': ib.tolist(), 'spread_a': sa.tolist(), 'spread_b': sb.tolist()}, open(OUT_J, 'w'))
    print('A: median spread across ratios/notes %.4f max %.4f; fit residual max %.4f' % (np.median(sa), sa.max(), ra.max()))
    print('B: median spread %.4f max %.4f; fit residual max %.4f' % (np.median(sb), sb.max(), rb.max()))
    ta, e1a, e2a = nodes(ia, 0, 33); tb, e1b, e2b = nodes(ib, 64, 17)
    print('nodes A', ta.tolist()); print('nodes B', tb.tolist())
    print('interpolation error A: %.4f abs %.3f%% rel; B: %.4f abs %.3f%% rel' % (e1a, e2a * 100, e1b, e2b * 100))
    if '--write' in sys.argv: write_header(ta, tb, (e1a, e2a), (e1b, e2b)); print('wrote', OUT_H)
