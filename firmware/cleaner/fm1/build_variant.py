#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
# build_variant.py [--name NAME] [--set KEY=VALUE ...] [--cflag FLAG ...] -- APP_DIR [build.py's arguments]
# fm1-base's tools/build.py (run from FM1_BASE, as build_usb.sh does) with fm1.toml's keys changed for this build only:
#   --name NAME      the build's file names (fm1.toml name: NAME.bin / .elf / .dis / .fwsc, lower case), so a variant
#                    does not overwrite another's outputs
#   --set KEY=VALUE  an fm1.toml number (ram_text_kb, ram_kb, ...)
#   --cflag FLAG     more flags for every unit (the app's and the o2 units), as no_ble_cflags gives the --no-ble build;
#                    given, they replace no_ble_cflags (in either build)
#   --lend           the memory map for Bluetooth's memory lent to the reverb: .bss with the
#                    common symbols first (the radio's .bss then the top of RAM, nothing of the app's among it), the
#                    pool with the radio's tasks' stacks first (the .pool of the link-time object: not the app's), then
#                    .pool.lend (the app's SH), then the rest; lend_check.py checks the result
#   --rt-spare       (1.13) a NOLOAD section .rt_spare right after the RAM code, in its region's unused end
#                    (_rts_start.._rts_end): zero-initialised state the pool has no room for (app.c HYB_RT_SPARE,
#                    zeroed by app_init); the linker fails it when the RAM code and it outgrow ram_text_kb.
#                    (1.18C36b) _rt_pc_end = _rts_end: the hardware's PC limit (felucca-bsp fm1_guard.h) covers it
#                    too, so code copied into it at run time may run (the reverb's slot, rv_slot_check.py); without
#                    it a fetch there faults on the device (either core) and the FM-1 restarts (the emulator does not
#                    model the PC limit)
#   --ns-group       (1.18C37) the engine's note-start code (synth.c SY_NSL, section .sy_ns_const) as one block among the
#                    late code, its bounds _sy_ns_start .. _sy_ns_end (the app's NS_WARM reads it through); and
#                    -DSY_NS_GROUP for every unit
# fm1-base itself is not changed: its module is loaded and these of its functions wrapped (read_conf, no_ble_flags, tc).
import argparse
import importlib.util
import os
import sys
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("--name")
ap.add_argument("--set", action="append", default=[])
ap.add_argument("--cflag", action="append", default=[])
ap.add_argument("--lend", action="store_true")
ap.add_argument("--rt-spare", action="store_true")
ap.add_argument("--ns-group", action="store_true")
ap.add_argument("rest", nargs=argparse.REMAINDER)
a = ap.parse_args()
rest = a.rest[1:] if a.rest[:1] == ["--"] else a.rest
base = Path(os.environ.get("FM1_BASE") or Path(__file__).resolve().parents[2] / "fm1-base")
spec = importlib.util.spec_from_file_location("fm1b_build", base / "tools" / "build.py")
b = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b)

over = {}
if a.name:
    over["name"] = a.name
for kv in a.set:
    k, v = kv.split("=", 1)
    over[k] = int(v) if v.isdigit() else v
read_conf = b.read_conf


def conf(path):
    c = read_conf(path)
    c.update(over)
    return c


b.read_conf = conf
if a.cflag:
    b.no_ble_flags = lambda: list(a.cflag)
if a.ns_group:
    if not a.cflag:
        raise SystemExit("build_variant: --ns-group with the build's --cflag list only")
    a.cflag.append("-DSY_NS_GROUP")
LD_SWAP = []
if a.lend:
    LD_SWAP += [("*(.bss*) *(.sbss*) *(COMMON) *(.*_bss) *(.bd_base)", "*(COMMON) *(.bss*) *(.sbss*) *(.*_bss) *(.bd_base)"),
                ("_pool_start = .; *(.pool*)", "_pool_start = .; *(EXCLUDE_FILE(*/fm1/build/*) .pool) *(.pool.lend) *(.pool*)")]
if a.rt_spare:
    LD_SWAP += [("_rt_load = LOADADDR(.ram_text);", "_rt_load = LOADADDR(.ram_text);\n    .rt_spare (NOLOAD) : { . = ALIGN(4); "
                 "_rts_start = .; *(.rt_spare*) . = ALIGN(4); _rts_end = .; } > RAMTEXT\n    _rt_pc_end = _rts_end;")]
if a.ns_group:
    LD_SWAP += [("              *(.*_const)", "              . = ALIGN(16); _sy_ns_start = .; *(.sy_ns_const) . = ALIGN(16); "
                 "_sy_ns_end = .;\n              *(.*_const)")]
if LD_SWAP:
    tc = b.tc

    def tc_lend(tool, *args):
        if tool == "pi32v2/bin/ld":
            args = list(args)
            i = args.index("-T") + 1
            src = Path(args[i])
            src = src if src.is_absolute() else b.ROOT / src
            if src.name not in ("app.ld", "app_rt.ld"):   # (the loader's link as it is)
                return tc(tool, *args)
            txt = src.read_text()
            for old, new in LD_SWAP:
                if old not in txt:
                    raise SystemExit(f"build_variant: {old!r} not in {src}")
                txt = txt.replace(old, new)
            dst = Path(b.OUT) / ("app_lend.ld" if a.lend else "app_var.ld")
            dst.write_text(txt)
            args[i] = dst
        return tc(tool, *args)
    b.tc = tc_lend
sys.argv = [str(base / "tools" / "build.py"), *rest]
sys.exit(b.main())
