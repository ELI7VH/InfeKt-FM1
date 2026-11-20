#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
# build_usb.sh [--release X.Y[.Z][-suffix]]: the USB build, without Bluetooth:
# the engine from the sources (make_engine.py), then fm1-base's tools/build.py --no-ble through
# build_variant.py (RAM code 92 KB, fm1.toml's 96 is the Bluetooth build's; fm1.toml's no_ble_cflags: two parts, the
# reverb ring float). The package
# is copied to build/infekt-fm1-cleaner-usb.fwsc (a release: build/infekt-fm1-cleaner-usb-X.Y.fwsc too). The Bluetooth build's outputs
# (build/cleaner.fwsc, .bin, .elf, .dis) are left as they are: this build writes cleaner-usb.* beside them.
#   FM1_BASE         fm1-base's checkout (default: fm1-base beside cleaner/, i.e. firmware/fm1-base)
#   JIELI_TOOLCHAIN  the toolchain (default ~/.jieli/toolchain); AC79_SDK the SDK (default /root/fw-AC79_AIoT_SDK)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
FM1_BASE=${FM1_BASE:-$(cd "$HERE/../.." && pwd)/fm1-base}
JIELI_TOOLCHAIN=${JIELI_TOOLCHAIN:-$HOME/.jieli/toolchain}
AC79_SDK=${AC79_SDK:-/root/fw-AC79_AIoT_SDK}
export JIELI_TOOLCHAIN AC79_SDK
REL=
if [ "$1" = "--release" ]; then REL=$2; fi
python3 "$HERE/make_engine.py" >/dev/null
cd "$FM1_BASE"
python3 "$HERE/build_variant.py" --set ram_text_kb=92 -- "$HERE" --no-ble "$@"   # (RAM code 92 KB: the pool keeps 4 KB)
python3 "$HERE/stack_check.py" "$HERE/build/cleaner-usb.dis"   # (the second core's stack: 1.18C4)
if [ -n "$REL" ]; then
    cp "$HERE/build/cleaner-usb-$REL.fwsc" "$HERE/build/infekt-fm1-cleaner-usb-$REL.fwsc"
    cp "$HERE/build/cleaner-usb-$REL.fwsc" "$HERE/build/infekt-fm1-cleaner-usb.fwsc"
else
    cp "$HERE/build/cleaner-usb.fwsc" "$HERE/build/infekt-fm1-cleaner-usb.fwsc"
fi
echo "copied   $HERE/build/infekt-fm1-cleaner-usb.fwsc"
