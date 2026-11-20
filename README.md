# InfeKt-FM1

*A virtual analog synthesizer firmware for the M-VAVE FM-1, by J0KERPACK.* Version 1.19.

It was about time: the InfeKt-FM1 is finally released. It turns the small FM-1 into a full virtual analog
synthesizer whose sound comes very close to the original, and it is **compatible with Virus patches**.

What the InfeKt-FM1 cannot give you is the incomparable hands-on feel of the original. That is why I very much hope
the Virus will be available again in the future.

**Website, installer and guide: https://j0kerpack.github.io/InfeKt-FM1/**  
**Repository and downloads: https://github.com/j0kerpack/InfeKt-FM1**

The firmware's source code is in [`firmware/`](firmware/) under the GNU GPL version 3 with an additional permission
for the platform's binary libraries: see [`README-SOURCE.md`](README-SOURCE.md), [`LICENSE`](LICENSE) and
[`LICENSE-ADDITIONAL-PERMISSION.md`](LICENSE-ADDITIONAL-PERMISSION.md).

Runs on both FM-1 hardware versions, including the newer one (another flash chip, UC25HQ80; GLO > ABOUT shows
FLASH B36014). On those newer units the optional m-bootloader is not installed (the installer refuses it).

## Features

**Sound engine**
- Oscillators: three oscillators per voice, plus sub oscillator, noise with colour control and ring modulator.
  Oscillators 1 and 2 morph continuously from sine through saw to pulse with pulse width. Sync and FM in several modes.
- Wavetables: the built-in wavetables are original designs made for this project and can be replaced by the user.
- Filters: two multimode filters (low pass, high pass, band pass, band stop) in four routings from serial to
  parallel, two analog modes, saturation with several curves, keyfollow.
- Envelopes: filter and amplifier envelope.
- LFOs: three LFOs with sine, triangle, saw, square, sample & hold, sample & glide and the wavetables as LFO shapes.
- Modulation matrix: six slots with up to three destinations each, and a very large number of destinations.
- Voices: up to 16 voices, unison, mono and glide modes, two parts as layer or split.

**Effects**
- Distortion in several characters, chorus, phaser, equalizer
- Delay and reverb, also tempo-synced
- Particles Shimmer: octave layers, modulation and an adjustable tail

**Play and arrange**
- Arpeggiator
- Hybrid Sequencer: transposable from the keys, with optional finger assignment for steps and chord steps
- Drum machine with 11 instruments
- MIDI over USB, Bluetooth and the TRS jack
- Web editor and patch manager for your own banks

## Install

1. Open the website's Install page ([install.html](install.html) in this repository) in Chrome or Edge, or download
   the installer package and double-click `infekt-fm1.html` from it.
2. Connect the FM-1 with a USB data cable, click *Connect* and allow MIDI.
3. Back up your banks first if you already use InfeKt-FM1 (*Patches* tab, *Back up all banks*).
4. Keep or untick the two options (the factory banks into banks A and B, Sete's nine wavetables), click *Install* and
   leave the cable in until the page says it is done.

The way back to the original firmware is on the installer's first tab.

## Downloads

See the [downloads](downloads/) folder (also linked from the website's Home page):

| File | What |
| --- | --- |
| [`infekt-fm1_installer_1.19.zip`](downloads/infekt-fm1_installer_1.19.zip) | the installer package: firmware, installer page for offline use, quickstart PDFs, rescue tools, licences |
| [`infekt-fm1-1.19-multi-ble.fwsc`](downloads/infekt-fm1-1.19-multi-ble.fwsc) | the firmware alone |
| [`InfeKt-FM1_Quickstart_EN.pdf`](downloads/InfeKt-FM1_Quickstart_EN.pdf), [`InfeKt-FM1_Quickstart_DE.pdf`](downloads/InfeKt-FM1_Quickstart_DE.pdf) | the two-page quickstart (English, German) |
| [`LICENSES/`](downloads/LICENSES/README.txt), [`CREDITS.txt`](downloads/CREDITS.txt) | licences and credits |
| [`infekt-fm1-1.19-fm1-base-src.tar.gz`](downloads/infekt-fm1-1.19-fm1-base-src.tar.gz) | the source of the MPL-2.0 platform files (fm1-base, felucca-bsp) as built ([SOURCE.txt](downloads/SOURCE.txt)) |

The website itself: `index.html`, `guide.html`, `install.html` and `app.html` (the installer page) in this repository.

## Thank you

Special thanks go to three sound designers. With their sound designs a synth like this is particularly great fun:

- Manuel Schleis ([Vengeance Sound](https://www.vengeance-sound.com/)), known worldwide for his legendary soundsets,
  producer sound packs and VST plug-ins
- [Alan Marcero](https://alanmarcero.com/), sound designer and trance producer
- Sete, sound designer

## Support

Want to support this project? Want more synths? An SMK-37 port? → **https://ko-fi.com/j0kerpack**

## Credits and licences

- **fm1-base** (Charles Vestal, with code by Leo Kuroshita and Chris Farrell), <https://github.com/charlesvestal/fm1-base>,
  and **felucca-bsp** (Leo Kuroshita), <https://github.com/hugelton/felucca-bsp>: the firmware's platform layer and the
  web installer's core, under the Mozilla Public License 2.0. Their source as used in this build, with the changes made
  to them: [`infekt-fm1-1.19-fm1-base-src.tar.gz`](downloads/infekt-fm1-1.19-fm1-base-src.tar.gz).
- **JieLi AC79 SDK** parts and Bluetooth libraries (Zhuhai Jieli Technology Co., Ltd.), Apache License 2.0
  ([text](downloads/LICENSES/Apache-2.0.txt)).
- **m-bootloader 1.2** by Julius Vitkauskas, <https://github.com/jvitkauskas/m-bootloader>, MIT licence
  ([text](downloads/LICENSES/MIT-m-bootloader.txt)).
- Display font: **Barlow Semi Condensed** (The Barlow Project Authors), SIL Open Font License 1.1
  ([text](downloads/LICENSES/OFL-1.1-Barlow.txt)); the only font in the firmware.
- Details: [CREDITS.txt](downloads/CREDITS.txt) and [LICENSES](downloads/LICENSES/README.txt).

Not affiliated with or endorsed by the manufacturer of the original; all trademarks belong to their owners.
