#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
# build_multi_ble.sh [--release X.Y[.Z][-suffix]]: two parts AND Bluetooth:
# the engine from the sources (make_engine.py), then fm1-base's tools/build.py through build_variant.py: the Bluetooth
# build with -DHYB_MULTI (the two parts; their reverb ring 16-bit, make_engine's default under HYB_MULTI; RAM code
# 92 KB, fm1.toml's 96 is the single build's),
# -DHYB_BANK_PACK (a smaller journal, app.c JR_N) and part B's block buffers in the pool (SY_MX_ATTR), its files cleaner-mble.* (the other builds'
# outputs left as they are); -DHYB_RV_LEND and --lend: Bluetooth off at power-on lends its memory to REVERB FULL's
# float ring (lend_check.py checks the map), -DHYB_RV_PACK the reverb's tables packed,
# -DHYB_ARP_PACK the arpeggiator's patterns without their empty tails, -DHYB_LFOW_PACK (0.86) the LFO waves in 11 bits a
# point (flash: make_engine.py 4b, 4c, 4d). 1.13: -DHYB_RT_SPARE and --rt-spare: RANDOMIZE's and the
# sampled hat's state (app.c HYB_SPARE, 432 B) in the RAM code region's unused end, its pool at the 8 KB spare
# (docs/drum-hat-sample.md "1.13").
# LEND_CFLAGS: more --cflag=... for a test build (-DHYB_TEST_BT_OFF, -DRVQ_HALF_DEF=0).
# The package is copied to build/infekt-fm1-cleaner-multi-ble.fwsc (a release:
# build/infekt-fm1-cleaner-multi-ble-X.Y.fwsc too).
#   FM1_BASE         fm1-base's checkout (default: fm1-base beside cleaner/, i.e. firmware/fm1-base)
#   JIELI_TOOLCHAIN  the toolchain (default ~/.jieli/toolchain); AC79_SDK the SDK (default /root/fw-AC79_AIoT_SDK)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
FM1_BASE=${FM1_BASE:-$(cd "$HERE/../.." && pwd)/fm1-base}
JIELI_TOOLCHAIN=${JIELI_TOOLCHAIN:-$HOME/.jieli/toolchain}
AC79_SDK=${AC79_SDK:-/root/fw-AC79_AIoT_SDK}
export FM1_BASE JIELI_TOOLCHAIN AC79_SDK
REL=
if [ "$1" = "--release" ]; then REL=$2; fi
python3 "$HERE/make_engine.py" >/dev/null
cd "$FM1_BASE"
python3 "$HERE/build_variant.py" --name CLEANER-MBLE --set ram_text_kb=92 --cflag=-DHYB_MULTI --cflag=-DHYB_BANK_PACK \
    '--cflag=-DSY_MX_ATTR=__attribute__((section(".pool")))' --cflag=-DHYB_RV_LEND --cflag=-DHYB_RV_PACK --cflag=-DHYB_ARP_PACK --cflag=-DHYB_LFOW_PACK --cflag=-DHYB_RT_SPARE $LEND_CFLAGS --lend --rt-spare --ns-group -- "$HERE" "$@"
python3 "$HERE/lend_check.py" "$HERE/build/cleaner-mble.elf"
python3 "$HERE/rv_slot_check.py" "$HERE/build/cleaner-mble.elf"   # (the reverb's swap slot: CPU plan section 15)
python3 "$HERE/stack_check.py" "$HERE/build/cleaner-mble.dis"   # (the second core's stack: 1.18C4)
if [ -n "$REL" ]; then
    cp "$HERE/build/cleaner-mble-$REL.fwsc" "$HERE/build/infekt-fm1-cleaner-multi-ble-$REL.fwsc"
    cp "$HERE/build/cleaner-mble-$REL.fwsc" "$HERE/build/infekt-fm1-cleaner-multi-ble.fwsc"
else
    cp "$HERE/build/cleaner-mble.fwsc" "$HERE/build/infekt-fm1-cleaner-multi-ble.fwsc"
fi
echo "copied   $HERE/build/infekt-fm1-cleaner-multi-ble.fwsc"
