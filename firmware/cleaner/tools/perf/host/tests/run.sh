#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
# run.sh HOST_BINARY [OUTDIR]: the host regression scripts here against a host simulator built by ../mk.sh
# (multi: the USB build's); a script fails on the host's own failures (said, expect) or a "FAIL" line (hostcmd.c).
# A script whose first line is "# requires: shimmer" (0.89) is skipped in a build without it (the Bluetooth
# two-part build: hostcmd.c shimhas; "# requires: shimmer multi": the USB build alone); "# requires: hat" (0.98) a
# build with the sampled hat's table (hat/hat_tab.h or hat_tab4.h generated), "# requires: nohat" one without;
# "# requires: rand" (after 0.98) a build with PRESET's RANDOMIZE (the USB build); "# requires: shimbt" (1.04) the
# Bluetooth two-part build booted with Bluetooth on (its shimmer locked: hostcmd.c "SHIMBT yes"; booted with it off,
# mk.sh mble-lent, the shimmer's scripts run there too). The cleaner: "# requires: nofactory" a build without factory
# banks (mk.sh with CLEANER_HOST_NOFACT=1: hostcmd.c "NOFACT yes"), where it is the only kind of script run
H=$1; O=${2:-$(mktemp -d)}; D=$(dirname "$0"); r=0
mkdir -p "$O"
printf 'shimhas\n' > "$O/shimhas.in"
SHX=$("$H" "$O/shimhas.in" "$O" 2>&1); SH=$(echo "$SHX" | grep -c "SHIMMER yes"); MU=$(echo "$SHX" | grep -c "MULTI yes")
HT=$(echo "$SHX" | grep -c "HAT yes"); RZ=$(echo "$SHX" | grep -c "RAND yes"); SB=$(echo "$SHX" | grep -c "SHIMBT yes")
NF=$(echo "$SHX" | grep -c "NOFACT yes")
for t in "$D"/*.txt; do
    if [ "$NF" = 0 ] && head -1 "$t" | grep -q "^# requires: nofactory"; then echo "skip $(basename "$t") (factory banks)"; continue; fi
    if [ "$NF" = 1 ] && ! head -1 "$t" | grep -q "^# requires: nofactory"; then continue; fi
    if [ "$SH" = 0 ] && head -1 "$t" | grep -q "^# requires: shimmer"; then echo "skip $(basename "$t") (no shimmer)"; continue; fi
    if [ "$MU" = 0 ] && head -1 "$t" | grep -q "^# requires: .*multi"; then echo "skip $(basename "$t") (one part)"; continue; fi
    if [ "$HT" = 0 ] && head -1 "$t" | grep -q "^# requires: hat"; then echo "skip $(basename "$t") (no hat table)"; continue; fi
    if [ "$HT" = 1 ] && head -1 "$t" | grep -q "^# requires: nohat"; then echo "skip $(basename "$t") (a hat table)"; continue; fi
    if [ "$RZ" = 0 ] && head -1 "$t" | grep -q "^# requires: rand"; then echo "skip $(basename "$t") (no RANDOMIZE)"; continue; fi
    if [ "$SB" = 0 ] && head -1 "$t" | grep -q "^# requires: shimbt"; then echo "skip $(basename "$t") (not the Bluetooth two-part build with Bluetooth on)"; continue; fi
    out=$("$H" "$t" "$O" 2>&1); rc=$?
    if [ $rc -ne 0 ] || echo "$out" | grep -q "^FAIL"; then echo "FAIL $(basename "$t")"; echo "$out" | grep -E "FAIL|fail"; r=1
    else echo "  ok $(basename "$t")"; fi
done
exit $r
