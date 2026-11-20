# SPDX-License-Identifier: GPL-3.0-only
"""clock.py: the oscillators' clock, measured on black-box renders of the target (osclib.py): how many ticks P a cycle
of osc 1 takes, by its pitch; the bits of osc 1's phase; writes osc_fit.json "clock".

1. P exactly where the images show (keys 84-127, a sine on osc 1 alone): a sine sampled on P ticks a cycle and joined
   by straight lines has, besides its own line, images at (P - 1) f1 and (P + 1) f1 (each 1 / (P -+ 1)^2 of it, whatever
   the filter on the ticks), folded at fs. The two strongest lines besides the sine's give P: the whole number whose
   images land on both. f1 from the sine's own line (osclib.lines).
2. The segments: a pitch-bend sweep over an octave (keys 96-109, the bend up in 16 steps; each change of P bisected): P x 2^o is constant over sixths
   of an octave apart (the edges), N_k per segment (four: two of the sixths share theirs); the octave's start F0 (a
   share of fs) the edge the pattern of gaps (1, 1, 2, 2 sixths) starts from.
   The same law checked at every key 84-127 and, below key 84 (images too faint), on the saw's roll-off: the clock's
   rate f1 P against the top harmonics' fall (T(f / (f1 P)) sinc^2), within 1 %.
3. The phase's bits: osc 1 restarts on its P-th tick when its step a tick is the largest multiple of 2^(32 - b) under
   2^32 / P; on a b-bit phase P x step falls short of a cycle by up to P x 2^(32 - b), and for P over 2^(b - 23 + 11)
   it can take a tick more: f1 flat by P / (P + 1). Keys 0-36 (P up to 14336): the measured f1 against the own engine
   built with a 32-bit step (the same pitch otherwise) for b = 16 .. 32: the b that predicts every key's ratio.
4. The restart's phase t0 (cycles): the saw's mean on its P ticks, 1 - 2 t0 - (P - 1) a, from its DC line (kernel.py
   reads it with the taps: there)."""
import sys, numpy as np
import osclib as O
import mlab

FS = O.FS


def sine_p(tgt, note, bend=8192, sec=1.6):
    p = O.patch(sh1=0)
    ev = [('midi', 0xE0, bend & 127, bend >> 7), ('on', note, 100), ('run', mlab.units(sec))]
    x = mlab.render(tgt, p, ev)[:, 0][6000:]
    f1 = 440 * 2 ** ((note - 69) / 12)
    N = 2 ** int(np.log2(len(x)))
    x = x[:N]
    X = np.abs(np.fft.rfft(x * np.blackman(N)))
    f = np.fft.rfftfreq(N, 1 / FS)
    i = np.argmax(X[f > 15]) + np.sum(f <= 15)
    a, b, c = np.log(X[i - 1:i + 2]); d = 0.5 * (a - c) / (a - 2 * b + c)
    f1 = (i + d) * FS / N
    f1, amps, _ = O.lines(x, f1, [1])
    # the strongest lines besides the sine's (local maxima, away from it)
    floor = np.median(X)
    pk = [j for j in range(2, len(X) - 2) if X[j] == X[j - 2:j + 3].max() and abs(f[j] - f1) > 20 and X[j] > 30 * floor]
    pk = sorted(pk, key=lambda j: -X[j])[:6]
    gs = [f[j] for j in pk]
    best = None
    # (the images' level, 1 / (P - 1)^2 of the sine's through the same filters within a few dB: P under 4 x that)
    pe = 10 ** (-20 * np.log10(X[pk[0]] / X[i]) / 40) + 1 if pk else 2
    Ps = np.arange(2, int(4 * pe) + 3)
    for s in (-1, 1):
        g = O.fold((Ps + s) * f1)[0]
        for gi in gs[:2]:
            hit = np.abs(g - gi) < 3 * FS / N
            for P in Ps[hit]:
                gm, _ = O.fold(np.array([(P - 1) * f1, (P + 1) * f1]))
                sc = sum(min(abs(gm[0] - q), abs(gm[1] - q)) < 3 * FS / N for q in gs[:3])
                if best is None or sc > best[0] or (sc == best[0] and P < best[1]): best = (sc, int(P))
    P = best[1] if best and best[0] >= 2 else None
    while P and P % 2 == 0 and P > 4:                # (images of 2P matched: those of P too, where the top's filters
        gm, _ = O.fold(np.array([(P // 2 - 1) * f1, (P // 2 + 1) * f1]))   # leave them: P)
        lv = [X[int(round(g * N / FS)) - 2:int(round(g * N / FS)) + 3].max() for g in gm]
        if min(lv) > 30 * floor: P //= 2
        else: break
    return f1, P, [20 * np.log10(X[j] / X[i]) for j in pk[:2]]


def main():
    tgt, new = O.engines()
    out = {}
    # 1. keys 84-127
    print('1. P from the images, keys 84-127')
    keys = {}
    for n in range(84, 128):
        f1, P, lv = sine_p(tgt, n)
        keys[n] = (f1, P)
        print('  key %3d  f1 %9.3f  P %s  (images %s dB)' % (n, f1, P, ' '.join('%.0f' % v for v in lv)))
    # 2. the sweep over an octave
    print('2. sweep keys 96-108 by the bend')
    pts = []
    for n in range(96, 110):
        for b in range(0, 16):
            bend = 8192 + b * 512                       # (up the bend's range in 16 steps: f1 measured anyway)
            f1, P, _ = sine_p(tgt, n, bend, sec=0.8)
            if P: pts.append((f1, P, n, bend))
    pts.sort()
    pts = [q for i, q in enumerate(pts) if 0 < i < len(pts) - 1 and (q[1] == pts[i - 1][1] or q[1] == pts[i + 1][1])] \
        + [pts[0], pts[-1]]                             # (a lone P between two others: a misread, dropped)
    pts.sort()
    f1s = np.array([p[0] for p in pts]); Ps = np.array([p[1] for p in pts])
    jumps = []
    for i in range(len(Ps) - 1):                     # each change of P bisected on the bend to 1/32 of its step
        if Ps[i] == Ps[i + 1]: continue
        a, b = pts[i], pts[i + 1]
        if a[2] == b[2]:
            for _ in range(6):
                m = (a[3] + b[3]) // 2
                if m in (a[3], b[3]): break
                f1, P, _ = sine_p(tgt, a[2], m, sec=0.8)
                if P == a[1]: a = (f1, P, a[2], m)
                elif P == b[1]: b = (f1, P, a[2], m)
                else: break
        jumps.append((a[0], b[0], a[1], b[1]))
    edges = np.array([np.sqrt(a * b) for a, b, _, _ in jumps])          # (each between the two pitches around it)
    print('  P changes (Hz):', ['%.1f-%.1f: %d->%d' % j for j in jumps])
    gaps = 6 * np.log2(edges[1:] / edges[:-1])
    print('  gaps between them (sixths of an octave):', np.round(gaps, 3))
    # the octave's start: the edge before two gaps of a sixth (the pattern 1, 1, 2, 2 repeats every octave); its
    # octave taken in fs / 1024 .. fs / 512 (any octave: N_k scales with it)
    i0 = [i for i in range(len(gaps) - 1) if abs(gaps[i] - 1) < 0.1 and abs(gaps[i + 1] - 1) < 0.1][0]
    F0 = edges[i0] / 2 ** np.floor(np.log2(edges[i0] / (FS / 1024)))
    e = [x / F0 / 2 ** np.floor(np.log2(x / F0)) for x in edges[i0 + 1:i0 + 4]]
    segE = sorted(e)
    o = np.floor(np.log2(f1s / F0)); xs = f1s / (F0 * 2 ** o); N = Ps * 2 ** o
    segN = []
    for k in range(len(segE) + 1):
        lo = 1.0 if k == 0 else segE[k - 1]; hi = 2.0 if k == len(segE) else segE[k]
        m = (xs > lo * 1.001) & (xs < hi * 0.999)
        segN.append(int(np.round(np.median(N[m]))))
    print('  F0 = fs / %.4f, N per segment %s, edges %s (2^(k/6): %s)' % (FS / F0, segN, ['%.5f' % v for v in segE],
          ['%.3f' % (6 * np.log2(v)) for v in segE]))
    # (F0 measured between two bend steps: the power-of-two share of fs within that step taken, if one is)
    lo, hi = [(a, b) for a, b, _, _ in jumps][i0]
    k2 = round(np.log2(FS / F0))
    if lo / 2 ** np.floor(np.log2(edges[i0] / (FS / 1024))) <= FS / 2 ** k2 <= hi / 2 ** np.floor(np.log2(edges[i0] / (FS / 1024))):
        F0 = FS / 2 ** k2
        print('  F0 = fs / %d (within the measured step)' % 2 ** k2)
    out['f0'] = float(F0 / FS)
    out['seg_n'] = segN
    out['seg_e_measured'] = [float(v) for v in segE]
    out['seg_e'] = [float(2 ** (round(6 * np.log2(v)) / 6)) for v in segE]   # (sixths: each within its measured gap)
    # the law at every key 84-127
    bad = 0
    for n, (f1, P) in keys.items():
        q = f1 / (out['f0'] * FS); oo = int(np.floor(np.log2(q))); x = q / 2 ** oo
        k = sum(x >= e for e in out['seg_e'])
        Pl = int(round(segN[k] / 2 ** oo))
        if P is not None and Pl != P: bad += 1; print('  key %d: law %d, measured %d' % (n, Pl, P))
    print('  law against keys 84-127: %d off' % bad)
    # 3. the phase's bits, keys 0-36
    print('3. the phase bits, keys 0-36')
    meas = []
    for n in range(0, 37):
        fr = []
        for exe in (tgt, new):
            p = O.patch()
            x = mlab.render(exe, p, [('on', n, 100), ('run', mlab.units(3.0))])[:, 0][20000:]
            f0 = 440 * 2 ** ((n - 69) / 12)
            fr.append(O.f1_precise(x, O.f1_precise(x[:8192 * 4], f0)))
        q = fr[1] / (out['f0'] * FS); oo = int(np.floor(np.log2(q))); xx = q / 2 ** oo
        Pn = int(round(segN[sum(xx >= e for e in out['seg_e'])] / 2 ** oo))
        meas.append((n, Pn, fr[0] / fr[1]))
    scores = {}
    for b in range(16, 33):
        err = 0
        for n, Pn, r in meas:
            step = ((2 ** b - 1) // Pn) << (32 - b)
            # ticks to the restart from 2^32 t0 (t0 small; 2^20 of 2^32 as kernel.py measures it): the first k with
            # t0 + k step >= 2^32
            t0 = 1 << 20
            k = -(-(2 ** 32 - t0) // step)
            pred = Pn / k
            # the own engine's f1 (its step from the same b in osc_fit.json): ratio against an exact P
            err += abs(r - pred / OWN_RATIO(Pn)) > 2e-6
        scores[b] = err
    print('  keys predicted wrong per bit count:', scores)
    out['ph_bits'] = int(min(scores, key=lambda b: (scores[b], -b)))
    F = O.load()
    out['t0'] = F.get('clock', {}).get('t0', 2 ** -12)
    O.save('clock', out)
    print('clock:', out)


def OWN_RATIO(P):
    """the own engine's f1 against an exact P ticks a cycle (its own step, osc_fit.json's bits)"""
    b = O.load().get('clock', {}).get('ph_bits', 32)
    step = ((2 ** b - 1) // P) << (32 - b)
    k = -(-(2 ** 32 - (1 << 20)) // step)
    return P / k


if __name__ == '__main__':
    main()
