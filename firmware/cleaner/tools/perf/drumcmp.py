# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
# drumcmp.py OLD_HOST NEW_HOST [OUTDIR] [--wav]: the drums of two host simulators (tools/perf/host/mk.sh mble; "drwav":
# the drums alone, the synth silent) compared per instrument (1.18C42): each instrument alone (four
# hits, the three accents), at both MODELs where it has two, TUNE and TONE at their ends, PAN, ATTACK; a beat. Per case:
# the level (RMS and A-weighted), the bands' levels, the envelope (20 ms RMS, where the old one is within 40 dB of its
# peak), each hit's onset (the first frame over 10 % of the hit's peak). Exit code 1 if a case is outside its limits
# (LIM: the tonal voices as good as unchanged; the noise voices' A-weighted level and bands below 8 kHz within a few dB,
# their top above ~10 kHz not checked: the half rate has none; a noise's envelope and onset only roughly: the noise
# is not the same; no onset with an ATTACK's fade-in). Usually OLD built with -DDR_HALF=0 (the drums as
# 1.18C41's), NEW as shipped: tools/perf/drumtest.sh
import os, subprocess, sys, tempfile, wave
import numpy as np
NI = ['BD', 'SD', 'LT', 'MT', 'HT', 'RS', 'CP', 'CH', 'OH', 'CY', 'CB']
TONAL = {'BD', 'LT', 'MT', 'HT'}
BPS = 44100 / 128.
B = [(20, 200), (200, 1000), (1000, 4000), (4000, 8000), (8000, 11000), (11000, 16000), (16000, 22050)]
#      A-weighted dB, bands below 8 kHz dB, envelope dB, onset frames
LIM = {'tonal': (0.3, 0.6, 1.0, 6), 'noise': (2.5, 6.0, 7.0, 12), 'CB': (0.3, 1.0, 1.0, 6), 'RS': (0.5, 4.0, 2.0, 6),
       'beat': (1.0, 2.0, None, None)}
def sb(acc, ln=4): return acc | ln << 2
def cases():
    C = {}
    hits = lambda i: [(int((0.25 + 0.6 * k) * BPS), i, sb(k % 3)) for k in range(4)]
    for i, n in enumerate(NI):
        C['solo_' + n] = (hits(i), int(3.5 * BPS), '100,0', [], n)
        if i <= 4: C['m909_' + n] = (hits(i), int(3.5 * BPS), '100,0', ['km %d 1' % i, 'km %d 0' % i], n)
        for f in ('tune', 'tone'):
            for t in (-12, 12):
                C['%s_%s%+d' % (n, f, t)] = (hits(i), int(3.5 * BPS), '100,0', ['kitset %d %s %d' % (i, f, t), 'kitset %d %s 0' % (i, f)], n)
        C['pan_' + n] = (hits(i), int(3.5 * BPS), '100,0', ['kitset %d pan 40' % i, 'kitset %d pan 0' % i], n)
        C['att_' + n] = (hits(i), int(3.5 * BPS), '100,0', ['kitset %d att 10' % i, 'kitset %d att 0' % i], n)
    ev = []; s = 0; t = 0.25
    while s < 64:
        b = s % 16; k = int(t * BPS)
        if b % 4 == 0: ev.append((k, 0, sb(2)))
        if b in (4, 12): ev += [(k, 1, sb(2)), (k, 6, sb(1))]
        if b % 2 == 0 and b != 14: ev.append((k, 7, sb(1)))
        if b == 14: ev.append((k, 8, sb(1)))
        if b == 0 and (s // 16) % 2 == 0: ev.append((k, 9, sb(1)))
        if (s // 16) % 2 == 1 and b in (13, 15): ev.append((k, 2 if b == 15 else 3, sb(1)))
        s += 1; t += 0.125
    C['beat'] = (ev, int((t + 2) * BPS), '100,0', [], 'beat')
    return C
def render(host, out, C):
    os.makedirs(out, exist_ok=True)
    lines = ['wait 1500']
    for c, (ev, nb, lv, pre, _) in sorted(C.items()):
        open(os.path.join(out, c + '.ev'), 'w').write(''.join('%d %d %d\n' % e for e in ev))
        lines += pre[:1] + ['drwav %s.wav %s.ev %d %s' % (c, c, nb, lv)] + pre[1:]
    open(os.path.join(out, 'r.in'), 'w').write('\n'.join(lines) + '\n')
    r = subprocess.run([os.path.abspath(host), 'r.in', 'o'], cwd=out, capture_output=True, text=True)
    if r.returncode or 'FAIL' in r.stdout: sys.exit('render failed: ' + r.stdout[-1000:] + r.stderr[-1000:])
def rd(p):
    w = wave.open(p)
    b = np.frombuffer(w.readframes(w.getnframes()), np.uint8).reshape(-1, 3).astype(np.int32)
    x = b[:, 0] | b[:, 1] << 8 | b[:, 2] << 16
    return (np.where(x >= 1 << 23, x - (1 << 24), x) / 8388608.).reshape(-1, 2), w.getframerate()
def db(x): return 10 * np.log10(x + 1e-20)
def AW(f): return (12194. ** 2 * f ** 4) / ((f ** 2 + 20.6 ** 2) * np.sqrt((f ** 2 + 107.7 ** 2) * (f ** 2 + 737.9 ** 2)) * (f ** 2 + 12194. ** 2))
def spec(x, fs):
    X = np.abs(np.fft.rfft(x.mean(1))) ** 2 / len(x) ** 2 * 2; return X, np.fft.rfftfreq(len(x), 1 / fs)
def measure(x, fs, hits):
    X, f = spec(x, fs)
    m = x.mean(1); n = int(fs * 0.02); k = len(m) // n
    on = []
    for h in hits:
        s0 = int(h * 128); seg = np.abs(m[s0:s0 + int(0.05 * fs)])
        on.append(int(np.argmax(seg > 0.1 * seg.max())) if seg.max() > 0 else 0)
    return dict(rms=db((x ** 2).mean()), aw=db((X * AW(np.maximum(f, 1)) ** 2).sum()),
                bands=[db(X[(f >= a) & (f < b)].sum()) for a, b in B], env=db((m[:k * n].reshape(k, n) ** 2).mean(1)), on=on)
def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    old, new = a[0], a[1]; out = a[2] if len(a) > 2 else tempfile.mkdtemp()
    C = cases()
    render(old, os.path.join(out, 'old'), C); render(new, os.path.join(out, 'new'), C)
    bad = 0
    print('%-14s %7s %7s %6s | %s | %5s | %s' % ('case', 'old dB', 'new-old', 'A', ' '.join('%6s' % ('%dk' % (x // 1000) if x >= 1000 else x) for x, _ in B), 'env', 'onset'))
    for c, (ev, nb, lv, pre, kind) in sorted(C.items()):
        xa, fs = rd(os.path.join(out, 'old', c + '.wav')); xb, _ = rd(os.path.join(out, 'new', c + '.wav'))
        p, q = measure(xa, fs, [e[0] for e in ev] if kind != 'beat' else []), measure(xb, fs, [e[0] for e in ev] if kind != 'beat' else [])
        lim = LIM['beat' if kind == 'beat' else kind if kind in ('CB', 'RS') else 'tonal' if kind in TONAL else 'noise']
        da = q['aw'] - p['aw']
        dband = [y - x for x, y, (lo, hi) in zip(p['bands'], q['bands'], B) if hi <= 8000 and x > max(p['bands']) - 30]
        sel = p['env'] > p['env'].max() - 40; denv = np.abs(p['env'][sel] - q['env'][sel]).max() if sel.any() else 0.
        don = [y - x for x, y in zip(p['on'], q['on'])]
        f = abs(da) > lim[0] or (dband and max(abs(x) for x in dband) > lim[1]) or (lim[2] is not None and denv > lim[2]) \
            or (lim[3] is not None and not c.startswith('att_') and don and max(abs(x) for x in don) > lim[3])
        bad |= bool(f)
        print('%-14s %7.1f %+7.2f %+6.2f | %s | %5.2f | %s%s' % (c, p['rms'], q['rms'] - p['rms'], da,
              ' '.join('%+6.1f' % (y - x) if x > -110 else '   ---' for x, y in zip(p['bands'], q['bands'])), denv, don,
              '  FAIL' if f else ''))
    print('drumcmp: %s' % ('FAIL' if bad else 'ok'))
    sys.exit(1 if bad else 0)
main()
