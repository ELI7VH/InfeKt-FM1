<!-- SPDX-License-Identifier: GPL-3.0-only; Copyright (C) 2026 J0KERPACK -->
# The sampled hi-hat (0.98)

A second model for the drum kit's hats: DRUM KIT > VOICE K2 **MODEL** on CH and OH, **SYN** (the synthesized hat,
`app.c` `dr_render`, as before and as shipped) or **SMP** (the user's own hi-hat sample: CH its closed hit, OH its open
one). the development notes 13.1 has the page; this file has the sound, the data and the measurements.

## The data: generated

The sample is the author's own recording; the WAV is not included, the two tables made from it are (`fm1/hat/`).

- `tools/drums/mk_hat.py WAV [--bits 8|4] [--out FILE] [--closed A:B] [--open A:B]` (committed) makes the table, a C
  header: `fm1/hat/hat_tab.h` (8 bit) and `fm1/hat/hat_tab4.h` (`--bits 4`), both in `fm1/hat/`.
  The default segments are the user's file's: the closed hit 0..0.2525 s, the open one from 0.2525 s to the end.
- `app.c` includes the table with `__has_include`: **without it nothing of the model is built** (MODEL shows "--" on
  the hats as before, a kit's MODEL bit there plays SYN; the image is the one without the model, the same bytes but
  the build's source hash). So a checkout without the table builds and runs as before.
- Which table a build takes (`app.c`, before `HAT_HAVE`): the USB build the 8-bit one; the Bluetooth build the 4-bit
  one (`HYB_HAT_SMALL`; the 8-bit one does not fit its flash: 1564 B over); the Bluetooth two-part build the 8-bit
  one since 1.13 (none up to 1.12, `HYB_NO_HAT`: 3128 B were free then, the player's code alone is ~2.6 KB; below,
  "1.13"). `-DHYB_HAT_FULL` forces the 8-bit table, `-DHYB_NO_HAT` none.

## What is stored (per hit)

- **The low part**: the hit (DC removed: a 15 Hz high-pass) decimated to 22.05 kHz (`resample_poly`), pre-emphasised
  for the player (the linear interpolation at TUNE 0, a 3-tap triangle at 44.1 kHz, and its 4th-order low-pass at
  11.025 kHz; up to 0.48 x 22.05 kHz, +12 dB at most), so the played low part is flat to the original within 0.2 dB
  (1/3 octaves, 250 Hz .. 8 kHz). 8 bit: mu-law (mu 255, 127 steps a sign, a 128-entry table); SNR 38 dB. 4 bit: a
  code per sample, (2c - 15) / 15 x the block's scale, a scale byte (2^(-e/8)) per 16 samples (4.5 bits a sample);
  SNR 22 dB. (IMA ADPCM was tried first: 14.6 dB on this material, the noise audible in the weak bands.)
- **The envelopes** in frames of 66 stored samples (3.0 ms): the level of the hit above the split (10.55 kHz: a
  4th-order Butterworth high-pass, the prototype's 0.9 x 23.4 kHz / 2) and of the low part as decoded, the sample's
  noise floor (the median of its quietest 15 % of 5 ms frames) subtracted in power, so both reach zero; a byte a
  frame, 1/16 octave a step, 255 none.
- **The end**: where the 5 ms level falls to the floor + 6 dB (the closed hit: 0.135 s), or the segment's end (the
  open hit: 0.189 s, the recording cut there at -26 dB of its peak); an 8 ms raised-cosine fade to it.
- **The tail's figures**: the anchor (a frame: a hit that decays into its floor where its band's level is 30 dB under
  its peak, the closed hit at 84 ms; a cut hit its last frame before the fade), the slope before it (a fit over 14
  frames; for a hit that decays by itself the slope that reaches -60 dB where the hit ends), the two levels at the
  anchor, the -60 dB point; the peak's frame (DECAY below 100 % fades from there).

Sizes (this sample): the 8-bit table 7145 B of samples + 218 B of envelopes + 256 B mu-law table; the 4-bit one
3573 + 447 (scales) + 218 B.

## The player (`app.c` `hat_*`, one voice: the hats')

At a hit (`dr_hit` -> `hat_hit`, the audio interrupt): the filters' coefficients (bilinear, `sy_tanpi`), the rate,
the gains, DECAY's fade or tail; the sections' states are kept (CH cutting OH: no step in them). Per output frame
(`hat_render`): the low part's two samples around the position (16.16, the step TUNE's rate x 22050 / fs), linear
interpolation, two low-pass sections at 11.025 kHz x TUNE's factor (at most 0.45 fs); the drums' shared white noise
(`nz`) through two high-pass sections at 10.55 kHz x the factor, times the noise band's envelope (ramped from frame
to frame, the frames advancing with the position, so they follow TUNE). Per frame (`hat_frame`, every ~3 ms) the
envelopes' next values and, with a tail, its level. The noise's level: white noise of -1..1 has the power 1/3, the
high-pass passes 1 - 2 fc / fs of it (within 0.15 dB for a 4th-order Butterworth through the bilinear transform), so
the noise band has the stored RMS.

- **TUNE** (-12 .. +12, as the other instruments): the playback rate 2^(n/12); everything follows (the two bands,
  the split, the envelopes' time, the fade's and the tail's time).
- **DECAY** (x 2^((n - 12)/6), with the step's LENGTH and the accent's +15 % / +30 % as for every instrument):
  - **100 %**: the hit as stored, the noise band following its envelope, both ending with the sample (the
    prototype's v3); no extrapolation. CH: -45 dB at 0.115 s, the voice over at 0.135 s; OH: the cut at 0.19 s.
  - **below 100 %**: an exponential fade over both parts from the hit's peak on, to -60 dB in f / (1 - f) x the hit's
    length after its peak (25 %: CH 35 ms, OH 65 ms to -60 dB); the voice ends at -60 dB.
  - **above 100 %**: from the anchor on, noise in both bands (the noise band's own noise at the larger of its
    envelope and the tail; the low part: noise high-passed at 3 kHz x TUNE, then the low part's low-pass, for what its
    own level lacks, `sqrt(tail^2 - env^2)`) at the hit's level at the anchor, falling exponentially; it **always
    reaches -60 dB of the hit's peak**, then the voice ends: the closed hit in f x the time the hit itself takes from
    the anchor (so 112 % is a little longer than 100 %, no step), the open hit (cut) in (f - 1) x its own decay at the
    cut (-129 dB/s), so 100 % ends with the sample and 400 % rings to 1.0 s.
- **TONE**: the noise band's level x 0.5 .. 2 (2^(n/12)).
- **LEVEL, ATTACK, PAN, SEND**: as for every instrument (after the voice). The level: the samples at their own
  level x 0.75 (`HAT_G`): CH at its peak (5 ms RMS) +2 dB over the synthesized CH, OH the sample's own 7 dB under
  its CH (the synthesized OH is louder: LEVEL per instrument evens it out if wanted).
- **Choke**: CH and OH share the voice as before: either cuts the other.
- **The metal** (the six squares of the synthesized hats and the cymbal) is not computed for the sampled hat alone
  (only for CY or a synthesized hat sounding).

## Storage of MODEL (no format change)

MODEL is bit 0 of the instrument's MODEL / SEND byte (`dr_kp_t` `m909`), until 0.97 used by BD SD LT MT HT alone:
no release let it be set on CH or OH (`kit_knob` returned), RESET clears it. So every kit saved before reads SYN on
the hats; the kit's size (80 B), place, version (`DR_KV1`) and the one write stay; no extra flash write. A kit saved
with SMP and read by 0.97 or a build without the table plays SYN (`dr_hit` takes the bit for the hats only with the
table). Patches and banks do not hold the kit: nothing changes there.

## Measured

Host simulator renders (`hostcmd.c` `kitset`, `hit`; the scratchpad's `hihat/device_render/usb_8bit` and
`ble_4bit`, `render.txt`), 44.1 kHz, LEVEL 100, no accent. The times to -20 / -40 / -60 dB under the hit's peak
(5 ms RMS frames):

| hit | TUNE | DECAY 25 % | 100 % | 400 % |
| --- | --- | --- | --- | --- |
| closed | -5 | 0.015 / 0.035 / 0.045 s | 0.080 / 0.140 / 0.175 | 0.080 / 0.235 / 0.375 |
| closed | 0 | 0.015 / 0.025 / 0.035 | 0.060 / 0.105 / 0.135 | 0.060 / 0.180 / 0.285 |
| closed | +5 | 0.010 / 0.020 / 0.025 | 0.045 / 0.075 / 0.100 | 0.045 / 0.135 / 0.210 |
| open | -5 | 0.050 / 0.070 / 0.085 | 0.240 / 0.250 / 0.250 | 0.365 / 0.945 / 1.365 |
| open | 0 | 0.040 / 0.055 / 0.065 | 0.185 / 0.190 / 0.190 | 0.270 / 0.700 / 1.025 |
| open | +5 | 0.030 / 0.040 / 0.050 | 0.135 / 0.140 / 0.145 | 0.210 / 0.535 / 0.755 |

DECAY between (closed, TUNE 0, to -60 dB): 50 % 0.070 s, 89 % 0.125, 112 % 0.140, 141 % 0.155, 200 % 0.185; open:
50 % 0.130, 112 % 0.210, 141 % 0.290, 200 % 0.460.

Against the prototype's v3 (the 8-bit low part at 23.4 kHz plus the enveloped noise, rendered at 46.875 kHz), at
TUNE 0 / DECAY 100 %, the 8-bit (USB) build: the 5 ms envelope during the hit within 0.4 dB (closed, to 100 ms) and
1.7 dB (open, mean 0.8); v3 sits on the recording's floor after the closed hit (-40 dB of its peak, the DC and the
noise), the model goes to silence (gated), so -45 dB comes at 0.115 s against v3's never; the 1/3-octave spectra
(first 100 ms, re the 8 kHz band) within 1.0 dB (closed) and 2.4 dB (open) below 10 kHz; above it the model is 1.5 ..
3.3 dB brighter at 12.7 and 16 kHz (its noise band is flat to 22 kHz and carries the energy the original has above
the split; v3's ran to 23.4 kHz and its low part stopped at 11.7 kHz); against the original recording the model is
within 0.2 dB (250 Hz .. 8 kHz) and -1.4 / +1.6 dB at 12.7 / 16 kHz. Against the original played at the pitch
(resampled), each normalised to its power: TUNE 0 and +5 within 2.1 dB in every third (rms 0.4 .. 0.9 dB); TUNE -5
-2.4 / -3.5 dB at 8 kHz (the split there, 7.9 kHz) and +4.5 dB at 16 kHz (the noise goes on to 22 kHz where the
slowed original stops at 16.5 kHz: the prototype does the same).

The 4-bit (Bluetooth) build against the 8-bit one: the same envelope times (±5 ms), the 1/3-octave spectra within
1 dB (rms 0.2 .. 0.3 dB) at TUNE -5 / 0 / +5; the difference signal -22.5 dB of the hit (the 4-bit quantisation, in
the low part).

CPU (emulator, the Bluetooth build, the default beat on A031 as in the development notes, `-DHYB_TEST_HAT=1`: CH and
OH SMP): the first core's audio interrupt 26.1 % against 25.1 % with SYN (USB build 26.5 % / 25.5 %), idle 5.4 %
both. The sampled hat costs about 170 instructions an output frame while it sounds (`hat_render`, `hat_next`,
`hat_frame`: 14.7 M instructions over 85 k frames; the 8-bit decoder 170, the 4-bit one 173), the synthesized hat
about 60 (its loop and the metal): one sampled hat sounding all the time is ~3 % of the first core (~250 M
instructions a second at 100 %), the synthesized one ~1 %.

Flash and RAM (against main at fe2317e, without the model): Bluetooth 579704 B (+6788; 1860 B left in the app slot of
581564), USB 481604 B (+10112), Bluetooth two parts 578436 B (unchanged: not built); RAM .data + .bss unchanged in
all three; the pool +168 B (USB +172: the voice's state, `hs`).

## Tests

- `tools/perf/host/tests/drum_hat.txt` (`# requires: hat`): a kit as an earlier release wrote it reads SYN on the
  hats and plays the synthesized hat; MODEL turned on the page (SMP / SYN, the strip "CH SMP"), saved with the kit
  and back after a reboot; CH at 100 % sounding at 0.12 s, over by 0.24 s and silent after; OH at 100 % sounding at
  0.16 s, over by 0.28 s; CH cutting a long OH (silent 0.25 s after CH); OH at 400 % sounding at 0.8 s, over by
  1.6 s; CH at 25 % over by 70 ms; TUNE +5 shorter, -5 longer; a synthesized OH after a sampled CH.
- `drum_hat_none.txt` (`# requires: nohat`): without the table MODEL does not turn on the hats, a kit's bit plays
  SYN.
- The output to the bit with the hats on SYN (the old model, kit settings, pans, sends, a synth note): the host
  simulators of the three builds give the same hash as main at fe2317e (`ohis`). The engine (`engine`) is not
  touched: `chk.sh` 78/78, `mchk.sh` 78/78 (main) as before.
- `run.sh` skips a script by `# requires: hat` / `nohat` (`hostcmd.c` `shimhas` prints `HAT yes|no`).

## 1.13: the Bluetooth two-part build

The Bluetooth single build stays at 1.12 (no further releases); 1.13 ships the two two-part builds, and the
Bluetooth one gets what it lacked: the sampled hat and RANDOMIZE (`gui-spec.md`, PRESET's list).

- **Flash**: the 8-bit table (the 4-bit one would leave ~3.4 KB more; not needed). Image 577904 of 581564 B (1.12:
  566884; +11020: the hat ~10.1 KB, RANDOMIZE ~0.9 KB), 3660 B left in the app slot (the XIP window is the slot).
- **RAM**: the pool was at its 8 KB spare (298880 of 307200: 128 B above it) and .data + .bss 200 B under the
  region; the new state is 432 B (`hs` 172, `rz_org` 256, `rz_sx`, `rz_seed`, `rz_from`). Nothing of it is used only
  with Bluetooth on or only while its memory is lent, so it cannot share the radio's memory (the lent ring and the
  shimmer's state take what Bluetooth leaves; with Bluetooth on the radio needs all of it). It goes to `HYB_SPARE`:
  in this build a NOLOAD section `.rt_spare` right after the RAM code, in the RAM code region's unused end
  (`build_variant.py --rt-spare`, `-DHYB_RT_SPARE`; RAM code 92004 of 94208 B, so 2204 B were free there, now 1772),
  zeroed by `app_init` before anything runs (the boot clears .bss and the pool only); no write guard covers it.
  Elsewhere (USB, the single build, the host) `HYB_SPARE` is `PLAT_POOL` as before. The pool (298880 B) and
  .data + .bss (73528 B) are 1.12's, `lend_check` ok (4624 B of the radio's .bss below the ring, the shimmer's 1536
  of them), `input()` 0xbac (USB 0xb4c, single 0x107a) unchanged.
- **Tests**: host simulators (`mk.sh`, `tests/run.sh`): single 69 ok / 17 skipped (as 1.12), USB two parts 84 / 2,
  Bluetooth two parts 79 / 7 (1.12: 77 / 9: `drum_hat.txt`, `plist_randomize.txt`, `edit_journal_rand.txt` now run,
  `drum_hat_none.txt` skipped), the same lent 84 / 2 (1.12: 82 / 4); 0 failures. `drjit_sweep.py`: 0 late, 0
  dropped, 0 off the grid in all four. `chk.sh` 78/78 both engines, `mchk.sh` 78 / 73 (known): the engine is not
  touched.
- **CPU** (emulator, the first core's interrupt, steady 9..12 s, `perf/fdiff.py`; LAYER, A024 with SHIMMER AMOUNT 127
  on A, A013 on B, three notes, then PLAY: the default beat, CH and OH SMP by `-DHYB_TEST_HAT=1`; the Bluetooth
  two-part build with `HYB_TEST_BT_OFF`, i.e. lent, shimmer running):

  | build | first core's interrupt | a block | `hat_render` | `dr_render` | `rvs_block` |
  | --- | --- | --- | --- | --- | --- |
  | Bluetooth two parts, lent, SMP | 150.2 M / s (24.1 %) | 410 k | 12.4 k | 11.9 k | 4.73 k |
  | Bluetooth two parts, lent, SYN | 146.4 M / s (23.6 %) | 400 k | - | 14.4 k | 4.73 k |
  | USB two parts, SMP | 150.3 M / s (24.0 %) | 410 k | 12.4 k | 11.9 k | 4.71 k |

  The sampled hats cost the same as in the USB build (+2.6 % over SYN in this case). `hat_render` runs from the
  flash, as in the other builds (the emulator does not model cold lines).
- Open: the device test (Bluetooth on and off: RANDOMIZE, CH / OH SMP, SHIMMER with Bluetooth off).
