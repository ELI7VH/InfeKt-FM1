# SPDX-License-Identifier: GPL-3.0-only
"""cmp_comp.py [HYB CLN]: per component, the cleaner against the development engine (both black boxes), on renders that isolate
it; per case the null (the difference's RMS against the signal, dB and %) and the level difference (dB):
  filter1   noise through the static path (input mode 2), cutoff 0..127 in steps of 3, resonance 0 / 40 / 64 / 100 / 127
  sat5      a 220 Hz sine (-20 / -6 dBFS) through the static path, saturation 5, drive 64..127 in steps of 3
  dist1-4   a 220 Hz sine (-30 / -18 / -6 / -1 dBFS) straight into the effects, types 1-4, intensity 0..127 in 9s
  dist8/9   the same, types 8 and 9
  shape     osc 1 alone, shapes 60..127, the first note (its phases deterministic in both engines)
Prints the max and the median per component and every case above 1 %."""
import sys
import numpy as np
import mlab

H, C = (sys.argv[1], sys.argv[2]) if len(sys.argv) > 2 else (mlab.HYB, mlab.CLN)
N = int(0.5 * mlab.FS); t = np.arange(N) / mlab.FS
rng = np.random.default_rng(5)
noise = np.clip(rng.standard_normal(N) * 0.05, -1, 1)


def static(kv):
    p = mlab.init_preset()
    p[mlab.INPUT_MODE] = 2; p[mlab.INPUT_SELECT] = 2; p[mlab.OSC_VOL] = 64; p[mlab.CHORUS_MIX] = 0; p[mlab.FX_SEND] = 0
    p[mlab.CUTOFF] = 127; p[mlab.CUTOFF2] = 64; p[mlab.F1_RESO] = 0; p[mlab.F2_RESO] = 0; p[mlab.F_BAL] = 0
    for k, v in kv.items(): p[k] = v
    return p


def fx(t_, it):
    p = mlab.init_preset()
    p[mlab.INPUT_MODE] = 3; p[mlab.INPUT_SELECT] = 2; p[mlab.CHORUS_MIX] = 0; p[mlab.FX_SEND] = 0; p[mlab.FX_MODE] = 0
    p[mlab.DIST_TYPE] = t_; p[mlab.DIST_INT] = it
    return p


def st(x):
    return np.stack([x, x], 1)


cases = {}
cases['filter1'] = [('c%d r%d' % (c, r), (static({mlab.CUTOFF: c, mlab.F1_RESO: r}), [('run', N // 128)],
                     st(noise * (0.05 if r >= 100 else 1)))) for c in range(0, 128, 3) for r in (0, 40, 64, 100, 127)]
cases['sat5'] = [('d%d %ddB' % (d, lv), (static({mlab.SAT_TYPE: 5, mlab.OSC_VOL: d}), [('run', N // 128)],
                  st(10 ** (lv / 20) * np.sin(2 * np.pi * 220 * t)))) for d in range(64, 128, 3) for lv in (-20, -6)]
for ty in (1, 2, 3, 4, 8, 9):
    cases['dist%d' % ty] = [('i%d %ddB' % (it, lv), (fx(ty, it), [('run', N // 128)], st(10 ** (lv / 20) * np.sin(2 * np.pi * 220 * t))))
                           for it in list(range(0, 128, 9)) + [127] for lv in (-30, -18, -6, -1)]


def oscp(sh):
    p = mlab.init_preset()
    p[mlab.OSC1_SHAPE] = sh; p[mlab.OSC_BAL] = 0; p[mlab.CHORUS_MIX] = 0; p[mlab.FX_SEND] = 0
    return p


cases['shape'] = [('s%d' % s, (oscp(s), [('on', 45, 100), ('run', 150)], None)) for s in range(60, 128, 2)]
print('%-8s %6s %10s %10s %9s %9s  %s' % ('part', 'cases', 'null max', 'median', '|lvl| max', 'band max', 'cases above 1 % (null; band dB)'))
for nm, cs in cases.items():
    jobs = [c[1] for c in cs]
    a, b = mlab.render_many(H, jobs), mlab.render_many(C, jobs)
    nul, lv, bad, bd = [], [], [], []
    for (cn, _), x, y in zip(cs, a, b):
        x, y = x[2048:], y[2048:]
        d = mlab.diff_db(x, y); nul.append(d)
        lv.append(abs(20 * np.log10((mlab.rms(y) + 1e-15) / (mlab.rms(x) + 1e-15))))
        _, bx = mlab.band_db(x); _, by = mlab.band_db(y); m = bx > np.nanmax(bx) - 40
        bd.append(float(np.nanmax(np.abs(bx[m] - by[m]))))
        if d > -40: bad.append('%s %.2f%%; %.2f' % (cn, 100 * 10 ** (d / 20), bd[-1]))
    nul = np.array(nul)
    print('%-8s %6d %6.1f dB %6.1f dB %7.3f dB %6.3f dB  %s' % (nm, len(cs), nul.max(), np.median(nul), max(lv), max(bd),
          ', '.join(bad[:8]) + (' ... (%d)' % len(bad) if len(bad) > 8 else '')))
