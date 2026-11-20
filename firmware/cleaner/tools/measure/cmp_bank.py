# SPDX-License-Identifier: GPL-3.0-only
"""cmp_bank.py [--dev] [A B]: the test bank rendered by two engines (default: the development engine's and the cleaner's host
engines, mlab.HYB / mlab.CLN), a few notes each: per patch the null test (the difference's RMS against the signal, dB),
the overall level difference (dB), and the largest third-octave band difference (dB, bands within 60 dB of the
loudest). PREB=n renders B with n blocks more before the notes (B = A then: the variation the development engine's own random
phases give, the floor of these figures). --dev adds the device banks' patches (MLAB_PATDIR: a folder of patch files, not part of this release) as more test material.
Prints the worst patches and the max / median of each figure."""
import sys, os, glob
import numpy as np
import mlab

args = [a for a in sys.argv[1:] if not a.startswith('--')]
A, B = (args + [mlab.HYB, mlab.CLN])[:2] if len(args) >= 2 else (mlab.HYB, mlab.CLN)
pats = mlab.bank()
if '--dev' in sys.argv:
    d = os.environ.get('MLAB_PATDIR', 'pat')
    pats += [('dev' + os.path.basename(f)[:-4], bytearray(open(f, 'rb').read())) for f in sorted(glob.glob(d + '/*.raw'))]
SEQ = [('on', 48, 100), ('on', 55, 90), ('on', 64, 110), ('run', mlab.units(1.2)), ('off', 48), ('off', 55), ('off', 64),
       ('run', mlab.units(0.6)), ('on', 36, 64), ('run', mlab.units(0.5)), ('off', 36), ('run', mlab.units(0.3)),
       ('on', 72, 127), ('run', mlab.units(0.4)), ('on', 76, 40), ('run', mlab.units(0.4)), ('off', 72), ('off', 76),
       ('run', mlab.units(0.8))]
jobs = [(p, SEQ) for _, p in pats]
ra = mlab.render_many(A, jobs)
pb = int(os.environ.get('PREB', '0'))
rb = mlab.render_many(B, [(p, [('run', pb)] + SEQ) for _, p in pats]) if pb else mlab.render_many(B, jobs)
if pb: rb = [y[pb * 128:] for y in rb]
rows = []
for (nm, _), x, y in zip(pats, ra, rb):
    n = min(len(x), len(y)); x, y = x[:n], y[:n]
    nul = mlab.diff_db(x, y)
    lv = 20 * np.log10((mlab.rms(y) + 1e-15) / (mlab.rms(x) + 1e-15))
    _, bx = mlab.band_db(x); _, by = mlab.band_db(y)
    m = bx > np.nanmax(bx) - 60
    bd = float(np.nanmax(np.abs(bx[m] - by[m]))) if m.any() else 0.0
    rows.append((nm, nul, lv, bd, 20 * np.log10(mlab.rms(x) + 1e-15)))
rows.sort(key=lambda r: -r[3])
print('%-22s %9s %8s %8s %8s' % ('patch', 'null dB', 'level', 'band', 'rms'))
for r in rows[:int(os.environ.get('TOP', '15'))]:
    print('%-22s %9.1f %8.3f %8.3f %8.1f' % r)
nul = np.array([r[1] for r in rows]); lv = np.abs([r[2] for r in rows]); bd = np.array([r[3] for r in rows])
fin = nul[np.isfinite(nul)]
print('%d patches: bit-identical %d; null max %.1f median %.1f dB; |level| max %.3f median %.3f dB; band max %.3f median %.3f dB'
      % (len(rows), int(np.sum(~np.isfinite(nul))), fin.max() if len(fin) else -np.inf, np.median(nul) if len(nul) else 0,
         lv.max(), np.median(lv), bd.max(), np.median(bd)))
