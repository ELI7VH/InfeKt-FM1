# SPDX-License-Identifier: GPL-3.0-only
"""lfo_rate.py: the LFO 1/2 rate table (Hz per value 0..127, sy_tables.c LFO_RATE_HZ) measured on the development engine host
engine as a black box (renders only; nothing of its source or tables is read).
Method: LFO 1 (triangle, key trigger: it starts at its minimum at the note) moves osc 1's pitch (a sine, filters open);
the pitch is read as the log of the carrier's instantaneous frequency (analytic phase). A triangle (in log frequency;
the map is monotonic, so the curve stays symmetric about the minimum) is fitted: rate r, start offset t0, depth and
offset solved linearly (variable projection). The renders span 8 cycles (1.5 s .. 300 s). LFO 2 is the same table (the
spec) and is checked at some values (--check2). Start values for r: the spec's coarse points (sy_tables.c LFO_HZ).
Usage: lfo_rate.py [--write] [--check2]   (prints the table as C; --write patches sy_tables.c and cleaner/tools/measure/lfo_rate.json)"""
import sys, re, json, os
import numpy as np
from scipy.optimize import least_squares
import mlab, lfo_meas

HERE = os.path.dirname(os.path.abspath(__file__))
TAB = os.path.join(mlab.ROOT, 'cleaner', 'engine', 'sy_tables.c')
FS, DEC = mlab.FS, 8


def seeds():
    s = open(TAB).read()
    m = re.search(r'static const pt LFO_HZ\[\] = \{(.*?)\};', s, re.S)
    return np.array([float(x) for x in re.findall(r'\{\d+, ([0-9.e+-]+)f\}', m.group(1))])


def patch(rate, lfo=1):
    P = lfo_meas.patch(1)
    P[68] = 1; P[73] = 1; P[67] = rate          # triangle, key trigger
    if lfo == 2:                                # LFO 2 on osc 1's pitch instead (same bytes, page A 79..)
        raise NotImplementedError
    return P


def logf(y, W):
    n = np.arange(len(y)); c = np.exp(-2j * np.pi * lfo_meas.FT * n / FS)
    w = np.hanning(W); w /= w.sum()
    z = np.convolve(y[:, 0] * c, w, 'same')
    fr = np.gradient(np.unwrap(np.angle(z + 1e-30))) * FS / (2 * np.pi) + lfo_meas.FT
    return np.log(np.maximum(fr, 1.0))


def tri(ph):                                    # starts at -1 (the minimum), 1 at half a cycle
    p = ph - np.floor(ph)
    return np.where(p < 0.5, -1 + 4 * p, 3 - 4 * p)


def fit(t, d, r0, N, A=None):
    """A given (a record of fewer than 1 cycle cannot tell depth from rate): the depth is fixed, from the fits of
    the longer records"""
    def lin(r, t0):
        h = tri(r * (t - t0))
        if A is not None: return A * h + np.mean(d - A * h) - d
        B = np.stack([h, np.ones_like(t)], 1)
        c, *_ = np.linalg.lstsq(B, d, rcond=None)
        lin.A = c[0]
        return B @ c - d
    best = (1e99, None)
    step = 0.04 / N
    for r in r0 * np.exp(np.arange(np.log(0.85), np.log(1.15), step)):
        for t0 in np.linspace(-0.5, 0.5, 9) / r * 0.1:
            e = float(np.sum(lin(r, t0) ** 2))
            if e < best[0]: best = (e, (r, t0))
    r, t0 = best[1]
    s = least_squares(lambda x: lin(x[0], x[1]), [r, t0], x_scale=[r * 1e-3, 1e-3], xtol=1e-14, ftol=1e-14)
    res = s.fun
    lin(s.x[0], s.x[1])
    return s.x[0], s.x[1], float(np.sqrt(np.mean(res ** 2))), (A if A is not None else lin.A)


def measure(v, r0, A=None):
    T = float(np.clip(8.0 / r0, 1.5, 300.0))
    nb = int(T * FS / mlab.BLOCK)
    y = mlab.render(mlab.HYB, patch(v), [('on', lfo_meas.KEY_T, 100), ('run', nb)])
    W = 801 if r0 < 5 else 201
    d = logf(y, W)[W:-W]
    t = (np.arange(len(y))[W:-W]) / FS
    d, t = d[::DEC], t[::DEC]
    return fit(t, d, r0, r0 * T, A)


if __name__ == '__main__':
    sd = seeds()
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(8) as ex:
        slow = [v for v in range(128) if sd[v] * 300.0 < 1.0]            # (less than a cycle in 300 s: none now)
        out = list(ex.map(lambda v: measure(v, sd[v]) if v not in slow else None, range(128)))
        cal = [out[v][3] for v in range(10, 50) if out[v]]       # (slow records, the readout filter does not touch them)
        Ac = float(np.median(cal))
        print('depth (log f) %.5f, spread %.2e' % (Ac, np.ptp(cal)), file=sys.stderr)
        for v in slow: out[v] = measure(v, sd[v], Ac)
    r = np.array([o[0] for o in out])
    print('worst fit residual (log f): %.2e; t0 range %.4f..%.4f s' % (max(o[2] for o in out), min(o[1] for o in out), max(o[1] for o in out)), file=sys.stderr)
    print('worst vs seed: %.3f %%' % (100 * np.max(np.abs(r / sd - 1))), file=sys.stderr)
    txt = ', '.join('%.6gf' % x for x in r)
    print('static const float LFO_RATE_HZ[128] = {' + txt + '};')
    if '--write' in sys.argv:
        json.dump([float('%.6g' % x) for x in r], open(os.path.join(HERE, 'lfo_rate.json'), 'w'))
        s = open(TAB).read()
        s = re.sub(r'static const float LFO_RATE_HZ\[128\] = \{.*?\};', lambda m: 'static const float LFO_RATE_HZ[128] = {' + txt + '};', s, flags=re.S)
        open(TAB, 'w').write(s)
