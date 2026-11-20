#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
# build_ble.sh [build.py's arguments, e.g. --release X.Y]: the Bluetooth build (the single one): the engine from
# the sources (make_engine.py), then fm1-base's
# tools/build.py. Its files: build/cleaner.* (a release: build/cleaner-X.Y.fwsc).
#   FM1_BASE         fm1-base's checkout (default: fm1-base beside cleaner/, i.e. firmware/fm1-base)
#   JIELI_TOOLCHAIN  the toolchain (default ~/.jieli/toolchain); AC79_SDK the SDK (default /root/fw-AC79_AIoT_SDK)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
FM1_BASE=${FM1_BASE:-$(cd "$HERE/../.." && pwd)/fm1-base}
JIELI_TOOLCHAIN=${JIELI_TOOLCHAIN:-$HOME/.jieli/toolchain}
AC79_SDK=${AC79_SDK:-/root/fw-AC79_AIoT_SDK}
export FM1_BASE JIELI_TOOLCHAIN AC79_SDK
python3 "$HERE/make_engine.py" >/dev/null
cd "$FM1_BASE"
python3 tools/build.py "$HERE" "$@"
python3 "$HERE/stack_check.py" "$HERE/build/cleaner.dis"   # (the second core's stack: 1.18C4)
