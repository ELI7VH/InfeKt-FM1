#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
# cpu_case.sh FWSC DIS CASE OUT [MS_SHORT MS_LONG]: one case of the CPU benchmark (cpu_keys.sh) in fm1-emu, two runs
# (9 s and 12 s) and their difference per function (fdiff.py: the steady instructions a block) -> OUT.txt; the runs'
# folders OUT_s.d, OUT_l.d (log, PC histogram). EURO sends its patch over USB at 2.5 s: EURO_SYX (a single dump, e.g.
# a player's patch, kept outside the repository), else euro_like.py's own. FM1_EMU: the emulator (default fm1-emu on
# the PATH). FM1_CPU_HZ (an emulator with the local rate hook): the core's instruction rate, e.g. 128000000 for the
# device's ~2.8 cycles an instruction at 360 MHz (the load, the dropouts and the governor then as on the device)
H=$(cd "$(dirname "$0")" && pwd)
ab() { echo "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; }
F=$(ab "$1"); DIS=$(ab "$2"); C=$3; O=$(ab "$4"); S=${5:-9000}; L=${6:-12000}
. "$H/cpu_keys.sh"
eval K=\$$C
U=
if [ "$C" = EURO ]; then
    X=$(mktemp)
    if [ -n "$EURO_SYX" ]; then python3 -c "import sys; d=bytearray(open(sys.argv[1],'rb').read()); d[-2]=sum(d[5:-2])&0x7f; print(bytes(d).hex())" "$EURO_SYX" > "$X"
    else python3 "$H/euro_like.py" "$X" --hex; fi
    U="2500:$(cat "$X")"; rm -f "$X"                     # (the dump's checksum made right: the firmware checks it)
fi
run() { rm -rf "$1"; mkdir -p "$1/work"; (cd "$1" && FM1_USB_SEND="$U" FM1_PC_HIST="$1/h.hist" ${FM1_EMU:-fm1-emu} --fwsc "$F" \
    --ms "$2" --steps 900000000000 --keys "$K" > log.txt 2>&1; rm -rf work); }
run "$O"_s.d "$S" & run "$O"_l.d "$L" & wait
python3 "$H/fdiff.py" "$O"_s.d "$O"_l.d "$DIS" 40 > "$O.txt"
head -1 "$O.txt"
