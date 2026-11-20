#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""shimtest.py DIR: the shimmer's MODULATION and level checks (1.18C45) on the lab renderers that
`make shimtest` builds into DIR (two parts, the lent float ring: the Bluetooth-off firmware's shimmer):
  e_new  the engine; e_l0  -DRVS_LEVEL=0 (TAIL LIMITED as 1.18C44); e_dry  -DCLN_RVS_LOG -DCLN_RV_TUNE -DRVS_TEST_DRY
  (the shifters' own output, SY_SHCAP, fed the dry input; the grains' random delays off: CLN_RVS_TUNE 9=0).
1. pitch: a sine (A3) held, the octave shifter's instantaneous frequency: MODULATION 0 none; 32 / 64 / 96 / 127 a
   triangle of +-50 x (v / 127)^2 cents (within 10 % + 0.5 c) at the reverb LFO's rate (1.0-1.3 Hz); LFO 3 on SHIM MOD
   (SY_SHRT 11=2, fast triangle): the deviation at LFO 3's rate, not the reverb's.
2. MODULATION 0 = the same render as without the control (e_new: AMOUNT 64 with and without a target set to 0).
3. level: a held chord into the shimmer (AMOUNT 64 / 110 / 127, TAIL LIMITED): the shimmer part's level (render minus
   the same at AMOUNT 0) in 0.5 s steps, detrended over 4.5 s: its peak to peak at most TAIL FREE's + 1.0 dB and at
   most 3.5 dB (e_new); e_l0 shows the pumping this replaced (reported). MODULATION 127 at AMOUNT 127: bounded (no growth).
Exit 1 on a failed check."""
import os, subprocess, sys, tempfile, shutil
import numpy as np
from pathlib import Path
from scipy.signal import hilbert, butter, sosfiltfilt
sys.path.insert(0, str(Path(__file__).resolve().parent))
import hostcases  # noqa: E402

FS = 46875.
FH = FS / 2
D = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("build/shim")
TMP = tempfile.mkdtemp()
bad = 0


def patch(sets):
    return hostcases.Case("shim", "", sets, []).patch()


def read24(p):
    import wave
    w = wave.open(p)
    b = np.frombuffer(w.readframes(w.getnframes()), np.uint8).reshape(-1, 3).astype(np.int32)
    v = b[:, 0] | b[:, 1] << 8 | b[:, 2] << 16
    return (((v << 8) >> 8) / 8388608.).reshape(-1, 2)


def render(eng, sets, shim, ev, env=None, cap=False):
    pf = os.path.join(TMP, "p.raw"); open(pf, "wb").write(patch(sets))
    out = os.path.join(TMP, "o.wav"); cp = os.path.join(TMP, "c.f32")
    e = dict(os.environ, SY_RVQ="0", SY_SHIM=shim, **(env or {}))
    for k in ("SY_SHLOG", "SY_SHCAP", "INPUT"):
        e.pop(k, None)
    if cap:
        e["SY_SHCAP"] = cp
    subprocess.run([str(D / eng), out, "preset", pf] + ev, env=e, check=True)
    x = read24(out); os.unlink(out)
    if cap:
        y = np.fromfile(cp, np.float32).astype(np.float64); os.unlink(cp); return y
    return x


def check(ok, text):
    global bad
    print("%s %s" % ("  ok" if ok else "FAIL", text))
    bad |= not ok


SINE = {"OSC_BAL": 0, "OSC_VOL": 110, "OSC1_SHAPE": 0, "OSC1_WAVE": 0, "AE_A": 0, "AE_S": 127, "AE_R": 40, "CUT1": 127,
        "FX_MODE": 2, "FX_SEND": 100, "FX_TIME": 40, "FX_DECAY": 80, "FX_DEPTH": 3, "FX_COLOUR": 64}
NB = lambda s: str(int(s * FS / 128))


def cents(y, f, t0=1.5, sm=0.02):
    sos = butter(3, [f * 0.95, f * 1.05], btype="band", fs=FH, output="sos")
    fi = np.diff(np.unwrap(np.angle(hilbert(sosfiltfilt(sos, y))))) * FH / (2 * np.pi)
    n = int(sm * FH)
    fi = np.convolve(fi, np.ones(n) / n, "same")[int(t0 * FH):-int(0.3 * FH)]
    return 1200 * np.log2(fi / f)


def strongest(c):
    d = (c - c.mean())[::32]
    sp = np.abs(np.fft.rfft(d * np.hanning(len(d))))
    fr = np.fft.rfftfreq(len(d), 32 / FH)
    k = np.argmax(sp[3:]) + 3
    return fr[k]


def pitch_checks():
    print("== pitch (the octave shifter's own output, a sine A3 held, MODULATION's triangle)")
    tune = {"CLN_RVS_TUNE": "9=0"}
    for m in (0, 32, 64, 96, 127):
        y = render("e_dry", SINE, "30,0,64,96,%d,0,1,0" % m, ["run", "10", "on", "57", "100", "run", NB(8)], tune, True)
        c = cents(y, 440.)
        hr = (np.percentile(c, 99.5) - np.percentile(c, 0.5)) / 2
        want = 50. * m * m / 16129.
        if m == 0:
            check(hr < 0.5, "MODULATION 0: no deviation (half range %.2f c)" % hr)
        else:
            f = strongest(c)
            check(abs(hr - want) <= 0.1 * want + 0.5 and 1.0 < f < 1.3,
                  "MODULATION %d: +-%.1f c (nominal %.1f), %.3f Hz" % (m, hr, want, f))
    l3 = dict(SINE); l3.update({128 + 7: 90, 128 + 8: 1, 128 + 12: 127, 128 + 11: 3})
    y = render("e_dry", l3, "30,0,64,96,127,0,1,0", ["run", "10", "on", "57", "100", "run", NB(8)], dict(tune, SY_SHRT="11=2"), True)
    c = cents(y, 440.); f = strongest(c)
    check(c.std() > 10. and not 1.0 < f < 1.3, "LFO 3 on SHIM MOD (rate 90, triangle): rms %.1f c at %.2f Hz (not the reverb's)" % (c.std(), f))
    y = render("e_dry", l3, "30,0,64,96,0,0,1,0", ["run", "10", "on", "57", "100", "run", NB(8)], dict(tune, SY_SHRT="11=2"), True)
    c = cents(y, 440.)
    check(c.std() < 0.3, "LFO 3 on SHIM MOD, MODULATION 0: none (rms %.2f c)" % c.std())


def null_checks():
    print("== MODULATION 0: the same render with SHIM MOD's route set and not (no LFO 3, no source)")
    a = render("e_new", SINE, "64,0,100,64,0,0,0,57", ["run", "10", "on", "57", "100", "run", NB(4), "off", "57", "run", NB(4)])
    b = render("e_new", SINE, "64,0,100,64,0,0,0,57", ["run", "10", "on", "57", "100", "run", NB(4), "off", "57", "run", NB(4)],
               {"SY_SHRT": "0=2"})
    check(np.array_equal(a, b), "matrix slot 1 on SHIM MOD without a source: bit-identical")


def pad(decay=110, room=3):
    return {"OSC_BAL": 64, "OSC_VOL": 100, "OSC2_DET": 64, "AE_A": 40, "AE_D": 90, "AE_S": 127, "AE_R": 70, "CUT1": 90,
            "FX_MODE": 2, "FX_SEND": 100, "FX_TIME": 40, "FX_DECAY": decay, "FX_DEPTH": room, "FX_COLOUR": 64}


def level(eng, shim, sets, hold=28.):
    ch = (48, 55, 60, 64)
    ev = ["run", "10"] + sum([["on", str(n), "100"] for n in ch], []) + ["run", NB(hold)]
    x1 = render(eng, sets, shim, ev)
    s0 = shim.split(","); s0[0] = "0"
    x0 = render(eng, sets, ",".join(s0), ev)
    d = (x1 - x0)[:, 0] ** 2 + (x1 - x0)[:, 1] ** 2
    n = int(FS * 0.5); L = 10 * np.log10(d[:len(d) // n * n].reshape(-1, n).mean(1) + 1e-20)
    seg = L[16:]
    tr = np.convolve(seg, np.ones(9) / 9, "same")
    dv = (seg - tr)[4:-4]
    return dv.max() - dv.min(), seg.mean(), L[-8:].mean() - L[16:24].mean()


def level_checks():
    print("== level: a held chord, the shimmer part's 0.5 s level detrended (peak to peak)")
    for a in (64, 110, 127):
        free, fl, _ = level("e_new", "%d,0,100,64,0,0,1,57" % a, pad())
        new, nl, _ = level("e_new", "%d,0,100,64,0,0,0,57" % a, pad())
        old, ol, _ = level("e_l0", "%d,0,100,64,0,0,0,57" % a, pad())
        check(new <= free + 1.0 and new <= 3.5,
              "AMOUNT %d TAIL LIMITED: %.1f dB p-p (FREE %.1f; 1.18C44 %.1f); level %.1f dB (1.18C44 %.1f, FREE %.1f)"
              % (a, new, free, old, nl, ol, fl))
    pp, lv, gr = level("e_new", "127,2,127,64,127,0,0,57", pad(116, 2))
    check(abs(gr) < 1.5 and pp <= 3.5, "AMOUNT 127 MODULATION 127 (OCT+OCT, GRAIN 200 ms), decay 116: %.1f dB p-p, the "
          "level's change from 8-12 s to the last 4 s %+.1f dB (bounded)" % (pp, gr))


try:
    pitch_checks()
    null_checks()
    level_checks()
finally:
    shutil.rmtree(TMP)
sys.exit(1 if bad else 0)
