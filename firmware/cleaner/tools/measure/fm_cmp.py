# SPDX-License-Identifier: GPL-3.0-only
"""fm_cmp.py: the cleaner's FM against the development engine's (both host engines as black boxes), per FM amount 0..127, FM modes
0-2 (and 3, 4 for the amounts' table), static and with the filter envelope's FM amount (osc 2 against osc 1 sines, a
sustained tone). Per case: the null (difference RMS against the signal, dB), the level difference, and the largest
difference of a spectral line above -40 dB of the strongest (the FFT bins that are local peaks). Prints max / median.
Usage: fm_cmp.py [--quick]"""
import sys
import numpy as np
from scipy.signal import find_peaks
import mlab, fm_index as F

def spec(y):
    w = np.hanning(len(y)); return np.abs(np.fft.rfft(y * w, 1 << 17))

def cmp(a, b):
    n = min(len(a), len(b)); a, b = a[:n], b[:n]
    nul = mlab.diff_db(a, b)
    lev = 20 * np.log10(mlab.rms(b) / (mlab.rms(a) + 1e-15) + 1e-15)
    A, B = spec(a), spec(b)
    pk, _ = find_peaks(A, height=A.max() * 10 ** (-40 / 20))
    line = np.max(np.abs(20 * np.log10((B[pk] + 1e-12) / (A[pk] + 1e-12)))) if len(pk) else 0.0
    return nul, lev, line

def run(cases, workers=8):
    from concurrent.futures import ThreadPoolExecutor
    def one(c):
        pat, note = c
        ev = [('on', note, 100), ('run', 80)]
        h = mlab.render(mlab.HYB, pat, ev)[:, 0][-int(1.5 * mlab.FS):]
        k = mlab.render(mlab.CLN, pat, ev)[:, 0][-int(1.5 * mlab.FS):]
        return cmp(h, k)
    with ThreadPoolExecutor(workers) as ex:
        return list(ex.map(one, cases))

if __name__ == '__main__':
    step = 4 if '--quick' in sys.argv else 1
    for mode in (0, 1, 2):
        for kind in ('static', 'envelope', 'both'):
            cases, tag = [], []
            for a in range(0, 128, step):
                for fb in ((64,) if kind == 'static' else (64 + (a % 64),)):
                    p = F.patch(a if kind != 'envelope' else 0, 6 if a % 2 else -6, fb=fb, mode=mode)
                    if kind == 'both': p[27] = a
                    cases.append((p, 40 + a % 20)); tag.append(a)
            r = np.array(run(cases))
            print('mode %d %-8s %3d cases: null max %.1f median %.1f dB | level max %.4f dB | line max %.4f median %.4f dB' % (
                mode, kind, len(r), np.nanmax(r[:, 0]), np.median(r[:, 0]), np.abs(r[:, 1]).max(), r[:, 2].max(), np.median(r[:, 2])))
