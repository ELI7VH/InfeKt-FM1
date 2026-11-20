#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# hostprof.sh [OUTDIR] [options]: the host instruction proxy for the CPU work (package M1).
# Builds the cleaner's host engine like the firmware's engine units (-O2, -ffp-contract=off, SY_MAX_VOICES=16) plus -g
# for line info, and measures the instructions per function and per source line of one 128-frame block for the fixed
# case set of hostcases.py, under valgrind --tool=callgrind (x86-64 instructions: ratios to the device's, not the
# same numbers; see the development notes). The reverb is REVERB HALF (SY_RVQ=0) as on the device.
#   OUTDIR            default /dev/shm/hostprof (profiles as OUTDIR/NAME.json; the engine as OUTDIR/engine_prof)
#   CASES=main|bank|all|a,b,c   (default all: the main cases and the 32 test-bank patches; $TESTBANK = their folder)
#   CC=gcc  CFLAGS_EXTRA="-DNAME=0 ..."  JOBS=4  N1=20 N2=60 (blocks of the two runs; the bank: N1B=10 N2B=40)
#   ENGINE=path   skip the build and profile this binary (it must have been built with -g)
#   REPORT=0      only measure; the tables: hostprof.py report OUTDIR [--md] [--lines FN,FN --case NAME]
# Needs valgrind and python3. Another source tree: SRC=/path/to/cleaner.
set -e
H=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC:-$(cd "$H/../.." && pwd)}
OUT=${1:-/dev/shm/hostprof}
mkdir -p "$OUT"
command -v valgrind >/dev/null || { echo "hostprof.sh: valgrind is not installed (apt install valgrind)" >&2; exit 2; }
if [ -z "$ENGINE" ]; then
    ENGINE=$OUT/engine_prof
    cd "$SRC"
    ${CC:-gcc} -O2 -g -std=c99 -ffp-contract=off -DSY_MAX_VOICES=16 -Wall -Wextra -Wno-unused-parameter $CFLAGS_EXTRA -Iengine \
        -o "$ENGINE" host/lab_engine.c engine/synth.c engine/sy_voice.c engine/sy_tables.c engine/sy_fx.c engine/sy_waves.c \
        engine/sy_dist_tab.c engine/sy_arp_tab.c engine/sy_hyb_lfow.c engine/sy_cln_rv.c -lm
fi
python3 "$H/hostprof.py" run --engine "$ENGINE" --out "$OUT" --cases "${CASES:-all}" --jobs "${JOBS:-4}" \
    --n1 "${N1:-20}" --n2 "${N2:-60}" --n1b "${N1B:-10}" --n2b "${N2B:-40}" --src "$SRC"
[ "${REPORT:-1}" = 0 ] || python3 "$H/hostprof.py" report "$OUT" --src "$SRC"
