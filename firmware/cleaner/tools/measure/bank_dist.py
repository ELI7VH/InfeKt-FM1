# SPDX-License-Identifier: GPL-3.0-only
"""bank_dist.py [--dev] [N]: the bank's patches compared as distributions: each patch rendered N times (default 10)
by the development engine and the cleaner, started 0, 3, 6, .. blocks later (the random start phases differ every time, in
either engine), then per patch: the mean level (dB) of each engine with its spread, the difference of the means and
its t value, and the mean third-octave spectra's largest difference (bands within 40 dB of the loudest). A patch is
flagged when the means differ by more than 0.1 dB with t > 3, or the mean spectra by more than 0.5 dB. The floor of
these figures: CLN=<the development engine> OFF=1 (the development engine against itself, started 1 block later each time)."""
import sys, os, glob
import numpy as np
import mlab
from cmp_bank import SEQ

N = int(([a for a in sys.argv[1:] if a.isdigit()] + ['10'])[0])
pats = mlab.bank()
if '--dev' in sys.argv:
    d = os.environ.get('MLAB_PATDIR', 'pat')
    pats += [('dev' + os.path.basename(f)[:-4], bytearray(open(f, 'rb').read())) for f in sorted(glob.glob(d + '/*.raw'))]
pres = [3 * k for k in range(N)]


def stats(exe, p, off=0):
    pr = [k + off for k in pres]
    ys = mlab.render_many(exe, [(p, ([('run', k)] if k else []) + SEQ) for k in pr])
    ys = [y[k * 128:] for y, k in zip(ys, pr)]
    lv = np.array([20 * np.log10(mlab.rms(y) + 1e-15) for y in ys])
    sp = np.mean([10 ** (mlab.band_db(y)[1] / 10) for y in ys], 0)
    return lv, 10 * np.log10(sp + 1e-30)


rows = []
for nm, p in pats:
    lh, sh = stats(mlab.HYB, p); lc, sc = stats(mlab.CLN, p, int(os.environ.get('OFF', '0')))
    dm = lc.mean() - lh.mean(); se = np.sqrt(lh.var(ddof=1) / N + lc.var(ddof=1) / N) + 1e-9
    m = sh > np.nanmax(sh) - 40
    rows.append((nm, lh.mean(), lh.std(), lc.mean(), lc.std(), dm, abs(dm) / se, float(np.nanmax(np.abs(sc[m] - sh[m])))))
fl = [r for r in rows if (abs(r[5]) > 0.1 and r[6] > 3) or r[7] > 0.5]
print('%-16s %8s %6s %8s %6s %7s %6s %7s' % ('patch', 'hyb dB', 'sd', 'cln dB', 'sd', 'diff', 't', 'spec'))
for r in sorted(fl, key=lambda r: -abs(r[5])): print('%-16s %8.2f %6.2f %8.2f %6.2f %7.3f %6.1f %7.3f' % r)
d = np.abs([r[5] for r in rows]); s = np.array([r[7] for r in rows])
print('%d patches x %d starts: |mean level diff| max %.3f median %.3f dB; mean spectra max %.3f median %.3f dB; flagged %d'
      % (len(rows), N, d.max(), np.median(d), s.max(), np.median(s), len(fl)))
