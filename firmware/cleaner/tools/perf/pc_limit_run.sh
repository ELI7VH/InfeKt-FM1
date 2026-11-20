#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
# pc_limit_run.sh FWSC ELF OUT MS [USBSEND] [KEYS]: a run of FWSC in fm1-emu with the chip's PC limit checked (1.18C36c):
# FM1_PC_LIMIT=1 (every fetch of either core against the windows the firmware armed) and FM1_EXEC_WATCH over
# ELF's rv_slot (the slot's contents each time code enters it), then pc_limit_check.py on the log (OUT/log.txt).
# Needs an fm1-emu with those two hooks (the patch beside this script, fm1-emu-pc-limit.patch); FM1_EMU: the
# emulator (default fm1-emu on the PATH). The device faults on a fetch outside the limit; the emulator alone does not.
# (The patch also gives FM1_PC_HIST_AT=MS,MS,..: the PC histogram written at those times too, for a chord's first
# blocks against the steady ones in one run.)
H=$(cd "$(dirname "$0")" && pwd)
ab() { echo "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; }
F=$(ab "$1"); E=$(ab "$2"); O=$3; MS=$4
OD=/root/.jieli/toolchain/common/bin/objdump
SLOT=$($OD -t "$E" | awk '$NF=="rv_slot" {print $1, $(NF-1); exit}')
W=
if [ -n "$SLOT" ]; then W=$(python3 -c "import sys; a,n=(int(x,16) for x in sys.argv[1:3]); print('%x:%x'%(a,a+n))" $SLOT); fi
rm -rf "$O"; mkdir -p "$O/work"
(cd "$O" && FM1_PC_LIMIT=1 FM1_EXEC_WATCH="$W" FM1_USB_SEND="${5:-}" ${FM1_EMU:-fm1-emu} --fwsc "$F" --ms "$MS" \
    --steps 900000000000 --keys "${6:-}" > log.txt 2>&1; echo "rc=$?" >> log.txt; rm -rf work)
python3 "$H/pc_limit_check.py" "$E" "$O/log.txt"
