# SPDX-License-Identifier: GPL-3.0-only
"""filt_dyn.py [HYB CLN] [FS]: filter 1 with its cutoff moving, the cleaner against the development engine (both black boxes), at the
FM-1's rate by default (44100: its codec; the cutoff map's limit to 0.489 fs then falls below cutoff 127, which the
static comparisons at 46875 never reach). The init patch, one note, filter 1 alone (balance 0):
  fenv  the filter envelope (amount 70: ~62 steps, decay 50) from every 4th cutoff, modes LP/HP/BP/BS/analog 2/4,
        resonance 64..127
  lfo   LFO 2 on cutoff 1 (rate 100, sine, amount 127), the same
  key   keyfollow 127, notes 24..96 (fractional cutoffs)
Per case the null (dB), the largest level difference over 512-frame windows within 40 dB of the loudest (dB) and the
total level difference; prints the worst cases and a count above 0.1 dB."""
import sys, itertools
import numpy as np
from concurrent.futures import ThreadPoolExecutor
import mlab

H, C = (sys.argv[1], sys.argv[2]) if len(sys.argv) > 2 else (mlab.HYB, mlab.CLN)
FS = sys.argv[3] if len(sys.argv) > 3 else '44100'


def frames(a, b, w=512):
    n = min(len(a), len(b)) // w
    ra = np.sqrt(np.mean(a[:n * w].reshape(n, w, -1) ** 2, axis=(1, 2)))
    rb = np.sqrt(np.mean(b[:n * w].reshape(n, w, -1) ** 2, axis=(1, 2)))
    m = ra > ra.max() * 0.01
    return float(np.max(np.abs(20 * np.log10((rb[m] + 1e-12) / (ra[m] + 1e-12)))))


jobs, keys = [], []
for kind, m, r, c in itertools.product(('fenv', 'lfo', 'key'), (0, 1, 2, 3, 5, 7), (64, 112, 120, 124, 127), range(0, 128, 8)):
    p = mlab.init_preset()
    p[mlab.CHORUS_MIX] = 0; p[mlab.FX_SEND] = 0; p[mlab.F_BAL] = 0
    p[mlab.F1_MODE] = m; p[mlab.F1_RESO] = r; p[mlab.CUTOFF] = c
    notes = [48]
    if kind == 'fenv':
        p[mlab.F1_ENV] = 70; p[54] = 0; p[55] = 50; p[56] = 0; p[58] = 30
    elif kind == 'lfo':
        p[mlab.LFO2_RATE] = 100; p[mlab.LFO2_SHAPE] = 0; p[88] = 127
    else:
        p[mlab.F1_KF] = 127; notes = [24, 60, 96]
    ev = []
    for n in notes:
        ev += [('on', n, 100), ('run', 60), ('off', n), ('run', 8)]
    jobs.append((p, ev)); keys.append((kind, m, r, c))
env = {'SY_FS': FS}
with ThreadPoolExecutor(8) as ex:
    A = list(ex.map(lambda j: mlab.render(H, *j, env=env), jobs))
    B = list(ex.map(lambda j: mlab.render(C, *j, env=env), jobs))
res = []
for k, a, b in zip(keys, A, B):
    lv = 20 * np.log10((mlab.rms(b) + 1e-15) / (mlab.rms(a) + 1e-15))
    res.append((frames(a, b), lv, mlab.diff_db(a, b), k))
res.sort(key=lambda x: -x[0])
for w in res[:20]:
    print('window max %6.3f dB  total %+.3f dB  null %6.1f dB  %s' % w)
for kind in ('fenv', 'lfo', 'key'):
    rk = [w for w in res if w[3][0] == kind]
    print('%-4s cases %d, windows above 0.1 dB: %d, totals above 0.1 dB: %d, window max %.3f dB, null median %.1f dB' % (
        kind, len(rk), sum(w[0] > 0.1 for w in rk), sum(abs(w[1]) > 0.1 for w in rk), max(w[0] for w in rk),
        float(np.median([w[2] for w in rk]))))
