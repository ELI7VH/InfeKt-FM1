# SPDX-License-Identifier: GPL-3.0-only
"""pedal_meas.py [ENGINE]: the sustain pedal (CC 64) rendered on ENGINE as a black box (default: the reference,
the repository's tools/*/ref_engine.sh, or MLAB_REFENG), the lab's init patch (a plain saw) and a few changes of it. Each case
is a list of MIDI events; what is read from the audio is only whether a note sounds and at what pitch, in windows of
blocks (128 frames). The same script on the cleaner's host engine (cleaner/build/engine) shows the engine's pedal
against the reference's (PROVENANCE.md, "Sustain pedal"). Cases:
    on    the pedal's threshold pressing: CC 64 = v (0..127) after the key, the key released: held or not
    off   the threshold releasing: held by CC 64 = 127, then CC 64 = w (0..127): released or not
    hyst  held by v, then a lower w (the pairs around the thresholds)
    late  the pedal pressed after the key's release (the note already in its release)
    multi two notes under the pedal: both end at pedal up
    re    a key re-struck while its note is held: a second voice or the same one (its level and harmonics while the
          pedal holds, against after pedal up with the key still down)
    mono  key mode mono (1): which note sounds after keys are released under the pedal, and after pedal up
    arp   the patch's arpeggiator (up, 1/16): its keys released under the pedal, does it keep playing
    uni   unison 3 (4 voices): held and released as a voice
    mtx   CC 64 as a matrix source (slot 1 -> osc 1 semitone, amount 72) while it holds the note
    ano   CC 123 (all notes off), CC 120 (all sound off), CC 121 (reset controllers) with notes held by the pedal
Prints a line per case: the levels (dB, per window) and the pitch where it matters."""
import glob, os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mlab

ENG = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1] else os.environ.get('MLAB_REFENG') or sorted(glob.glob(os.path.join(mlab.ROOT, 'tools', '*', 'ref_engine.sh')))[0]
ONLY = sys.argv[2].split(',') if len(sys.argv) > 2 else None
B = mlab.BLOCK
SIL = -90.0                              # below: silent
KEY_MODE, UNISON, OSC_INIT_PHASE = 94, 97, mlab.PB + 35
ARP_MODE, ARP_PAT, ARP_OCT, ARP_HOLD, ARP_CLOCK = mlab.PB + 1, mlab.PB + 2, mlab.PB + 3, mlab.PB + 4, mlab.PB + 17
MX_SRC, MX_DST, MX_AMT = mlab.PB + 64, mlab.PB + 65, mlab.PB + 66
SRC_CC64, DST_OSC1_SEMI = 16, 9         # (sources 3-18 CC 1, 2, 3, 4, 6, 8, 9, 11, 12, 13, 14, 15,
                                         # 16, 64, 65, 66; destination 9 osc 1 semitone)


def ped(v):
    return ('midi', 176, 64, v)


def lev(a, b0, b1):
    """the level (dB RMS, left) of blocks b0..b1"""
    x = a[b0 * B:b1 * B, 0]
    return 20 * np.log10(np.sqrt(np.mean(x ** 2)) + 1e-12) if len(x) else -200.0


def note_of(a, b0, b1):
    """the note sounding in blocks b0..b1 (the autocorrelation's fundamental, as a MIDI note number, rounded to 0.1);
    None if silent"""
    if b1 * B > len(a) or lev(a, b0, b1) < SIL:
        return None
    x = a[b0 * B:b1 * B, 0]
    x = x - x.mean()
    n = len(x)
    c = np.fft.irfft(np.abs(np.fft.rfft(x, 2 * n)) ** 2)[:n]
    lo, hi = int(mlab.FS / 2000), int(mlab.FS / 40)
    k = lo + int(np.argmax(c[lo:hi]))
    if 0 < k < n - 1:
        y0, y1, y2 = c[k - 1], c[k], c[k + 1]
        k = k + 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2)
    return round(69 + 12 * np.log2(mlab.FS / k / 440.0), 1)


def harm(a, b0, b1, f, n=8):
    """the first n harmonics' levels (dB) of a note at f Hz in blocks b0..b1"""
    x = a[b0 * B:b1 * B, 0] * np.hanning((b1 - b0) * B)
    s = np.abs(np.fft.rfft(x, 1 << 16))
    return np.array([20 * np.log10(s[int(round(k * f * (1 << 16) / mlab.FS))] + 1e-12) for k in range(1, n + 1)])


def run(jobs):
    return mlab.render_many(ENG, jobs, workers=8)


def want(c):
    return ONLY is None or c in ONLY


def sounding(a, t):
    return lev(a, t, t + 8) > SIL


P0 = mlab.init_preset()
# ---- on: CC 64 = v 20 blocks after the key, the key off 20 later; sounding 100 blocks after the key's release?
# (the init patch's release ends within ~30 blocks: case 'late')
if want('on') or want('off'):
    V = list(range(128))
    on = run([(P0, [('on', 60, 100), ('run', 20), ped(v), ('run', 20), ('off', 60), ('run', 120)]) for v in V])
    held_on = [v for v, a in zip(V, on) if sounding(a, 150)]
    print('on   held by CC 64 =', (min(held_on), '..', max(held_on)) if held_on else 'none',
          ' released (not held):', [v for v in V if v not in held_on][:8], '...' if len(V) - len(held_on) > 8 else '')
    off = run([(P0, [('on', 60, 100), ('run', 20), ped(127), ('run', 20), ('off', 60), ('run', 40), ped(w), ('run', 120)])
               for w in V])
    rel_off = [w for w, a in zip(V, off) if not sounding(a, 190)]
    print('off  held by 127, then CC 64 = w: released for w =', (min(rel_off), '..', max(rel_off)) if rel_off else 'none',
          ' still held for', [w for w, a in zip(V, off) if sounding(a, 190)][:8])
if want('hyst'):
    pairs = [(127, 62), (127, 63), (127, 64), (64, 63), (63, 62), (63, 64), (70, 60), (2, 1), (1, 0)]
    r = run([(P0, [('on', 60, 100), ('run', 20), ped(v), ('run', 20), ('off', 60), ('run', 40), ped(w), ('run', 120)])
             for v, w in pairs])
    print('hyst', '  '.join('%d->%d: %s' % (v, w, 'held' if sounding(a, 190) else 'released') for (v, w), a in zip(pairs, r)))
if want('late'):
    r = run([(P0, [('on', 60, 100), ('run', 40), ('off', 60), ('run', 120)]),
             (P0, [('on', 60, 100), ('run', 40), ('off', 60), ('run', 4), ped(127), ('run', 120)]),
             (P0, [('on', 60, 100), ('run', 40), ('off', 60), ('run', 40), ped(127), ('run', 100)])])
    def end(a):
        k = 40
        while k < len(a) // B - 1 and lev(a, k, k + 1) > SIL: k += 1
        return k - 40
    print('late no pedal: silent %d blocks after the key\'s release; pedal 4 blocks after it: %d; 40 after: %s' %
          (end(r[0]), end(r[1]), 'silent' if not sounding(r[2], 100) else 'SOUNDING'))
if want('multi'):
    a = run([(P0, [ped(127), ('run', 4), ('on', 60, 100), ('run', 30), ('off', 60), ('run', 20), ('on', 67, 100), ('run', 30),
                   ('off', 67), ('run', 60), ped(0), ('run', 80)])])[0]
    print('multi before pedal up: notes %s %s, level %.1f; after pedal up (40 blocks): %s' %
          (note_of(a, 20, 34), note_of(a, 110, 140), lev(a, 110, 140), 'silent' if not sounding(a, 190) else 'SOUNDING'))
if want('re'):
    # re-struck under the pedal, the key kept down; pedal up at 120; the key up at 200
    for name, p in (('free phase', P0), ('phase reset', None)):
        if p is None:
            p = mlab.init_preset(); p[OSC_INIT_PHASE] = 1
        ev = [ped(127), ('run', 4), ('on', 60, 100), ('run', 40), ('off', 60), ('run', 30), ('on', 60, 100), ('run', 46),
              ped(0), ('run', 80), ('off', 60), ('run', 60)]
        single = run([(p, [('on', 60, 100), ('run', 300)])])[0]
        a = run([(p, ev)])[0]
        f = 440 * 2 ** ((60 - 69) / 12)
        h1, h2, hs = harm(a, 90, 120, f), harm(a, 140, 200, f), harm(single, 140, 200, f)
        print('re   %s: single voice %.1f dB; re-struck, pedal down %.1f dB; pedal up, key down %.1f dB; key up %s;'
              ' harmonics changed at pedal up by %.1f dB (max), after pedal up vs a single voice %.1f dB' %
              (name, lev(single, 90, 120), lev(a, 90, 120), lev(a, 140, 200), 'silent' if not sounding(a, 250) else 'SOUNDING',
               np.max(np.abs(h1 - h2)), np.max(np.abs(h2 - hs))))
    # re-struck and released under the pedal: pedal up -> silent?
    a = run([(P0, [ped(127), ('run', 4), ('on', 60, 100), ('run', 40), ('off', 60), ('run', 30), ('on', 60, 100), ('run', 30),
                   ('off', 60), ('run', 30), ped(0), ('run', 80)])])[0]
    print('re   re-struck and released under the pedal: %s before pedal up, %s 40 blocks after' %
          ('sounding' if sounding(a, 125) else 'silent', 'SOUNDING' if sounding(a, 180) else 'silent'))
if want('mono'):
    pm = mlab.init_preset(); pm[KEY_MODE] = 1
    cases = [
        ('60 dn, 64 dn, 64 up, 60 up, pedal up', [ped(127), ('run', 4), ('on', 60, 100), ('run', 30), ('on', 64, 100), ('run', 30),
                                                    ('off', 64), ('run', 30), ('off', 60), ('run', 30), ped(0), ('run', 60)],
         [(20, 30), (50, 60), (80, 90), (110, 120), (160, 180)]),
        ('60 dn, 60 up, 64 dn, 64 up, pedal up', [ped(127), ('run', 4), ('on', 60, 100), ('run', 30), ('off', 60), ('run', 30),
                                                    ('on', 64, 100), ('run', 30), ('off', 64), ('run', 30), ped(0), ('run', 60)],
         [(20, 30), (50, 60), (80, 90), (110, 120), (160, 180)]),
        ('60 dn, 64 dn, 64 up (60 down), pedal up, 60 up', [ped(127), ('run', 4), ('on', 60, 100), ('run', 30), ('on', 64, 100),
                                                           ('run', 30), ('off', 64), ('run', 30), ped(0), ('run', 30),
                                                           ('off', 60), ('run', 60)],
         [(20, 30), (50, 60), (80, 90), (110, 120), (160, 180)]),
        ('no pedal: 60 dn, 64 dn, 64 up, 60 up', [('on', 60, 100), ('run', 34), ('on', 64, 100), ('run', 30), ('off', 64), ('run', 30),
                                                 ('off', 60), ('run', 90)],
         [(20, 30), (50, 60), (80, 90), (110, 120), (160, 180)]),
    ]
    r = run([(pm, ev) for _, ev, _ in cases])
    for (name, _, w), a in zip(cases, r):
        print('mono %-42s notes:' % name, ' '.join(str(note_of(a, b0, b1)) for b0, b1 in w))
if want('arp'):
    pa = mlab.init_preset(); pa[ARP_MODE] = 1; pa[ARP_CLOCK] = 9      # (up; the clock a sixteenth or so)
    cases = [('no pedal: 60+64 held 100 blocks', [('on', 60, 100), ('on', 64, 100), ('run', 100), ('off', 60), ('off', 64), ('run', 200)]),
             ('pedal: 60+64 for 100 blocks, pedal up at 300', [ped(127), ('run', 2), ('on', 60, 100), ('on', 64, 100), ('run', 100),
                                                              ('off', 60), ('off', 64), ('run', 198), ped(0), ('run', 100)])]
    r = run([(pa, ev) for _, ev in cases])
    for (name, _), a in zip(cases, r):
        print('arp  %-46s' % name, 'notes per 20 blocks:', ' '.join(str(note_of(a, k, k + 6)) for k in range(0, 400, 20)))
if want('uni'):
    pu = mlab.init_preset(); pu[UNISON] = 3
    a = run([(pu, [ped(127), ('run', 4), ('on', 60, 100), ('run', 30), ('off', 60), ('run', 60), ped(0), ('run', 80)]),
             (pu, [('on', 60, 100), ('run', 34), ('off', 60), ('run', 140)])])
    print('uni  pedal: %s 60 blocks after the key\'s release, %s 40 after pedal up; no pedal: %s 60 after the release' %
          ('held' if sounding(a[0], 85) else 'released', 'SOUNDING' if sounding(a[0], 140) else 'silent',
           'SOUNDING' if sounding(a[1], 95) else 'silent'))
if want('mtx'):
    px = mlab.init_preset(); px[MX_SRC] = SRC_CC64; px[MX_DST] = DST_OSC1_SEMI; px[MX_AMT] = 72
    a = run([(px, [('on', 60, 100), ('run', 30), ped(127), ('run', 30), ('off', 60), ('run', 40), ped(64), ('run', 40), ped(0), ('run', 60)]),
             (px, [('on', 60, 100), ('run', 30), ped(63), ('run', 30), ('off', 60), ('run', 40), ped(0), ('run', 60)])])
    print('mtx  CC 64 -> osc 1 semitone (amount 72): before %s, pedal 127 %s, key up %s, pedal 64 %s, pedal 0 %s' %
          (note_of(a[0], 10, 30), note_of(a[0], 40, 60), note_of(a[0], 80, 100), note_of(a[0], 115, 140), 'silent' if not sounding(a[0], 170) else note_of(a[0], 170, 190)))
    print('mtx  pedal 63: before %s, pedal 63 %s, key up %s, pedal 0 %s' %
          (note_of(a[1], 10, 30), note_of(a[1], 40, 60), note_of(a[1], 80, 100), 'silent' if not sounding(a[1], 130) else 'SOUNDING'))
if want('ano'):
    r = run([(P0, [ped(127), ('run', 4), ('on', 60, 100), ('run', 30), ('off', 60), ('run', 30), ('midi', 176, 123, 0), ('run', 100)]),
             (P0, [ped(127), ('run', 4), ('on', 60, 100), ('run', 30), ('off', 60), ('run', 30), ('midi', 176, 120, 0), ('run', 100)]),
             (P0, [ped(127), ('run', 4), ('on', 60, 100), ('run', 30), ('midi', 176, 123, 0), ('run', 100)]),
             (P0, [('on', 60, 100), ('run', 34), ('midi', 176, 120, 0), ('run', 130)]),
             (P0, [ped(127), ('run', 4), ('on', 60, 100), ('run', 30), ('off', 60), ('run', 30), ('midi', 176, 121, 0), ('run', 100)])])
    print('ano  held by the pedal, CC 123: %s 60 blocks later; CC 120: %s; the key still down (pedal down), CC 123: %s;'
          ' no pedal, the key down, CC 120: %s; held by the pedal, CC 121: %s' %
          tuple('SOUNDING' if sounding(a, 130) else 'silent' for a in r))
    r = run([(P0, [ped(127), ('run', 4), ('on', 60, 100), ('run', 30), ('off', 60), ('run', 30), ('midi', 176, k, 0), ('run', 20),
                   ('on', 64, 100), ('run', 30), ('off', 64), ('run', 60)]) for k in (123, 121)])
    print('ano  the pedal still down after CC 123: a new note %s 50 blocks after its key\'s release; after CC 121: %s' %
          tuple('held' if sounding(a, 165) else 'released' for a in r))
