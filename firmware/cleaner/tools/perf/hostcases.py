#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""hostcases.py: the fixed case set of the host profiler (hostprof.sh) and the null test (nulltest.py).

Every case is a patch of the project's own, built from the INIT patch (fm1/init_patch.h) by setting named bytes
(names after the development notes), plus the notes played and the environment of the render (SY_RVQ=0 selects
the reverb's HALF network, as on the device; the host's default is FULL). No third-party patch is stored here: the
32-patch test bank is read from $TESTBANK (default: the team repository's reference/patches) at run time.

  hostcases.py --list                 the cases and their notes
  hostcases.py --write DIR            the patches as DIR/NAME.raw (256 bytes each) and DIR/cases.tsv
As a module: CASES (name -> Case), bank_cases(), events(case, blocks)."""
import os
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
INIT = HERE.parents[1] / "fm1" / "init_patch.h"
BASE = [int(x) for x in re.findall(r"\d+", INIT.read_text().split("{", 1)[1].split("}", 1)[0])]
assert len(BASE) == 256
TESTBANK = os.environ.get("TESTBANK", "/path/to/spec/reference/patches")

# byte names (page A; page B bytes are 128 + n)
B = dict(PAN=10, OSC1_SHAPE=17, OSC1_PW=18, OSC1_WAVE=19, OSC2_SHAPE=22, OSC2_PW=23, OSC2_WAVE=24, OSC2_SEMI=25,
         OSC2_DET=26, OSC2_FM=27, OSC2_SYNC=28, OSC_BAL=33, SUB_VOL=34, SUB_SHAPE=35, OSC_VOL=36, NOISE=37, RING=38,
         CUT1=40, RES1=42, FENV1=44, SAT=49, FE_D=55, FE_S=56, AE_A=59, AE_D=60, AE_S=61, AE_R=63,
         LFO1_RATE=67, LFO1_SHAPE=68, LFO1_OSC1=74, LFO1_OSC2=75, LFO1_PW=76,
         UNISON=97, UNI_DET=98, UNI_SPREAD=99, CHORUS=105, FX_MODE=112, FX_SEND=113, FX_TIME=114, FX_FB=115,
         FX_DECAY=116, FX_DEPTH=117, FX_COLOUR=119, FM_MODE=128 + 34, OSC3_MODE=128 + 41)

N4 = lambda r: [r, r + 4, r + 7, r + 12]
CH8 = [48, 52, 55, 59, 60, 64, 67, 71]
EA = [36, 40, 43, 47, 48, 52, 55, 59, 60, 64, 67, 71, 72, 76, 79, 83]
REVERB = {"FX_MODE": 2, "FX_SEND": 86, "FX_TIME": 64, "FX_DECAY": 104, "FX_DEPTH": 72 - 60, "FX_COLOUR": 49}


class Case:
    def __init__(self, name, doc, sets, notes, env=None, vel=100):
        self.name, self.doc, self.sets, self.notes, self.vel = name, doc, sets, list(notes), vel
        self.env = dict(env or {})

    def patch(self):
        p = list(BASE)
        for k, v in self.sets.items():
            p[B[k] if isinstance(k, str) else k] = v
        name = self.name[:10].encode()
        p[240:240 + len(name)] = list(name)
        return bytes(p)


HALF = {"SY_RVQ": "0"}
FULL = {"SY_RVQ": "1"}
CASES = {}


def add(*a, **k):
    c = Case(*a, **k)
    CASES[c.name] = c


# EURO-like: the CPU plan's EURO case as the project's own patch (euro_like.py): a 4-voice unison string pad, full keys
add("euro", "EURO-like: unison 4, both oscillators, reverb HALF, 16 notes",
    {"OSC_BAL": 64, "OSC_VOL": 100, "OSC2_DET": 70, "AE_A": 40, "AE_D": 90, "AE_S": 100, "AE_R": 90, "UNISON": 3,
     "UNI_DET": 127, "UNI_SPREAD": 127, **REVERB}, EA, HALF)
# the same with the ring modulator and the sub oscillator (the CPU plan's EURO figures are of a ring + sub patch: the
# oscillator path of RING_SUB: co_ring, sub_ring, osc_run, blep)
add("euro_ring", "EURO-like + ring modulator + sub oscillator (RING_SUB), 16 notes",
    {"OSC_BAL": 64, "OSC_VOL": 100, "OSC2_DET": 70, "AE_A": 40, "AE_D": 90, "AE_S": 100, "AE_R": 90, "UNISON": 3,
     "UNI_DET": 127, "UNI_SPREAD": 127, "RING": 64, "SUB_VOL": 80, **REVERB}, EA, HALF)
# A010-like: osc 2 hard-synced and tuned up, the ring modulator and the sub oscillator
# (wave 2) the paths of the factory patch A010 in the emulator: osc 2 synced with the triangle FM (mode 0), osc 3 on
# (mode 1), ring + sub (RING_SUB), the noise (low pass), unison 4 (the 6-voice state), osc 1's pulse width moving (its
# increment off the window's step: PW_PH), osc 2 a sine + saw morph (the sine in the sync loop)
add("a010", "A010-like: sync + FM, ring + sub, osc 3, noise, moving pulse width, unison 4, 8 notes (16 voices)",
    {"OSC_BAL": 64, "OSC2_SEMI": 64 + 7, "OSC2_SYNC": 1, "RING": 64, "SUB_VOL": 90, "OSC_VOL": 90, "UNISON": 3,
     "UNI_DET": 60, "OSC2_FM": 4, "OSC3_MODE": 1, "NOISE": 35, "OSC1_SHAPE": 66, "OSC2_SHAPE": 60, "LFO1_RATE": 60,
     "LFO1_SHAPE": 1, "LFO1_PW": 64 + 20}, CH8, HALF)
# (wave 1's a010, M1: ring + sub + sync, unison 2; no FM, no osc 3, no noise)
add("a010_m1", "A010-like of M1: ring + sub + sync, unison 2, 8 notes (16 voices)",
    {"OSC_BAL": 64, "OSC2_SEMI": 64 + 7, "OSC2_SYNC": 1, "RING": 64, "SUB_VOL": 90, "OSC_VOL": 90, "UNISON": 1,
     "UNI_DET": 60}, CH8, HALF)
add("ring", "ring modulator only (osc 2 detuned up), 8 notes",
    {"OSC_BAL": 64, "OSC2_SEMI": 64 + 12, "RING": 90, "OSC_VOL": 80}, CH8, HALF)
add("unison_saw", "detuned unison saw chord (unison 3, both oscillators), 4 notes at 60",
    {"OSC_BAL": 64, "OSC2_DET": 50, "UNISON": 3, "UNI_DET": 90, "UNI_SPREAD": 127}, N4(60), HALF)
add("pw_pulse", "PW-modulated pulse chord (LFO 1 -> pulse width, both oscillators), 4 notes at 60",
    {"OSC1_SHAPE": 127, "OSC1_PW": 30, "OSC2_SHAPE": 127, "OSC2_PW": 30, "OSC_BAL": 64, "OSC2_DET": 36,
     "LFO1_RATE": 60, "LFO1_SHAPE": 1, "LFO1_PW": 64 + 50}, N4(60), HALF)
add("pulse_fix", "fixed-width pulse chord (pw 30), 4 notes at 60",
    {"OSC1_SHAPE": 127, "OSC1_PW": 30}, N4(60), HALF)
add("tri", "triangle chord (wave 1 in the sine slot), 4 notes at 60",
    {"OSC1_SHAPE": 0, "OSC1_WAVE": 1}, N4(60), HALF)
for k in (36, 60, 84):
    add("saw%d" % k, "plain saw chord, 4 notes from key %d" % k, {}, N4(k), HALF)
add("saw1v", "plain saw, one voice (key 60)", {}, [60], HALF)
add("saw16v", "plain saw, 16 voices (16 notes)", {}, EA, HALF)
add("fm", "FM: osc 1 sine modulates osc 2 sine (FM 64), 4 notes at 60",
    {"OSC1_SHAPE": 0, "OSC2_SHAPE": 0, "OSC_BAL": 127, "OSC2_FM": 64}, N4(60), HALF)
add("fm_saw", "FM: osc 1 saw modulates osc 2 saw (FM 64), 4 notes at 60",
    {"OSC_BAL": 127, "OSC2_FM": 64}, N4(60), HALF)
add("table", "table waves: osc 1 wave 20, osc 2 wave 41, 4 notes at 60",
    {"OSC1_SHAPE": 0, "OSC1_WAVE": 20, "OSC2_SHAPE": 0, "OSC2_WAVE": 41, "OSC_BAL": 64, "OSC2_DET": 40}, N4(60), HALF)
add("pad_dry", "two detuned saws, filter envelope, no effects, 4 notes at 60",
    {"OSC_BAL": 64, "OSC2_DET": 40, "CUT1": 80, "RES1": 40, "FENV1": 80}, N4(60))
add("pad_rev", "pad_dry + reverb HALF (the device's mode)", {"OSC_BAL": 64, "OSC2_DET": 40, "CUT1": 80, "RES1": 40,
    "FENV1": 80, **REVERB}, N4(60), HALF)
add("pad_revfull", "pad_dry + reverb FULL", {"OSC_BAL": 64, "OSC2_DET": 40, "CUT1": 80, "RES1": 40, "FENV1": 80,
    **REVERB}, N4(60), FULL)
add("k1like", "K1-like: unison 2, two saws, chorus, reverb HALF, 3 notes (one part of the K1 layer)",
    {"OSC_BAL": 64, "OSC2_DET": 45, "UNISON": 1, "UNI_DET": 60, "CUT1": 90, "RES1": 30, "FENV1": 60, "CHORUS": 70,
     **REVERB}, [60, 64, 67], HALF)

# the plain oscillator path (package P3: sync, FM, the simple oscillator for the sub and osc 3): every sync wave, every
# FM mode, the ring, the sub (square, triangle) and the osc 3 modes; 8 notes over the keys 24-96 (the chord turns
# the 6-voice state on)
KR8 = [24, 36, 43, 55, 67, 79, 88, 96]
SYNC = {"OSC_BAL": 64, "OSC2_SEMI": 64 + 7, "OSC2_SYNC": 1, "OSC_VOL": 80}
add("sy_saw", "sync, osc 2 saw (shape 64), osc 1 saw, keys 24-96", dict(SYNC), KR8, HALF)
add("sy_pulse", "sync, osc 2 pulse (shape 127, pw 30), osc 1 pulse, keys 24-96",
    {**SYNC, "OSC2_SHAPE": 127, "OSC2_PW": 30, "OSC1_SHAPE": 127, "OSC1_PW": 90}, KR8, HALF)
add("sy_sine", "sync, osc 2 sine, osc 1 sine, keys 24-96", {**SYNC, "OSC2_SHAPE": 0, "OSC1_SHAPE": 0}, KR8, HALF)
add("sy_table", "sync, osc 2 table wave 41, osc 1 table wave 20, keys 24-96",
    {**SYNC, "OSC2_SHAPE": 0, "OSC2_WAVE": 41, "OSC1_SHAPE": 0, "OSC1_WAVE": 20}, KR8, HALF)
add("sy_mix", "sync, osc 2 triangle + saw (shape 40), osc 1 sine + saw (shape 30), keys 24-96",
    {**SYNC, "OSC2_SHAPE": 40, "OSC2_WAVE": 1, "OSC1_SHAPE": 30}, KR8, HALF)
add("sy_npulse", "sync, osc 1 narrow pulse (wave 64), osc 2 table + saw (shape 20, wave 30), keys 24-96",
    {**SYNC, "OSC1_SHAPE": 0, "OSC1_WAVE": 64, "OSC2_SHAPE": 20, "OSC2_WAVE": 30}, KR8, HALF)
add("sy_subtri", "sync + ring + triangle sub, keys 24-96",
    {**SYNC, "RING": 80, "SUB_VOL": 90, "SUB_SHAPE": 64}, KR8, HALF)
add("sy_sub", "sync + square sub (no ring), keys 24-96", {**SYNC, "SUB_VOL": 90}, KR8, HALF)
for m, o3 in ((1, 1), (2, 2), (3, 3), (4, 4), (5, 5), (10, 10), (68, 68)):
    add("sy_o3m%d" % m, "sync + osc 3 mode %d, keys 24-96" % m, {**SYNC, "OSC3_MODE": o3, 128 + 42: 100}, KR8, HALF)
for m in range(5):
    add("fm_m%d" % m, "FM mode %d (FM 80), osc 1 sine, osc 2 saw + pulse, keys 24-96" % m,
        {"OSC1_SHAPE": 0, "OSC_BAL": 100, "OSC2_FM": 80, "FM_MODE": m}, KR8, HALF)
add("fm_m2saw", "FM mode 2 by osc 1's saw (shape 100), osc 2 table wave 30, keys 24-96",
    {"OSC1_SHAPE": 100, "OSC2_SHAPE": 0, "OSC2_WAVE": 30, "OSC_BAL": 100, "OSC2_FM": 90, "FM_MODE": 2}, KR8, HALF)
add("fm_sync_ring", "FM mode 1 + sync + ring + sub + osc 3 pulse, keys 24-96",
    {**SYNC, "OSC2_FM": 50, "FM_MODE": 1, "RING": 70, "SUB_VOL": 70, "OSC3_MODE": 3, 128 + 42: 80}, KR8, HALF)
add("ring_sub", "ring + square sub on the own clock (RING_SUB: sub_ring reads osc 2), keys 24-96",
    {"OSC_BAL": 64, "OSC2_SEMI": 64 + 5, "RING": 80, "SUB_VOL": 90}, KR8, HALF)
add("tk_o3", "own clock + osc 3 table + triangle sub, keys 24-96",
    {"OSC_BAL": 64, "OSC2_DET": 50, "SUB_VOL": 60, "SUB_SHAPE": 30, "OSC3_MODE": 12, 128 + 42: 90}, KR8, HALF)

# the oscillator package's (P1, P2) coverage: CLEAN and between, the triangle, the pulse fixed and moving, the sine,
# the narrow pulse, the table waves, the ring with each, osc 2 alone, the keys' range (8 notes: the null test's chord)
LO8 = [24, 28, 31, 36, 40, 43, 48, 52]
HI8 = [72, 76, 79, 84, 88, 91, 96, 100]
CLEAN = {"SY_OSC_CLEAN": "1"}
MIDC = {"SY_OSC_CLEAN": "0.5"}
add("clean_uni", "CLEAN: detuned unison saw chord (unison 3, both oscillators)",
    {"OSC_BAL": 64, "OSC2_DET": 50, "UNISON": 3, "UNI_DET": 90, "UNI_SPREAD": 127}, N4(60), dict(HALF, **CLEAN))
add("clean_pwm", "CLEAN: PW-modulated pulse chord", {"OSC1_SHAPE": 127, "OSC1_PW": 30, "OSC2_SHAPE": 127, "OSC2_PW": 30,
    "OSC_BAL": 64, "OSC2_DET": 36, "LFO1_RATE": 60, "LFO1_SHAPE": 1, "LFO1_PW": 64 + 50}, N4(60), dict(HALF, **CLEAN))
add("clean_ring_hi", "CLEAN: ring, pulse and saw, high keys", {"OSC1_SHAPE": 127, "OSC1_PW": 20, "OSC_BAL": 64,
    "OSC2_SEMI": 64 + 7, "RING": 80}, HI8, dict(HALF, **CLEAN))
add("mid_tri_ring", "osc_clean 0.5: triangle and saw, ring", {"OSC1_SHAPE": 0, "OSC1_WAVE": 1, "OSC_BAL": 64,
    "OSC2_DET": 50, "RING": 70}, N4(60), dict(HALF, **MIDC))
add("clean_sine_lo", "CLEAN: sine + saw mix, low keys", {"OSC1_SHAPE": 30, "OSC1_WAVE": 0, "OSC_BAL": 64,
    "OSC2_SHAPE": 100, "OSC2_DET": 30}, LO8, dict(HALF, **CLEAN))
add("sine", "sines (osc 1 and 2), 4 notes at 60", {"OSC1_SHAPE": 0, "OSC1_WAVE": 0, "OSC2_SHAPE": 0, "OSC2_WAVE": 0,
    "OSC_BAL": 64, "OSC2_DET": 40}, N4(60), HALF)
add("sine_saw", "sine + saw shapes (32), both oscillators", {"OSC1_SHAPE": 32, "OSC2_SHAPE": 40, "OSC_BAL": 64,
    "OSC2_DET": 45}, N4(60), HALF)
add("narrow", "narrow pulse (wave 64) and pulse", {"OSC1_SHAPE": 0, "OSC1_WAVE": 64, "OSC2_SHAPE": 127, "OSC2_PW": 64,
    "OSC_BAL": 64, "OSC2_DET": 40}, N4(48), HALF)
add("tri2_pw_hi", "osc 2 triangle, osc 1 pulse width moving, high keys", {"OSC1_SHAPE": 127, "OSC1_PW": 50,
    "OSC2_SHAPE": 0, "OSC2_WAVE": 1, "OSC_BAL": 64, "LFO1_RATE": 70, "LFO1_SHAPE": 1, "LFO1_PW": 64 + 40}, HI8, HALF)
add("ring_tri", "ring: triangle x saw", {"OSC1_SHAPE": 0, "OSC1_WAVE": 1, "OSC_BAL": 64, "OSC2_SEMI": 64 + 5,
    "RING": 90}, N4(60), HALF)
add("ring_pulse", "ring: pulse x pulse (fixed widths)", {"OSC1_SHAPE": 127, "OSC1_PW": 30, "OSC2_SHAPE": 127,
    "OSC2_PW": 90, "OSC_BAL": 64, "OSC2_SEMI": 64 + 12, "RING": 90}, N4(60), HALF)
add("ring_pwm", "ring: PW-modulated pulse x saw", {"OSC1_SHAPE": 127, "OSC1_PW": 30, "OSC_BAL": 64, "OSC2_SEMI": 64 + 3,
    "RING": 90, "LFO1_RATE": 60, "LFO1_SHAPE": 1, "LFO1_PW": 64 + 50}, N4(60), HALF)
add("ring_sine", "ring: sine x saw", {"OSC1_SHAPE": 0, "OSC1_WAVE": 0, "OSC_BAL": 64, "OSC2_SEMI": 64 + 7,
    "RING": 90}, N4(60), HALF)
add("ring_table", "ring: table x pulse", {"OSC1_SHAPE": 0, "OSC1_WAVE": 20, "OSC2_SHAPE": 127, "OSC_BAL": 64,
    "OSC2_SEMI": 64 + 7, "RING": 90}, N4(60), HALF)
add("ring_only", "ring only (osc levels 0), saws", {"OSC_BAL": 64, "OSC_VOL": 0, "OSC2_SEMI": 64 + 7, "RING": 110},
    N4(60), HALF)
add("osc2_only", "osc 2 alone (balance 127), pulse", {"OSC_BAL": 127, "OSC2_SHAPE": 127, "OSC2_PW": 40}, N4(60), HALF)
add("keys_lo", "saw and pulse, 8 low keys (24-52)", {"OSC_BAL": 64, "OSC2_SHAPE": 127, "OSC2_DET": 40}, LO8, HALF)
add("keys_hi", "saw and pulse, 8 high keys (72-100)", {"OSC_BAL": 64, "OSC2_SHAPE": 127, "OSC2_DET": 40}, HI8, HALF)
add("pulse_edge", "pulses at the widths' ends (osc 2 127: its fall a tick from its wrap, osc 1 0), ring, sub",
    {"OSC1_SHAPE": 127, "OSC1_PW": 0, "OSC2_SHAPE": 127, "OSC2_PW": 127, "OSC_BAL": 64, "OSC2_DET": 82, "RING": 78,
     "SUB_VOL": 24}, N4(48), HALF)
add("pulse_edge2", "osc 2's pulse at width 127 alone, keys 24-52", {"OSC_BAL": 127, "OSC2_SHAPE": 127, "OSC2_PW": 127},
    LO8, HALF)
add("v6_load", "the 6-voice state from the load: 3 notes, analog filter, ring, reverb", {"OSC_BAL": 64,
    "OSC2_DET": 40, "RING": 40, 51: 5, "SAT": 1, "NOISE": 10, **REVERB}, [60, 64, 67], HALF)

# (cpu-p1b) the triangle and the narrow pulse by events, CLEAN by events, the sines and tables: their coverage (keys
# 24-96, the ring, mixes, the pulse width's extremes; the chord of 8 turns the 6-voice state on)
add("tri_kr", "osc 1 triangle + saw (shape 30), osc 2 triangle, keys 24-96", {"OSC1_SHAPE": 30, "OSC1_WAVE": 1,
    "OSC2_SHAPE": 0, "OSC2_WAVE": 1, "OSC_BAL": 64, "OSC2_DET": 40}, KR8, HALF)
add("tri_hi", "triangle chord, high keys (72-100)", {"OSC1_SHAPE": 0, "OSC1_WAVE": 1}, HI8, HALF)
add("tri_ring_kr", "ring: triangle x triangle (osc 2 +7), keys 24-96", {"OSC1_SHAPE": 0, "OSC1_WAVE": 1, "OSC2_SHAPE": 0,
    "OSC2_WAVE": 1, "OSC_BAL": 64, "OSC2_SEMI": 64 + 7, "RING": 90}, KR8, HALF)
add("tri_ring_pw", "ring: osc 2 triangle x osc 1 pulse, its width moving (fast LFO)", {"OSC1_SHAPE": 127, "OSC1_PW": 40,
    "OSC2_SHAPE": 0, "OSC2_WAVE": 1, "OSC_BAL": 64, "OSC2_SEMI": 64 + 5, "RING": 90, "LFO1_RATE": 100, "LFO1_SHAPE": 1,
    "LFO1_PW": 64 + 60}, KR8, HALF)
add("tri_ring_saw", "ring: osc 1 triangle + saw (shape 20) x osc 2 saw, sub", {"OSC1_SHAPE": 20, "OSC1_WAVE": 1,
    "OSC_BAL": 64, "OSC2_SEMI": 64 - 5, "RING": 90, "SUB_VOL": 50}, KR8, HALF)
add("narrow_kr", "narrow pulse alone (osc 1), keys 24-96", {"OSC1_SHAPE": 0, "OSC1_WAVE": 64, "OSC_BAL": 0}, KR8, HALF)
add("narrow_ring", "ring: narrow pulse x saw, keys 24-96", {"OSC1_SHAPE": 0, "OSC1_WAVE": 64, "OSC_BAL": 64,
    "OSC2_SEMI": 64 + 3, "RING": 90}, KR8, HALF)
add("narrow_mix", "narrow pulse + saw (shape 40) and pulse, high keys", {"OSC1_SHAPE": 40, "OSC1_WAVE": 64,
    "OSC2_SHAPE": 127, "OSC2_PW": 100, "OSC_BAL": 64, "OSC2_DET": 30}, HI8, HALF)
for _n, _r in (("fast", 110), ("slow", 20)):
    add("pw_ext_" + _n, "pulse widths swept over the whole range (%s LFO), osc 2 saw, ring" % _n, {"OSC1_SHAPE": 127,
        "OSC1_PW": 0, "OSC2_SHAPE": 127, "OSC2_PW": 0, "OSC_BAL": 64, "OSC2_DET": 30, "RING": 40, "LFO1_RATE": _r,
        "LFO1_SHAPE": 1, "LFO1_PW": 127}, KR8, HALF)
    add("pul0_" + _n, "osc 2's pulse above half the clock (+36 semitones), its width moving (%s LFO)" % _n,
        {"OSC1_SHAPE": 64, "OSC2_SHAPE": 127, "OSC2_PW": 10, "OSC_BAL": 64, "OSC2_SEMI": 64 + 36, "LFO1_RATE": _r,
         "LFO1_SHAPE": 1, "LFO1_PW": 64 + 60}, HI8, HALF)
add("pw_0", "pulse widths at -127 / 127 (osc 1 / osc 2), ring", {"OSC1_SHAPE": 127, "OSC1_PW": 0, "OSC2_SHAPE": 127,
    "OSC2_PW": 127, "OSC_BAL": 64, "OSC2_SEMI": 64 + 7, "RING": 60}, KR8, HALF)
for _w, _sets in (("saw", {}), ("pulse", {"OSC1_SHAPE": 127, "OSC1_PW": 30, "OSC2_SHAPE": 127, "OSC2_PW": 90}),
                  ("tri", {"OSC1_SHAPE": 0, "OSC1_WAVE": 1, "OSC2_SHAPE": 0, "OSC2_WAVE": 1}),
                  ("narrow", {"OSC1_SHAPE": 0, "OSC1_WAVE": 64, "OSC2_SHAPE": 100}),
                  ("sine", {"OSC1_SHAPE": 0, "OSC2_SHAPE": 0}),
                  ("table", {"OSC1_SHAPE": 0, "OSC1_WAVE": 20, "OSC2_SHAPE": 0, "OSC2_WAVE": 41})):
    add("cl_" + _w, "CLEAN: %s, both oscillators, keys 24-96" % _w, dict({"OSC_BAL": 64, "OSC2_DET": 40}, **_sets), KR8,
        dict(HALF, **CLEAN))
    add("cl_%s_ring" % _w, "CLEAN: %s, ring (osc 2 +7), keys 24-96" % _w, dict({"OSC_BAL": 64, "OSC2_SEMI": 64 + 7,
        "RING": 80}, **_sets), KR8, dict(HALF, **CLEAN))
add("mid_saw", "osc_clean 0.5: saw and pulse, keys 24-96", {"OSC_BAL": 64, "OSC2_SHAPE": 127, "OSC2_DET": 40}, KR8,
    dict(HALF, **MIDC))
add("sine_kr", "sines, keys 24-96", {"OSC1_SHAPE": 0, "OSC2_SHAPE": 0, "OSC_BAL": 64, "OSC2_DET": 40}, KR8, HALF)
add("table_kr", "table waves (20, 41), keys 24-96", {"OSC1_SHAPE": 0, "OSC1_WAVE": 20, "OSC2_SHAPE": 0, "OSC2_WAVE": 41,
    "OSC_BAL": 64, "OSC2_DET": 40}, KR8, HALF)
add("tabsaw_kr", "osc 1 table + saw (shape 30, wave 20), osc 2 pulse, keys 24-96", {"OSC1_SHAPE": 30, "OSC1_WAVE": 20,
    "OSC2_SHAPE": 127, "OSC2_PW": 40, "OSC_BAL": 64, "OSC2_DET": 40}, KR8, HALF)
add("ring_sine_kr", "ring: sine x sine (osc 2 +7), keys 24-96", {"OSC1_SHAPE": 0, "OSC2_SHAPE": 0, "OSC_BAL": 64,
    "OSC2_SEMI": 64 + 7, "RING": 90}, KR8, HALF)
add("ring_tab_kr", "ring: table x table (osc 2 +5), keys 24-96", {"OSC1_SHAPE": 0, "OSC1_WAVE": 20, "OSC2_SHAPE": 0,
    "OSC2_WAVE": 41, "OSC_BAL": 64, "OSC2_SEMI": 64 + 5, "RING": 90}, KR8, HALF)
add("sine_mix_kr", "sine + saw (shape 32) x table + saw (shape 40), keys 24-96", {"OSC1_SHAPE": 32, "OSC2_SHAPE": 40,
    "OSC2_WAVE": 30, "OSC_BAL": 64, "OSC2_DET": 45}, KR8, HALF)
add("uni6_tri", "unison 3, triangle + saw and saw: the 6-voice state, 16 notes", {"OSC1_SHAPE": 20, "OSC1_WAVE": 1,
    "OSC_BAL": 64, "OSC2_DET": 50, "UNISON": 3, "UNI_DET": 90}, EA, HALF)
add("uni6_clean", "CLEAN: unison 3 saws, 16 notes", {"OSC_BAL": 64, "OSC2_DET": 50, "UNISON": 3, "UNI_DET": 90}, EA,
    dict(HALF, **CLEAN))

# (cpu-cave) the ring with a sine + saw mix, the sub in the ring (RING_SUB), unison: a user patch's paths, the project's
# own values: osc 1 sine + saw (shape 38), osc 2 saw (shape 64: its pulse part a hair) an octave up and detuned, the
# ring, the sub, osc 3 (mode 2), the noise, unison 3, chorus, reverb HALF
CAVE = {"OSC1_SHAPE": 38, "OSC1_WAVE": 0, "OSC2_SHAPE": 64, "OSC2_SEMI": 64 + 12, "OSC2_DET": 103, "OSC_BAL": 64,
        "SUB_VOL": 84, "NOISE": 14, "RING": 72, "UNISON": 3, "UNI_DET": 83, "UNI_SPREAD": 96, "OSC3_MODE": 2,
        "CHORUS": 21, "CUT1": 70, "AE_A": 13, "AE_D": 74, "AE_S": 90, "AE_R": 64, **REVERB, "FX_SEND": 65}
add("ring_sine_sub_uni", "ring x sine + saw (shape 38) with the sub (RING_SUB), osc 3, unison 3, 4 notes (16 voices)",
    dict(CAVE), [48, 55, 60, 64], HALF)
add("ring_sine_sub_1k", "ring_sine_sub_uni, one key (4 voices)", dict(CAVE), [57], HALF)
# (cpu-cave) co_runs' coverage (CO_PSIN): sine + saw mixes at several shapes, the ring with a sine on either side and on
# both, with the triangle, the narrow pulse, a moving pulse width, a table, the sub, unison and the 6-voice state, keys
# 24-96, CLEAN
for _s in (10, 38, 54):
    add("sinemix%d_kr" % _s, "osc 1 sine + saw (shape %d), osc 2 saw (shape 64) +12, keys 24-96" % _s,
        {"OSC1_SHAPE": _s, "OSC_BAL": 64, "OSC2_SEMI": 64 + 12, "OSC2_DET": 103}, KR8, HALF)
    add("ring_sinemix%d_kr" % _s, "ring: osc 1 sine + saw (shape %d) x osc 2 saw (shape 64) +12, keys 24-96" % _s,
        {"OSC1_SHAPE": _s, "OSC_BAL": 64, "OSC2_SEMI": 64 + 12, "OSC2_DET": 103, "RING": 72}, KR8, HALF)
add("ring_sine2_kr", "ring: osc 1 sine + saw (shape 38) x osc 2 sine + saw (shape 30) -5, keys 24-96",
    {"OSC1_SHAPE": 38, "OSC2_SHAPE": 30, "OSC_BAL": 64, "OSC2_SEMI": 64 - 5, "RING": 90}, KR8, HALF)
add("ring_o2sine_kr", "ring: osc 1 saw x osc 2 sine + saw (shape 20) +7, ring only levels low, keys 24-96",
    {"OSC2_SHAPE": 20, "OSC_BAL": 100, "OSC2_SEMI": 64 + 7, "RING": 100}, KR8, HALF)
add("ring_tri_sine", "ring: osc 1 triangle x osc 2 sine + saw (shape 30), keys 24-96",
    {"OSC1_SHAPE": 0, "OSC1_WAVE": 1, "OSC2_SHAPE": 30, "OSC_BAL": 64, "OSC2_SEMI": 64 + 5, "RING": 90}, KR8, HALF)
add("ring_npl_sine", "ring: osc 1 narrow pulse x osc 2 sine (shape 0), keys 24-96",
    {"OSC1_SHAPE": 0, "OSC1_WAVE": 64, "OSC2_SHAPE": 0, "OSC_BAL": 64, "OSC2_SEMI": 64 + 3, "RING": 90}, KR8, HALF)
add("ring_pw_sine", "ring: osc 1 pulse, its width moving, x osc 2 sine + saw (shape 25)",
    {"OSC1_SHAPE": 127, "OSC1_PW": 40, "OSC2_SHAPE": 25, "OSC_BAL": 64, "OSC2_SEMI": 64 + 5, "RING": 90,
     "LFO1_RATE": 100, "LFO1_SHAPE": 1, "LFO1_PW": 64 + 60}, KR8, HALF)
add("ring_sine_tab", "ring: osc 1 sine + saw (shape 38) x osc 2 table 41 (per sample: co_rung)",
    {"OSC1_SHAPE": 38, "OSC2_SHAPE": 0, "OSC2_WAVE": 41, "OSC_BAL": 64, "OSC2_SEMI": 64 + 7, "RING": 90}, KR8, HALF)
add("ring_sine_sub_kr", "the CAVE case (ring x sine + saw, sub, osc 3, unison 3), keys 24-96", dict(CAVE), KR8, HALF)
add("ring_sine_sub16", "the CAVE case, 16 notes (the 6-voice state)", dict(CAVE), EA, HALF)
add("sine_hi", "sine + saw (shape 30) and sine, high keys (72-100)", {"OSC1_SHAPE": 30, "OSC2_SHAPE": 0, "OSC_BAL": 64,
    "OSC2_DET": 40, "RING": 50}, HI8, HALF)
add("cl_sinemix", "CLEAN: osc 1 sine + saw (shape 38), osc 2 pulse, keys 24-96", {"OSC1_SHAPE": 38, "OSC2_SHAPE": 127,
    "OSC2_PW": 40, "OSC_BAL": 64, "OSC2_DET": 40}, KR8, dict(HALF, **CLEAN))
add("cl_ring_sinemix", "CLEAN: the CAVE case's oscillators with the ring (per sample: co_rung, co_rsmq)",
    {"OSC1_SHAPE": 38, "OSC_BAL": 64, "OSC2_SEMI": 64 + 12, "OSC2_DET": 103, "RING": 72, "SUB_VOL": 84}, KR8,
    dict(HALF, **CLEAN))
add("cl_ring_sinesaw", "CLEAN: ring, osc 1 sine + saw (shape 38) x osc 2 sine + saw (shape 40), 4 notes at 48 (co_rung's quiet"
    " stretches, co_rsmq)", {"OSC1_SHAPE": 38, "OSC2_SHAPE": 40, "OSC_BAL": 64, "OSC2_SEMI": 64 + 7, "RING": 80}, N4(48),
    dict(HALF, **CLEAN))
add("mid_sinemix", "osc_clean 0.5: sine + saw mix and saw, ring", {"OSC1_SHAPE": 38, "OSC_BAL": 64, "OSC2_DET": 50,
    "RING": 60}, KR8, dict(HALF, **MIDC))

# the filter's cases (cpu-filt): filter 1 modes 0-7 (4 / 6 the stale modes with the coloured noise), filter 2 modes 0-3
# in every routing, the balance, the saturation types 1-14 between the filters, high resonance (mode 7 near
# self-oscillation), the noise into the analog loop; a filter envelope (the cutoff moving, then held) and an LFO on
# the cutoff (moving all the time); keys 24-96
FB = {"OSC_BAL": 64, "OSC2_DET": 40, "CUT1": 60, "RES1": 80, "FENV1": 90, "FE_D": 70, "FE_S": 40, 41: 90,
      43: 50, 48: 64, "AE_A": 0, "AE_S": 110}


def _fd(*ds):
    o = {}
    for d in ds:
        o.update(d)
    return o


for _m in range(8):
    add("flt_m1_%d" % _m, "filter 1 mode %d, filter 2 low pass (high pass after mode 1), serial 4" % _m,
        _fd(FB, {51: _m}, {"NOISE": 60, 39: 30} if _m in (4, 6) else {}, {52: 1, 41: 40} if _m == 1 else {}), KR8, HALF)
for _r in range(4):
    for _m in range(4):
        add("flt_r%d_m2_%d" % (_r, _m), "routing %d, filter 2 mode %d, filter 1 low pass" % (_r, _m),
            _fd(FB, {52: _m, 53: _r, 48: 80 if _r >= 2 else 64}), KR8, HALF)
for _r in (0, 1, 2):
    for _b in (0, 32, 96, 127):
        add("flt_r%d_b%d" % (_r, _b), "routing %d, balance %d, filter 2 band pass" % (_r, _b),
            _fd(FB, {52: 2, 53: _r, 48: _b}), KR8, HALF)
for _t in range(1, 15):
    add("flt_sat%d" % _t, "saturation %d at drive (osc volume 110), serial 4" % _t,
        _fd(FB, {"SAT": _t, "OSC_VOL": 110}), KR8, HALF)
for _t in (4, 7, 8, 10):
    add("flt_s6_sat%d" % _t, "serial 6, saturation %d, filter 2 notch" % _t,
        _fd(FB, {"SAT": _t, "OSC_VOL": 100, 53: 1, 52: 3, 48: 40}), KR8, HALF)
add("flt_hires", "both filters at resonance 127, saturation 0", _fd(FB, {"RES1": 127, 43: 127}), KR8, HALF)
add("flt_hires_s4", "both filters at resonance 127, saturation 4", _fd(FB, {"RES1": 127, 43: 127, "SAT": 4,
    "OSC_VOL": 100}), KR8, HALF)
add("flt_s6_bp", "serial 6 band pass at resonance 100", _fd(FB, {51: 2, 53: 1, "RES1": 100, "CUT1": 85, 52: 1, 41: 20}), KR8, HALF)
for _m, _q in ((7, 127), (7, 118), (5, 127)):
    add("flt_a%d_r%d" % (_m, _q), "analog mode %d at resonance %d (near self-oscillation), noise into the loop" % (_m, _q),
        _fd(FB, {51: _m, "RES1": _q, "NOISE": 20, "SAT": 4}), KR8, HALF)
add("flt_m6_dist", "filter 1 mode 6 with the distortion on (mode 4 in a voice)", _fd(FB, {51: 6, 128 + 100: 3}),
    KR8, HALF)
add("flt_lfo", "LFO 2 on both cutoffs (moving all the time), serial 6, saturation 1",
    _fd(FB, {79: 90, 80: 1, 87: 64 + 40, 88: 64 + 30, 53: 1, "SAT": 1, "OSC_VOL": 90}), KR8, HALF)
add("flt_split", "split routing, filter 2 high pass, saturation 5", _fd(FB, {53: 3, 52: 1, "SAT": 5,
    "OSC_VOL": 100}), KR8, HALF)
for _r, _t in ((0, 7), (1, 4), (2, 0), (3, 4), (0, 0)):
    add("flt_f2off_r%d_s%d" % (_r, _t), "filter 2 off (balance 0), routing %d, saturation %d" % (_r, _t),
        _fd(FB, {48: 0, 53: _r, "SAT": _t, "OSC_VOL": 100}), KR8, HALF)

# (1.18C37) the phaser at each stage count (its loop per count, PH_SPEC) and the distortion's types (its code in the
# flash on the FM-1, FX_DIST_FLASH): the effects on a saw chord
for _n in range(6):
    add("fx_ph_n%d" % (_n + 1), "phaser, %d stages, mix 80, feedback 90" % (_n + 1),
        _fd(FB, {128 + 84: _n, 128 + 85: 80, 128 + 86: 40, 128 + 87: 70, 128 + 88: 50, 128 + 89: 96, 128 + 90: 30}),
        KR8, HALF)
add("fx_ph_wet", "phaser, 6 stages, mix 127, feedback 20", _fd(FB, {128 + 84: 5, 128 + 85: 127, 128 + 86: 90,
    128 + 87: 127, 128 + 88: 20, 128 + 89: 20, 128 + 90: 127}), KR8, HALF)
for _t in range(1, 12):
    add("fx_dist%d" % _t, "distortion type %d, intensity 90" % _t, _fd(FB, {128 + 100: _t, 128 + 101: 90}), KR8, HALF)

# the reverb's own cases (package P5; nulltest.py --cases rv): a saw chord with noise, short notes, the reverb alone
# on the FX send (send 127), each in REVERB HALF and FULL, a long tail after the notes (rv_seq). Rooms 0-3, decays
# 12 / 64 / 120 / 127 (held), colours 0 / 30 / 64 / 127, pre-delays, modes 3 and 4 with feedback
RVCASES = {}
RV_BASE = {"OSC_BAL": 64, "OSC2_DET": 40, "NOISE": 60, "AE_A": 0, "AE_D": 60, "AE_S": 90, "AE_R": 20,
           "FX_MODE": 2, "FX_SEND": 127, "FX_TIME": 8, "FX_FB": 0, "FX_DECAY": 64, "FX_DEPTH": 3, "FX_COLOUR": 64}
RV_SET = [("r0", {"FX_DEPTH": 0}), ("r1", {"FX_DEPTH": 1}), ("r2", {"FX_DEPTH": 2}), ("r3", {}),
          ("d12", {"FX_DECAY": 12}), ("d120", {"FX_DECAY": 120}), ("d127", {"FX_DECAY": 127, "FX_DEPTH": 1}),
          ("c0", {"FX_COLOUR": 0, "FX_DEPTH": 2}), ("c30", {"FX_COLOUR": 30}), ("c127", {"FX_COLOUR": 127, "FX_DEPTH": 2}),
          ("pd0", {"FX_TIME": 0, "FX_DEPTH": 1}), ("pd100", {"FX_TIME": 100, "FX_DEPTH": 0}),
          ("m3", {"FX_MODE": 3, "FX_FB": 80, "FX_TIME": 30, "FX_DEPTH": 2}),
          ("m4", {"FX_MODE": 4, "FX_FB": 100, "FX_TIME": 20, "FX_DECAY": 100}),
          # (after 1.18C37) a dull, quiet sound below the LF path's split: osc 1 a sine, no noise, the filter nearly closed,
          # a long decay (the LF path's interpolation images, RVLF_SPLINE: rvlf_images.py)
          ("lfs", {"OSC1_SHAPE": 0, "OSC_BAL": 0, "NOISE": 0, "CUT1": 40, "FX_DECAY": 96, "FX_SEND": 80})]
for _k, _s in RV_SET:
    for _v, _e in (("h", HALF), ("f", FULL)):
        _c = Case("rv_%s_%s" % (_k, _v), "reverb case %s, %s" % (_k, "HALF" if _v == "h" else "FULL"),
                  dict(RV_BASE, **_s), [48, 55, 60, 64], _e)
        _c.seq = ["on", "48", "110", "on", "55", "100", "on", "60", "100", "on", "64", "90", "run", "30", "off", "48",
                  "off", "55", "off", "60", "off", "64", "run", "60", "on", "36", "120", "run", "4", "off", "36",
                  "run", "520"]
        RVCASES[_c.name] = _c


def rv_cases():
    return list(RVCASES.values())


BANK_NOTES = [48, 55, 60, 64]


def bank_cases():
    """the 32 test-bank patches as cases (read from $TESTBANK; not part of the repository)"""
    out = []
    d = Path(TESTBANK)
    if d.is_dir():
        for f in sorted(d.glob("*.raw")):
            c = Case("bank_" + f.stem, "test-bank patch " + f.stem, {}, BANK_NOTES, HALF)
            c.raw = f.read_bytes()
            c.patch = (lambda c=c: c.raw[:256])
            out.append(c)
    return out


def events(c, blocks):
    """the engine's argument list: the notes on, `blocks` blocks of 128 frames"""
    ev = []
    for n in c.notes:
        ev += ["on", str(n), str(c.vel)]
    return ev + ["run", str(blocks)]


def seq_events(c):
    """the null test's note sequence: a single note, then the case's chord (8 notes: the case's own, or CH8); a case's
    own sequence (c.seq: the reverb's cases) instead when it has one"""
    if getattr(c, "seq", None):
        return list(c.seq)
    ev = ["on", "60", "100", "run", "24", "off", "60", "run", "16"]
    for n in (c.notes if len(c.notes) >= 8 else CH8):
        ev += ["on", str(n), "100"]
    ev += ["run", "40"]
    for n in (c.notes if len(c.notes) >= 8 else CH8):
        ev += ["off", str(n)]
    return ev + ["run", "24"]


def all_cases(bank=True):
    return list(CASES.values()) + (bank_cases() if bank else [])


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "--list":
        for c in all_cases():
            print("%-14s %s  notes=%s env=%s" % (c.name, c.doc, c.notes, c.env))
    elif len(sys.argv) >= 3 and sys.argv[1] == "--write":
        out = Path(sys.argv[2]); out.mkdir(parents=True, exist_ok=True)
        with open(out / "cases.tsv", "w") as t:
            for c in all_cases():
                (out / (c.name + ".raw")).write_bytes(c.patch())
                t.write("%s\t%s\t%s\n" % (c.name, ",".join("%s=%s" % kv for kv in c.env.items()), " ".join(map(str, c.notes))))
    else:
        print(__doc__)
