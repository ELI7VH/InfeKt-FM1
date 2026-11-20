# SPDX-License-Identifier: GPL-3.0-only
"""ph_meas.py ENGINE [OUT.json]: the oscillators' start phases at a note-on, measured as a black box. A patch of three
sines (osc 1 at the key, osc 2 seven semitones up, osc 3 an octave down), filters open, the amp envelope at once; each
note's three phases are read by a least-squares fit of three sines (frequency free) over its first 2048 frames and
taken back to the note-on's frame. Experiments (the phase hand-over's rules, cleaner/PROVENANCE.md):
  first   the first note after the patch load, at delays 0..31 blocks
  fresh   a second note while the first sounds (no voice released yet), at delays 1..96 blocks: the start phases
          of voices that take none over (their distribution; osc 1 against osc 2)
  hand    A on, A off, B on (another key) d blocks later: B's phases against A's at its release
  order   A on, B on, A off, B off, C on: whose phases C takes
  sub     the sub oscillator's start phase against osc 1's (osc 1 and the sub alone)
Prints a summary; OUT keeps the numbers."""
import sys, json
import numpy as np
import mlab

SEMI2, SEMI3 = 7, -12
FS = mlab.FS


def patch(sub=False):
    p = mlab.init_preset()
    p[mlab.OSC1_SHAPE] = 0; p[mlab.OSC2_SHAPE] = 0; p[25] = 64 + SEMI2; p[26] = 64   # osc 2: sine, +7, no detune
    p[mlab.OSC_BAL] = 64; p[mlab.CUTOFF] = 127; p[mlab.CUTOFF2] = 127; p[mlab.F1_RESO] = 0; p[mlab.F2_RESO] = 0
    p[mlab.CHORUS_MIX] = 0; p[mlab.FX_SEND] = 0; p[59] = 0; p[60] = 0; p[61] = 127; p[63] = 0   # amp env: at once
    p[mlab.PB + 41] = 4; p[mlab.PB + 42] = 100; p[mlab.PB + 43] = 64 + SEMI3; p[mlab.PB + 44] = 64   # osc 3: sine
    p[97] = 0; p[94] = 0; p[mlab.PB + 35] = 0   # no unison, poly, init phase free
    if sub:
        p[mlab.OSC_BAL] = 0; p[mlab.PB + 41] = 0; p[mlab.SUB_VOL] = 127; p[35] = 0
    return p


def hz(key):
    return 440.0 * 2 ** ((key - 69) / 12)


def fit_phases(y, t0, freqs, n=2048):
    """phases (cycles) at frame t0 of sines near freqs in y[t0 + 64 : t0 + 64 + n]"""
    seg = y[t0 + 64:t0 + 64 + n]; t = (np.arange(n) + 64) / FS
    fr = list(freqs)
    for _ in range(4):                  # (Gauss-Newton on the frequencies, linear amplitudes)
        cols = []
        for f in fr: cols += [np.sin(2 * np.pi * f * t), np.cos(2 * np.pi * f * t)]
        for f in fr: cols += [t * np.cos(2 * np.pi * f * t), -t * np.sin(2 * np.pi * f * t)]
        A = np.stack(cols[:2 * len(fr)], 1); c = np.linalg.lstsq(A, seg, rcond=None)[0]
        J = []
        for i, f in enumerate(fr):
            J.append(2 * np.pi * t * (c[2 * i] * np.cos(2 * np.pi * f * t) - c[2 * i + 1] * np.sin(2 * np.pi * f * t)))
        r = seg - A @ c
        d = np.linalg.lstsq(np.stack(J, 1), r, rcond=None)[0]
        fr = [f + x for f, x in zip(fr, d)]
    cols = []
    for f in fr: cols += [np.sin(2 * np.pi * f * t), np.cos(2 * np.pi * f * t)]
    c = np.linalg.lstsq(np.stack(cols, 1), seg, rcond=None)[0]
    return [float((np.arctan2(c[2 * i + 1], c[2 * i]) / (2 * np.pi)) % 1.0) for i in range(len(fr))], fr


def onset(y, frm):
    """the first frame from frm where the signal leaves the silence"""
    i = np.flatnonzero(np.abs(y[frm:]) > 1e-4)
    return frm + int(i[0]) if len(i) else frm


def osc_fr(key):
    return (hz(key), hz(key) * 2 ** (SEMI2 / 12), hz(key) * 2 ** (SEMI3 / 12))


def cdiff(a, b):
    return (np.asarray(a) - np.asarray(b) + 0.5) % 1.0 - 0.5


def run(exe):
    P = patch()
    R = {}
    # first: the first note after the load
    jobs = [(P, [('run', d), ('on', 57, 100), ('run', 40)]) for d in range(32)]
    res = mlab.render_many(exe, jobs)
    R['first'] = [fit_phases(a[:, 0], d * 128, osc_fr(57))[0] for d, a in zip(range(32), res)]
    # fresh: a second note (key 64) while key 45 sounds; its phases from the difference to a render without it
    jobs, ks = [], []
    for d in range(1, 97):
        jobs.append((P, [('on', 45, 100), ('run', d), ('on', 64, 100), ('run', 40)]))
        jobs.append((P, [('on', 45, 100), ('run', d + 40)]))
        ks.append(d)
    res = mlab.render_many(exe, jobs)
    R['fresh'] = [fit_phases(res[2 * i][:, 0] - res[2 * i + 1][:, 0], d * 128, osc_fr(64))[0] for i, d in enumerate(ks)]
    # hand: A (45) on, off after a blocks, B (52) on b blocks later: B's phases against A's at the release (A's own
    # phases at its start and frequency taken forward)
    jobs, ks = [], []
    for a in (8, 13, 21):
        for b in (1, 5, 30):
            jobs.append((P, [('on', 45, 100), ('run', a), ('off', 45), ('run', b), ('on', 52, 100), ('run', 40)]))
            ks.append((a, b))
    res = mlab.render_many(exe, jobs)
    R['hand'] = []
    for (a, b), y in zip(ks, res):
        pa, fa = fit_phases(y[:, 0], 0, osc_fr(45), n=a * 128 - 128)
        rel = [(p + f * a * 128 / FS) % 1.0 for p, f in zip(pa, fa)]          # A's phases at its release
        pb, _ = fit_phases(y[:, 0] - 0, (a + b) * 128, osc_fr(52)) if False else (None, None)
        # (B overlaps A's release tail: B's phases from the difference to A alone)
        R['hand'].append({'a': a, 'b': b, 'rel': rel})
    jobs = [(P, [('on', 45, 100), ('run', a), ('off', 45), ('run', b + 40)]) for a, b in ks]
    res2 = mlab.render_many(exe, jobs)
    for q, y, y2 in zip(R['hand'], res, res2):
        pb, _ = fit_phases(y[:, 0] - y2[:, 0], (q['a'] + q['b']) * 128, osc_fr(52))
        q['b_ph'] = pb
        q['diff'] = cdiff(pb, q['rel']).tolist()
    # order: A (45) on, B (50) on, A off, B off, C (57) on: C's phases against A's and B's at their releases
    seq = [('on', 45, 100), ('run', 6), ('on', 50, 100), ('run', 7), ('off', 45), ('run', 5), ('off', 50), ('run', 4)]
    y = mlab.render(exe, P, seq + [('on', 57, 100), ('run', 40)])
    y0 = mlab.render(exe, P, seq + [('run', 40)])
    ya = mlab.render(exe, P, [('on', 45, 100), ('run', 13 + 5 + 4 + 40)])
    yb = mlab.render(exe, P, [('run', 6), ('on', 50, 100), ('run', 7 + 5 + 4 + 40)])
    pa, fa = fit_phases(ya[:, 0], 0, osc_fr(45)); pb, fb = fit_phases(yb[:, 0], 6 * 128, osc_fr(50))
    ra = [(p + f * 13 * 128 / FS) % 1 for p, f in zip(pa, fa)]; rb = [(p + f * 12 * 128 / FS) % 1 for p, f in zip(pb, fb)]
    pc, _ = fit_phases(y[:, 0] - y0[:, 0], 22 * 128, osc_fr(57))
    R['order'] = {'c_vs_a': cdiff(pc, ra).tolist(), 'c_vs_b': cdiff(pc, rb).tolist()}
    # sub: osc 1 and the sub (byte 35 shape 0: square), fresh voices: the sub's square edge against osc 1's phase
    S = patch(sub=True)
    jobs = [(S, [('on', 45, 100), ('run', d), ('on', 57, 100), ('run', 40)]) for d in range(1, 25)]
    jobs0 = [(S, [('on', 45, 100), ('run', d + 40)]) for d in range(1, 25)]
    r1, r0 = mlab.render_many(exe, jobs), mlab.render_many(exe, jobs0)
    R['sub'] = []
    for d, y, y0 in zip(range(1, 25), r1, r0):
        z = y[:, 0] - y0[:, 0]
        ph, fr = fit_phases(z, d * 128, (hz(57), hz(57) / 2))
        R['sub'].append(float(cdiff(ph[1], ph[0] / 2)))   # (the sub's fundamental against half osc 1's phase)
    return R


if __name__ == '__main__':
    R = run(sys.argv[1])
    if len(sys.argv) > 2: json.dump(R, open(sys.argv[2], 'w'))
    f = np.array(R['first']); print('first note: phases', f[:4].round(3).tolist(), 'spread', np.std(f, 0).round(4).tolist())
    fr = np.array(R['fresh'])
    print('fresh: n %d, mean |e^i2pi ph| %s (uniform: ~%.2f), corr osc1-osc2 %.2f' % (len(fr), np.abs(np.mean(np.exp(2j * np.pi * fr), 0)).round(3).tolist(), 1 / np.sqrt(len(fr)), np.corrcoef(fr[:, 0], fr[:, 1])[0, 1]))
    for q in R['hand']: print('hand a %2d b %2d  B - A at release %s' % (q['a'], q['b'], np.round(q['diff'], 3).tolist()))
    print('order: C - A %s  C - B %s' % (np.round(R['order']['c_vs_a'], 3).tolist(), np.round(R['order']['c_vs_b'], 3).tolist()))
    print('sub - osc1/2: %s' % np.round(R['sub'], 3).tolist())
