#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
# euro_like.py OUT.syx [--hex]: the CPU benchmark's EURO case as a patch of the project's own (no third-party data): the
# INIT patch (fm1/init_patch.h, every byte a named default) with the load of a big unison string pad played on full
# keys: UNISON 4 voices a note (byte 97 = 3), detune and spread 127, a slow attack and a long release (release tails
# when the keys are struck again), both oscillators, and the delay / reverb in a reverb mode (112 = 2) with send 86
# and a long decay (the reverb FULL / HALF / AUTO under the most voices). A single dump (edit buffer); --hex writes
# its bytes in hex on one line (fm1-emu's FM1_USB_SEND, the benchmark's emu.sh)
import re
import sys
from pathlib import Path

INIT = Path(__file__).resolve().parents[2] / "fm1" / "init_patch.h"
p = [int(x) for x in re.findall(r"\d+", INIT.read_text().split("{", 1)[1].split("}", 1)[0])]
assert len(p) == 256
SET = {
    33: 64,            # OSC_BAL: both oscillators
    36: 100,           # OSC_VOL
    26: 70,            # O2_DET
    59: 40, 60: 90, 61: 100, 63: 90,   # AE_A, AE_D, AE_S, AE_R: a slow attack, a long release
    97: 3, 98: 127, 99: 127,           # UNISON (4 voices), UNI_DET, UNI_SPREAD
    112: 2, 113: 86, 114: 64, 115: 72, 116: 104, 117: 3, 119: 49,   # a reverb mode: send, pre-delay, ..., decay, room, colour
}
for k, v in SET.items():
    p[k] = v
name = b"EuroLike  "
p[240:240 + len(name)] = list(name)
body = [0x00, 0x20, 0x33, 0x01, 0x00, 0x10, 0x00, 0x00] + p
cs = sum(body[4:]) & 0x7F
msg = bytes([0xF0] + body + [cs, 0xF7])
assert len(msg) == 267 and all(b < 128 for b in msg[1:-1])
if "--hex" in sys.argv:
    Path(sys.argv[1]).write_text(msg.hex())
else:
    Path(sys.argv[1]).write_bytes(msg)
