# SPDX-License-Identifier: GPL-3.0-only
"""cmp_osc.py: the own oscillator against the development engine's (osclib.py: NEW against TGT, black-box renders) per wave and
key 0-127 (every third), ORIGINAL (and with --clean CLEAN, SY_OSC_CLEAN=1, against the same target).

Per case and key, a held note (0.6 s; the first 0.1 s left out):
  level   the RMS difference, dB
  lines   the harmonics within 60 dB of the fundamental (below 20 kHz): the largest difference in level (dB) and in
          phase (degrees, the lines above -30 dB), each line read by least squares (osclib.lines) at the measured f1
  alias   everything else in band (20 Hz - 20 kHz, off the lines by 3 bins of a Blackman window) against the lines'
          energy, dB, each engine's
  null    the difference's RMS against the signal, dB
  start   the first 20 ms after the note's start (the start transient, the first cycles): null, dB
python3 cmp_osc.py [--clean] [--quick]"""
import sys, json, numpy as np
import osclib as O
import mlab

FS = O.FS
CASES = [('saw', {}), ('square', dict(sh1=127)), ('pulse 100', dict(sh1=100)), ('pw 90', dict(sh1=127, pw1=90)),
         ('sine', dict(sh1=0)), ('triangle', dict(sh1=0, w1=1)), ('wave 8', dict(sh1=0, w1=8)),
         ('wave 36', dict(sh1=0, w1=36)), ('sine+saw 32', dict(sh1=32)), ('narrow', dict(sh1=0, w1=64)),
         ('osc 2 saw', dict(bal=127, semi2=71)), ('osc 2 square', dict(bal=127, sh2=127, semi2=67)),
         ('mix, detune', dict(bal=64, det=90)), ('ring saw', dict(ring=127, semi2=71)),
         ('ring square', dict(ring=127, sh1=127, sh2=100, semi2=71)), ('ring wave', dict(ring=127, sh1=0, w1=36, semi2=67)),
         ('init phase', dict(iph=40, bal=64, sh2=127))]


def harm(x, f1):
    f1 = O.f1_precise(x, O.f1_precise(x[:16384], f1))
    K = int(19000 / f1)
    ns = list(range(1, max(2, min(K, 120)) + 1))
    _, A, _ = O.lines(x, f1, ns, fit_f1=False)
    return f1, np.array(ns), A


def alias(x, f1):
    N = 2 ** int(np.log2(len(x)))
    x = x[:N]
    X = np.abs(np.fft.rfft(x * np.blackman(N))) ** 2
    f = np.fft.rfftfreq(N, 1 / FS)
    k = np.round(f / f1)
    h = (np.abs(f - k * f1) < 3 * FS / N) & (k >= 1)
    band = (f > 20) & (f < 20000)
    return 10 * np.log10(X[(~h) & band].sum() / max(X[h & band].sum(), 1e-30))


def main():
    clean = '--clean' in sys.argv
    keys = [60] if '--debug' in sys.argv else list(range(0, 128, 9 if '--quick' in sys.argv else 3))
    tgt, new = O.engines()
    env = {'SY_OSC_CLEAN': '1'} if clean else {}
    jobs = []
    for name, kw in CASES:
        for k in keys:
            jobs.append((name, k, O.patch(**kw), [('on', k, 100), ('run', mlab.units(0.7))]))
    A = mlab.render_many(tgt, [(p, e) for _, _, p, e in jobs])
    B = [mlab.render(new, p, e, env=env) for _, _, p, e in jobs]
    rows, out = [], {}
    for (name, k, p, e), a, b in zip(jobs, A, B):
        a, b = a[:, 0], b[:, 0]
        s0 = int(0.1 * FS)
        f0 = 440 * 2 ** ((k - 69) / 12)
        lv = 20 * np.log10(mlab.rms(b[s0:]) / max(mlab.rms(a[s0:]), 1e-12))
        nl = mlab.diff_db(a[s0:], b[s0:])
        sn = mlab.diff_db(a[:int(0.02 * FS)], b[:int(0.02 * FS)])
        try:
            f1, ns, Ha = harm(a[s0:], f0 * (2 if 'osc 2' in name else 1))
            _, Hb, _ = O.lines(b[s0:], f1, list(ns), fit_f1=False)
            ref = np.nanmax(np.abs(Ha))
            m = np.isfinite(Ha) & np.isfinite(Hb) & (np.abs(Ha) > 1e-3 * ref)
            dl = np.max(np.abs(20 * np.log10(np.abs(Hb[m]) / np.abs(Ha[m])))) if m.any() else np.nan
            m2 = m & (np.abs(Ha) > 0.0316 * ref)
            dp = np.max(np.abs(np.angle(Hb[m2] / Ha[m2], deg=True))) if m2.any() else np.nan
            aa, ab = alias(a[s0:], f1), alias(b[s0:], f1)
        except Exception as ex:
            if '--debug' in sys.argv: raise
            dl = dp = aa = ab = np.nan
        rows.append((name, k, lv, dl, dp, aa, ab, nl, sn))
    print('%-14s %4s %8s %8s %8s %8s %8s %8s %8s' % ('case', 'key', 'level', 'lines', 'phase', 'alias T', 'alias N', 'null', 'start'))
    for r in rows:
        print('%-14s %4d %+8.3f %8.3f %8.2f %8.1f %8.1f %8.1f %8.1f' % r)
    print('\nper case: level max |dB|, lines max |dB|, phase max deg, alias difference (N - T) median / max dB, null max, start max')
    for name, _ in CASES:
        R = np.array([r[2:] for r in rows if r[0] == name], float)
        d = R[:, 4] - R[:, 3]
        summ = (np.nanmax(np.abs(R[:, 0])), np.nanmax(R[:, 1]), np.nanmax(R[:, 2]), np.nanmedian(d), np.nanmax(np.abs(d)),
                np.nanmax(R[:, 5]), np.nanmax(R[:, 6]))
        out[name] = summ
        print('%-14s %7.3f %7.3f %7.2f %+6.2f / %5.2f %7.1f %7.1f' % ((name,) + summ))
    import os
    json.dump({'clean': clean, 'rows': rows}, open(os.path.join(O.cbuild.SCR, 'cmp_osc%s.json' % ('_clean' if clean else '')), 'w'))


if __name__ == '__main__':
    main()
