# InfeKt-FM1: source code

The source of the InfeKt-FM1 firmware for the M-VAVE FM-1, version 1.19.8: the synthesizer engine, the FM-1 app around
it (pages, sequencer, drum machine, launcher, bank store, Bluetooth build), the host renderer and test programs, and
the scripts that measured and fitted the engine's numbers.

Copyright (C) 2026 J0KERPACK. Licensed under the GNU General Public License version 3 only (`LICENSE`), as each
file's `SPDX-License-Identifier` says. Files that say otherwise keep their own licence: the web page
`firmware/cleaner/tools/web/infekt-fm1.html` and its tests are MPL-2.0 (they contain parts of fm1-base's installer;
`LICENSES/MPL-2.0.txt`); `firmware/cleaner/tools/perf/fm1-emu-pc-limit.patch` is a change to the MIT-licensed FM-1
emulator (`LICENSES/MIT-fm1-emu.txt`).

The GPL-licensed files come with an additional permission under GPLv3 section 7 for linking the JieLi AC79 SDK's
binary libraries (as used through fm1-base): `LICENSE-ADDITIONAL-PERMISSION.md`.

## Layout

    firmware/cleaner/engine/   the synthesizer engine (C, single-precision float, no allocation in the audio path)
    firmware/cleaner/fm1/      the FM-1 app and its build scripts (on fm1-base); fm1/hat/: the sampled hi-hat's tables
    firmware/cleaner/host/     the host renderer (build/engine) and test programs
    firmware/cleaner/tools/    measurement and fit scripts (measure/), performance and host-simulator tests (perf/),
                               the web page and its tests (web/), the hi-hat table generator (drums/), the INIT patch
    firmware/cleaner/PROVENANCE.md   where every part of the engine comes from, measured how
    firmware.json, docs/img/screen.png   the listing data for firmware hubs

## How the engine was made

The engine was made by measuring the reference instrument with audio and MIDI data: notes, controller movements
and parameter sweeps were played to it, test signals were sent through its filters and effects, and the resulting
audio was analysed (spectra, levels, impulse responses, fitted curves). The fitted numbers went into the engine's
tables. No code, tables or memory of another instrument are part of the engine. The scripts in
`firmware/cleaner/tools/measure/` reproduce each fit; each states its method at its top.

People were part of this, not only scripts. What numbers alone do not settle was fine-tuned by ear, in listening
comparisons with the reference: for example the oscillators' start phases, the modulation of the reverb and the
behaviour of the envelopes.

`PROVENANCE.md` lists the components one by one: what was designed (for example the oscillator waves, the reverb, the
band-limited oscillator), what was measured and fitted (filter laws, saturation and distortion curves, LFO rates, FM
index, the phase rules), and how close the result is to the target (null tests, level and band differences). The
targets measured during development (recordings and renders of the reference instrument and of an earlier
development engine) are not part of this release; where a part's measuring script is not included, PROVENANCE.md
says so.

## Building

You need: `make`, a C compiler and Python 3 with numpy (host renderer); for the firmware also the JieLi pi32v2
toolchain, the JieLi AC79 SDK files (Apache-2.0) and fm1-base with felucca-bsp (MPL-2.0) as used for 1.19.8, published
beside the binary as `infekt-fm1-1.19.8-fm1-base-src.tar.gz`. Unpack it as `firmware/fm1-base` (or set `FM1_BASE`); set
`JIELI_TOOLCHAIN` and `AC79_SDK` (fm1-base's `bsp/tools/get_sdk.sh` and its README say where they come from).

    make -C firmware/cleaner                                   # build/engine: the host renderer
    cd firmware/cleaner/fm1 && sh build_multi_ble.sh --release 1.19.8   # the public build: two parts, Bluetooth
    cd firmware/cleaner/fm1 && sh build_usb.sh                 # the USB build (two parts, no Bluetooth)

`unifdef` is not needed to build; the host-simulator tests (`tools/perf/host/mk.sh`, `tests/run.sh`) need fm1-base's
host build.

## Not included, and why

- Factory banks: the firmware has none (banks A and B stay empty; the player's six banks hold the patches).
- The development notes, the measured targets and their results, the test banks used during development, and the
  host tests that played them: private working material, or other people's sounds.

The firmware links JieLi's binary Bluetooth libraries from the AC79 SDK (Apache-2.0) through fm1-base; see
`LICENSE-ADDITIONAL-PERMISSION.md`.
