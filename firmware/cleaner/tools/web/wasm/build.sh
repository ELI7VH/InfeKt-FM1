#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
# build.sh [OUT.wasm] [OPT]: the web synth (ws_engine.c and cleaner/engine) as a freestanding WebAssembly module
#. clang (wasm32 target) and wasm-ld; no emscripten, no libc, no libm (the engine has its own
# float math, sy_math.h); memcpy / memset are the bulk-memory instructions (-mbulk-memory). The engine's switches are
# the Bluetooth two-part firmware's (fm1.toml o2_cflags, build_multi_ble.sh) where they change the sound: HYB_MULTI,
# 16 voice slots, the 32768-frame delay, the unison caps, HYB_RV_LEND (the reverb's float ring, the shimmer); the
# firmware's code-placement switches (RAM sections, the second core, packed tables) are left out: same numbers.
# -ffp-contract=off as the host build (no fused multiply-add: the host's results). OPT: -O2 (default) or -Oz.
set -e
H=$(cd "$(dirname "$0")" && pwd)
E=${ENGINE_DIR:-$H/../../../engine}
OUT=${1:-$H/../../../build/ws_engine.wasm}
OPT=${2:-${WS_OPT:--O2}}
CC=${CLANG:-clang}
LD=${WASM_LD:-wasm-ld}
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT
FLAGS="--target=wasm32 -nostdlib -nostdlibinc -ffreestanding -mbulk-memory -std=c99 $OPT -ffp-contract=off
       -fno-exceptions -Wall -Wno-unused-parameter -Wno-unused-function -Wno-literal-conversion
       -DHYB_MULTI -DHYB_RV_LEND -DSY_MAX_VOICES=16 -DSY_DELAY_FRAMES=32768 -DSY_UNI_CAP -I$E"
for f in $H/ws_engine.c $E/synth.c $E/sy_voice.c $E/sy_tables.c $E/sy_fx.c $E/sy_waves.c $E/sy_dist_tab.c \
         $E/sy_arp_tab.c $E/sy_hyb_lfow.c $E/sy_cln_rv.c; do
    $CC $FLAGS -c "$f" -o "$D/$(basename "${f%.c}").o"
done
mkdir -p "$(dirname "$OUT")"
# the stack 64 KB (the engine's deepest chain is a few KB: fm1/stack_check.py); the memory as large as the statics
$LD --no-entry --strip-all --export-dynamic -z stack-size=65536 --initial-memory=4194304 -o "$OUT" "$D"/*.o
echo "$OUT: $(wc -c < "$OUT") B ($OPT)"
