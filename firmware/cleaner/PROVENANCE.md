<!-- SPDX-License-Identifier: GPL-3.0-only; Copyright (C) 2026 J0KERPACK -->
# Where the engine's numbers come from

The engine reproduces the sound of a hardware synthesizer ("the reference", not named here). The rule: every number
in the engine is an own design choice, the result of a measurement of the reference with audio and MIDI data, or a
fine-tuning by ear. For a measurement an engine is an executable driven through the lab's command line
(`tools/measure/mlab.py`: patch, notes, MIDI in; a stereo WAV out), and its audio output is analysed. No code, table
or memory of the reference is used.

People were part of this, not only scripts. What numbers alone do not settle was fine-tuned by ear, in listening
comparisons with the reference: for example the oscillators' start phases, the modulation of the reverb and the
behaviour of the envelopes.

The targets of the comparisons are not part of this release, so the scripts below need one supplied to be run again.
"Null" is the RMS of
the difference of two renders against the signal; "band" a third-octave band level.

## 1. Parts of the engine

| Part | Origin | Method and script | Closeness to the target |
|---|---|---|---|
| Oscillator waves 2-63 (`sy_waves.c`) | own design from coarse band-energy targets per wave | - | - |
| User waves | the player's own (`fm1/waves.c`) | - | - |
| Osc 1 and 2 (`sy_cln_osc.inc`, `sy_cln_osc.h` generated) | own band-limited oscillator (minBLEP-style: the naive wave delayed, a residual per step and corner, the steps on a measured clock) | `tools/measure/osc/` (`clock.py`, `kernel.py`, `level.py`, `co_design.py`, `make_hdr.py`; checks `cmp_osc.py`, `v6.py`, `v6load.py`) | not sample-exact (another structure); compared per wave and key by line levels |
| Saw-to-square morph (shape >= 64) | c = 0.75 u / (1 - 0.25 u), u = (shape - 63.5) / 63.5 | `meas_shape.py` (2nd harmonic against the fundamental), `fit_shape.py` (a 0.75002, b 0.24998) | null -129 dB or better |
| FM index, modes 0-2 (`sy_cln_fm.h`, generated) | nodes 4 steps apart, straight between (`fm_law`) | `fm_index.py`: a sine modulating a sine, 3 pitch ratios x 3 notes per value, the index from a Bessel fit of the lines; `fm_index.json` | `fm_cmp.py` (1152 renders): level 0.0001 dB max, lines above -40 dB within 0.006 dB, null median -110 to -127 dB |
| FM mode 4's index (`T_FM_I`) | measured points (the 2nd harmonic of two sines per FM amount) | measured during development; the script is not part of this release | - |
| Filter 1 frequency and damping (`sy_cln_filt.h`) | the cutoff law times a measured correction (1/100 cent); damping at resonance 0, 64, 96, 112, 120, 124, 127, linear between | `filt_fit.py` (periodic noise through the static path, the responses' ratio fitted with a state-variable low pass), `fit_filt.py`, `filt_top.py`, `filt_top2.py`; checks `filt_ring.py`, `filt_dyn.py`, `filt_knobs.py` | 215 cases: null median -95.9 dB, level 0.005 dB; one case above 1 % (cutoff 126 at resonance 127: 1.15 %); impulse ringing: peak within 0.037 dB, decay within 0.78 % |
| Filter balance, filter 1 modes 4 and 6 (`BAL_LAW`, `F1_STALE`, `F1M6_DIST`) | own laws for the measured behaviour | `bal_law.py`, `f1stale.py` | - |
| Ring modulator and the sub in it (`RING_SUB`, `sy_hyb_ring.h`) | measured per value | the sub's lines per balance, ring and osc volume; the tables measured during development (script not part of this release) | - |
| Saturation 5 (`sy_cln_sat5.h`) | slope at 33 drives, limit at 9, linear between | `meas_sat5.py` (a ramp through the static path), `fit_sat5.py` | null <= -66 dB, level 0.002 dB |
| Saturation 6-14, the analog loop (`sy_hyb_ladsat.h`, `sy_hyb_ladst.h`) | measured curves | measured during development; the script is not part of this release | - |
| Distortion 1-4 (`sy_cln_d14.h`, `sy_cln_d14_ref.h` the default) | per type and half one shape S, y = A S(k \|x\|), per intensity node A and k | `meas_dist14.py` (a ramp into the effects), `fit_dist14.py` (alternating least squares) | static curves within 0.05-0.19 % of the peak against the reference |
| Distortion 5-7, 10-11 | measured laws | measured during development; the script is not part of this release | - |
| Distortion 8 (quantiser), 9 (rate reducer) | steps / phase step linear between intensity nodes | `meas_dist89.py`, `fit_dist89.py` | steps within 6e-5 (null median -128 dB); 9: within 3e-5, spectra within 0.5 dB |
| Drums' DRIVE | the engine's distortion curves 1-4 | as distortion 1-4 | - |
| Phase hand-over, first note's phases, sub offset | own code: a table of 16 released voices' phases with a stamp, else an own xorshift32 generator; `CLN_PH_FIRST1..3`, `SUB_C` = 3/4 cycle | `ph_meas.py`, `ph_order.py`, `ph_null.py` (a parabola search on the null) | first note -134 dB, sub -150 dB |
| LFO rates (`LFO_RATE_HZ`, `sy_tables.c`) | measured per value | `lfo_rate.py` (LFO 1 on osc 1's pitch, the log frequency fitted with a triangle; `lfo_rate.json`) | every value within 0.1 % |
| LFO start phases (`LFO1_PH0`, `LFO2_PH0`, `CLN_LFO_PH`) | measured | `lfo_null.py`, `lfo_meas.py` | -90 / -121 dB; pan curves within 0.011 |
| LFO waves 6-67 (`sy_hyb_lfow.c`) | a measured table, one cycle in 64 points, read from renders | `lfo_wave.py` re-measures the waves on black-box renders; it does not reproduce the table (errors up to 0.47 of full scale) | - |
| Arpeggiator patterns 1-61 (`sy_arp_tab.c`) | measured patterns: per note the start and the gate | measured during development; the script is not part of this release | - |
| Envelopes, matrix, unison, voice stealing, chorus, phaser, delay, EQ, start transient (`sy_hyb_click.h`), BLEP for osc 3 / sync / FM (`sy_hyb_blepx.h`), volume (`sy_hyb_vol.h`) | measured laws and curves | measured during development; the scripts are not part of this release | - |
| Reverb, modes 2-4 (`sy_cln_rv.c`, `sy_cln_rvlf.h`) | own design: pre-delay with a feedback echo, four allpass diffusers, a figure-eight tank of plain delays read through a triangle LFO, damping, decay gain; HALF between half-band FIRs; a low-frequency mode bank | `tools/measure/reverb/`: `rv_struct.py`, `modtrack2.py`, `fit_rv.py`, `apply_fit.py`, `fit_rvlf.py` (the low modes fitted to the reference's measured response); checks `meas_rv.py`, `cmp_rv.py`, `perc_rv.py`, `wobble.py`, `glide.py` | not sample-exact (another structure); compared by room metrics (decay per band, band levels, modulation) |
| Shimmer (FX > SHIMMER) | own design on the own reverb | `fit_shim.py` (the octave bands 1-8 kHz of a noise burst's shimmer over 8 s) | - |
| Sustain pedal (CC 64; `synth.c` `sustain`, `rel_voices`) | own code for the measured behaviour: a key's note-off under the pedal holds its voices until pedal up (oldest first); a re-struck key a second voice; mono's key stack, the arpeggiator's keys and unison held the same way; CC 123 releases the held notes, the pedal stays down; CC 64 as a matrix source at the same time. Deliberately different: CC 120 also releases them and forgets the pedal (the firmware's PANIC) | `pedal_meas.py` (MIDI in, audio out: whether a note sounds and its pitch, per window of 128 frames; the same script on `build/engine`) | every case the same but CC 120 (above); without CC 64 the engine is unchanged (bit-identical) |
| Drum kit's hi-hat tables (`fm1/hat/`) | the author's own material | `tools/drums/mk_hat.py`, `docs/drum-hat-sample.md` | - |
| INIT patch (`fm1/init_patch.h`) | own: every byte a named default | `tools/make_init.py` | - |

## 2. Whole patches

`bank_dist.py`: a test bank rendered 10 times per engine at different start times (the start phases are random in
both), per patch the mean level and the mean third-octave spectrum. Against the development engine: mean level
difference max 1.40 dB / median 0.018 dB (the development engine against itself, started a block later: 0.88 / 0.015
dB); no patch outside its statistical spread (t < 3). `cmp_bank.py` (one render each): median level difference 0.04
dB. `cmp_comp.py` measures the components above one by one.

## 3. Build and reproduce

    make                                   # build/engine, the lab's renderer
    cd fm1 && sh build_multi_ble.sh        # the Bluetooth two-part firmware; build_usb.sh: USB two parts
    cd tools/measure && python3 cmp_comp.py && python3 bank_dist.py --dev

The measurement builds come from `tools/measure/cbuild.py`; the targets are set in `mlab.py` and by the environment
variables above.
