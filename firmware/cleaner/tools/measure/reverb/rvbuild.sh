#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
# rvbuild.sh: the renderers rvlib.py measures, into $RV_SCR (default the scratchpad's rvm):
#   new_full   the cleaner's engine with its own reverb (one part, the float ring; SY_RVQ=0/1 HALF/FULL, SY_SHIM=...),
#              built with CLN_RV_TUNE (CLN_RV_TUNE=a,b,... in the environment: the design constants, fit_rv.py)
#   new_multi  the same as two parts (the 16-bit HALF ring; SY_NOLEND unset: Bluetooth's lent ring, FULL possible)
#   (RV_OLD / RV_OLD16 of rvlib.py, the target to match, are renderers built elsewhere: not part of this release)
set -e
R=$(cd "$(dirname "$0")/../../.." && pwd)
S=${RV_SCR:-/tmp/infekt/rvm}
mkdir -p "$S"
E="$R/engine"
SRC="$E/synth.c $E/sy_voice.c $E/sy_tables.c $E/sy_fx.c $E/sy_waves.c $E/sy_dist_tab.c $E/sy_arp_tab.c $E/sy_hyb_lfow.c $E/sy_cln_rv.c"
F="-O2 -std=c99 -w -ffp-contract=off -DCLN_RV_TUNE $CLN_XF -DSY_DELAY_FRAMES=32768 -I$E"
cc $F -o "$S/new_full" "$R/host/lab_engine.c" $SRC -lm
cc $F -DHYB_MULTI -DHYB_RV_LEND -o "$S/new_multi" "$R/host/lab_engine.c" $SRC -lm
echo "rvbuild: $S/new_full $S/new_multi"
