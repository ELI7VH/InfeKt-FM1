# SPDX-License-Identifier: GPL-3.0-only
"""lfo_meas.py: the free LFOs' start phases, measured on the development engine as a black box and set in the cleaner:
  LFO1_PH0, LFO2_PH0  the part's LFO 1 and 2 phase at the start (a first note alone takes it, run on by the rate)
  CLN_LFO_A, _D      a voice started while others sound: its free LFO 1 and 2 start at A + D n (n: the voice starts
                     since the load, a cycle of 16)
The method: LFO 2 (or 1) moves the pan of a sine voice; the voice's pan curve (its band's level left against right)
is compared with the cleaner's, built with known phases, at every phase shift (cross-correlation over one LFO
cycle): the development engine's phase is the shift that fits best. Then A and D (a straight line through the 16 phases, mod 1)
and the start phases, refined on the null of the whole sequence. Prints the -D flags and the fit's residuals."""
import sys
import numpy as np
import mlab, cbuild

KEY_HELD, KEY_T = 36, 82                # (no harmonic of the held key near the test key)
FT = 440.0 * 2 ** ((KEY_T - 69) / 12)
NV, LEN, GAP = 18, 48, 4


def patch(lfo):
    p = mlab.init_preset()
    p[mlab.OSC1_SHAPE] = 0; p[mlab.OSC_BAL] = 0; p[mlab.CUTOFF] = 127; p[mlab.CUTOFF2] = 127
    p[mlab.CHORUS_MIX] = 0; p[mlab.FX_SEND] = 0; p[59] = 0; p[60] = 0; p[61] = 127; p[63] = 0; p[97] = 0; p[94] = 0
    for b in (69, 73, 81, 85): p[b] = 0                       # (no envelope mode, no key trigger)
    p[68] = 0; p[80] = 0; p[70] = 0; p[82] = 0                # sine, poly
    p[mlab.PB + 79] = 0; p[mlab.PB + 80] = 0                  # (no clock)
    p[67] = 90; p[79] = 90
    if lfo == 2: p[90] = 127
    else: p[90] = 64; p[74] = 100; p[72] = 0                  # (LFO 1: osc 1's pitch)
    return p


def seq():
    e = [('on', KEY_HELD, 100), ('run', 6)]
    for n in range(NV):
        e += [('on', KEY_T, 100), ('run', LEN), ('off', KEY_T), ('run', GAP)]
    return e


CURVE = 'pan'


def pan_curve(y, f=FT):
    """LFO 2 (CURVE 'pan'): the test key's band, left against right; LFO 1 ('pitch'): its instantaneous frequency"""
    n = np.arange(len(y)); c = np.exp(-2j * np.pi * f * n / mlab.FS)
    w = np.hanning(801); w /= w.sum()            # (a smooth low pass: the held voice's line well below it)
    zl = np.convolve(y[:, 0] * c, w, 'same')
    if CURVE == 'pitch':
        return np.gradient(np.unwrap(np.angle(zl + 1e-12)))
    L = np.abs(zl); R = np.abs(np.convolve(y[:, 1] * c, w, 'same'))
    return (L - R) / (L + R + 1e-9)


def voice_win(n):
    s = (6 + n * (LEN + GAP)) * 128
    return s + 512, s + LEN * 128 - 256


def phases(yh, yr, rate_hz):
    """per test voice: the shift (cycles) of the development engine's pan curve against yr: the cleaner's one long added voice at
    the internal phase 0 (its curve as a function of the time since its start, over two LFO cycles)"""
    ph, ch = [], []
    ph_h, ref = pan_curve(yh), pan_curve(yr)
    s0 = 6 * 128
    per = mlab.FS / rate_hz
    for n in range(NV):
        a, b = voice_win(n)
        s = (6 + n * (LEN + GAP)) * 128
        tau = np.arange(a, b) - s
        best = None
        for sh in np.linspace(0, 1, 3601)[:-1]:
            tt = s0 + (tau + sh * per) % per + 1024     # (the reference past its first 1024 frames: the filter's edge)
            tt = np.where(tt - s0 < (b - s), tt, tt)
            cand = np.interp(tt, np.arange(len(ref)), ref)
            e = np.mean((cand - ph_h[a:b]) ** 2)
            if best is None or e < best[0]: best = (e, sh)
        ph.append((best[1] + 1024 / per) % 1.0); ch.append(best[0])
    return np.array(ph), np.array(ch)


def first_shift(eng_h, eng_c, P, rate):
    """a first note alone (the part's LFO phase, run on from the start): the development engine's shift against the cleaner's"""
    ev = [('run', 6), ('on', KEY_T, 100), ('run', 300)]
    ch, cc = pan_curve(mlab.render(eng_h, P, ev)), pan_curve(mlab.render(eng_c, P, ev))
    per = mlab.FS / rate; a = 6 * 128 + 1024; b = a + int(per)
    best = None
    for sh in np.linspace(0, 1, 3601)[:-1]:
        cand = np.interp(np.arange(a, b) + sh * per, np.arange(len(cc)), cc)
        e = np.mean((cand - ch[a:b]) ** 2)
        if best is None or e < best[0]: best = (e, sh)
    return best[1], best[0]


if __name__ == '__main__':
    lfo = int(sys.argv[1]) if len(sys.argv) > 1 else 2
    CURVE = 'pan' if lfo == 2 else 'pitch'
    P = patch(lfo)
    yh = mlab.render(mlab.HYB, P, seq())
    # the cleaner with every added voice's LFO at phase 0 (CLN_LFO_ZERO): the LFO's rate from one long voice
    e0 = cbuild.build('lfo0', ['-DCLN_LFO_ZERO'])
    yr = mlab.render(e0, P, [('on', KEY_HELD, 100), ('run', 6), ('on', KEY_T, 100), ('run', 300)])
    pc = pan_curve(yr)[6 * 128 + 1024:6 * 128 + 1024 + 30000]
    z = pc - np.mean(pc); sp = np.abs(np.fft.rfft(z * np.hanning(len(z)), 1 << 20)); fr = np.fft.rfftfreq(1 << 20, 1 / mlab.FS)
    rate = fr[np.argmax(sp[1:]) + 1]
    ph, err = phases(yh, yr, rate)
    print('LFO %d rate %.3f Hz; per added voice (the n-th voice start, n = 2..): its start phase, the fit\'s error' % (lfo, rate))
    for n in range(NV): print(n + 2, '%.4f  %.2e' % (ph[n], err[n]))
    tab = [0.0] * 16
    for n in range(16): tab[n & 15] = ph[n]                # (test voice n: entry n, by the null check below)
    print('CLN_LFO_PH (x 65536):', ', '.join('%du' % int(round(v * 65536) % 65536) for v in tab))
    sh, e = first_shift(mlab.HYB, mlab.CLN, P, rate)
    print('first note alone: the development engine ahead of the cleaner (as built) by %.4f cycles (error %.1e): LFO%d_PH0 + that' % (sh, e, lfo))
