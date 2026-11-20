# SPDX-License-Identifier: GPL-3.0-only
"""bank_stat.py [--dev]: the test bank (and with --dev the device banks' patches) through the development engine and the cleaner,
judged against the development engine's own variation: the random phases of fresh voices (and the hand-over that follows) make
every render of a patch differ with the moment it is played. Per patch the level and the third-octave band
difference cleaner - development engine, and the floor: the largest of development engine - development engine rendered 1, 2, 3 and 5 blocks later.
Prints the figures, the patches whose difference exceeds the floor (by > 0.1 dB level or > 0.5 dB in a band) and the
max / median."""
import sys, os, glob
import numpy as np
import mlab
from cmp_bank import SEQ

pats = mlab.bank()
if '--dev' in sys.argv:
    d = os.environ.get('MLAB_PATDIR', 'pat')
    pats += [('dev' + os.path.basename(f)[:-4], bytearray(open(f, 'rb').read())) for f in sorted(glob.glob(d + '/*.raw'))]


def rend(exe, pre):
    r = mlab.render_many(exe, [(p, ([('run', pre)] if pre else []) + SEQ) for _, p in pats])
    return [y[pre * 128:] for y in r]


def figs(x, y):
    n = min(len(x), len(y)); x, y = x[:n], y[:n]
    lv = 20 * np.log10((mlab.rms(y) + 1e-15) / (mlab.rms(x) + 1e-15))
    _, bx = mlab.band_db(x); _, by = mlab.band_db(y); m = bx > np.nanmax(bx) - 60
    return lv, float(np.nanmax(np.abs(bx[m] - by[m])))


H0 = rend(mlab.HYB, 0); C0 = rend(mlab.CLN, 0)
F = [rend(mlab.HYB, k) for k in (1, 2, 3, 5)]
rows = []
for i, (nm, _) in enumerate(pats):
    lv, bd = figs(H0[i], C0[i])
    fl = [figs(H0[i], f[i]) for f in F]
    flv, fbd = max(abs(a) for a, _ in fl), max(b for _, b in fl)
    rows.append((nm, lv, bd, flv, fbd))
out = [r for r in rows if abs(r[1]) > r[3] + 0.1 or r[2] > r[4] + 0.5]
print('%-16s %8s %8s %10s %10s' % ('patch', 'level', 'band', 'floor lvl', 'floor band'))
for r in sorted(out, key=lambda r: -(r[2] - r[4])): print('%-16s %8.3f %8.3f %10.3f %10.3f' % r)
lv = np.abs([r[1] for r in rows]); bd = np.array([r[2] for r in rows])
flv = np.array([r[3] for r in rows]); fbd = np.array([r[4] for r in rows])
print('%d patches; cleaner-target |level| max %.3f median %.3f dB, band max %.3f median %.3f dB; the development engine\'s own floor: '
      '|level| max %.3f median %.3f, band max %.3f median %.3f; beyond the floor: %d' %
      (len(rows), lv.max(), np.median(lv), bd.max(), np.median(bd), flv.max(), np.median(flv), fbd.max(), np.median(fbd), len(out)))
