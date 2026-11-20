#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
# lend_check.py ELF: the Bluetooth two-part build's memory map for REVERB FULL's float ring in Bluetooth's memory
# (build_variant.py --lend). The ring, 128 KB, is [SH - 64 KB, SH + 64 KB): the top of RAM's
# .bss, the gap to the pool, the radio's tasks' stacks (first in the pool), then SH's delay / reverb memory (SH.sp,
# SH's first member). Checked: the stacks at the pool's start, SH right after them, and nothing in [SH - 64 KB, SH)
# but the link-time object's .bss (the radio's: JieLi's libraries, the shim, the MIDI service) and the stacks; the
# app's (bt_lend_init) checks the same at power-on, but for the .bss, which only this sees. 1.04: the shimmer's state
# lies in the HYB_RV_LEND_PRE bytes right below the ring (the engine's sy_cln_rv.h): the radio's .bss there too.
import os, re, subprocess, sys
from pathlib import Path

elf = sys.argv[1]
tc = Path(os.environ.get("JIELI_TOOLCHAIN", Path.home() / ".jieli/toolchain"))
out = subprocess.run([str(tc / "common/bin/objdump"), "-t", elf], capture_output=True, text=True, check=True).stdout
cur, syms, gl = None, [], {}
for ln in out.splitlines():
    m = re.match(r"^([0-9a-f]{8}) (.{7}) (\S+)\s+([0-9a-f]{8}) ?(.*)$", ln)
    if not m:
        continue
    a, f, sect, z, name = m.groups()
    name = name.strip()
    if "df" in f.split():
        cur = name                      # (the link-time object's symbols follow a file symbol with no name)
        continue
    a, z = int(a, 16), int(z, 16)
    if not z and sect == "*ABS*" or not z:
        gl.setdefault(name, a)
        if not z:
            continue
    syms.append((a, z, sect, name, cur if "l" in f[:2] else None, "g" in f[:2]))
    gl.setdefault(name, a)


def need(c, msg):
    if not c:
        sys.exit("lend_check: FAIL " + msg)


# the app's globals: those its objects define (the link-time object's globals are the radio's)
app = set()
for o in [*Path(elf).parent.glob("*.o"), *Path(elf).parent.glob("o2/*.o")]:
    r = subprocess.run([str(tc / "pi32v2/bin/nm"), str(o)], capture_output=True, text=True)
    app |= {l.split()[-1] for l in r.stdout.splitlines() if len(l.split()) == 3 and l.split()[1] not in "Uu"}
syms = [(a, z, sect, name, ("app" if name in app else "") if src is None else src, g) for a, z, sect, name, src, g in syms]
ps, be, bs = gl["_pool_start"], gl["_bss_end"], gl["_bss_start"]
stk = [s for s in syms if s[3] == "stacks" and s[2] == ".pool"]
sh = [s for s in syms if s[3] == "SH" and s[2] == ".pool"]
need(len(stk) == 1 and len(sh) == 1, "no stacks / SH in the pool")
stk, sh = stk[0], sh[0]
need(stk[0] == ps and stk[1] == 24576, f"the stacks at {stk[0]:#x} ({stk[1]} B), the pool from {ps:#x}")
need(sh[0] == ps + 24576, f"SH at {sh[0]:#x}, not right after the stacks")
lo = sh[0] - 65536
need(bs <= lo and be <= ps, "the ring's start below .bss")
# the app's symbols by name: globals defined in the app's objects (a global of the link-time object's has no file)
bad, lto_lo = [], None
for a, z, sect, name, src, g in syms:
    if a + z <= lo or a >= sh[0] or sect not in (".bss", ".data", ".pool", ".noinit", ".ram_text"):
        continue
    if name == "stacks" or (src == "" and sect == ".bss"):
        lto_lo = a if lto_lo is None else min(lto_lo, a)
        continue
    bad.append(f"{name} {a:#x} {z} {sect} {src}")
need(not bad, "not the radio's, in the lent memory: " + "; ".join(bad[:8]))
low = min(a for a, z, sect, name, src, g in syms if src == "" and sect == ".bss")
eng = Path(__file__).resolve().parents[2] / "build" / "cleaner_ble" / "engine"   # (the engine built: its reverb's header)
hdr = next((h for h in (eng / "sy_cln_rv.h",) if h.exists() and "HYB_RV_LEND_PRE" in h.read_text()), None)
m = re.search(r"#define HYB_RV_LEND_PRE (\d+)u", hdr.read_text()) if hdr else None
pre = int(m.group(1)) if m else 0
bad = [f"{name} {a:#x} {z} {sect} {src}" for a, z, sect, name, src, g in syms
       if a < lo and a + z > lo - pre and sect in (".bss", ".data", ".pool", ".noinit", ".ram_text") and src != ""]
need(lo - low >= pre and not bad, f"the shimmer's {pre} B below the ring: the radio's .bss from {low:#x}; " + "; ".join(bad[:8]))
print(f"lend_check: ok   the ring {lo:#x}..{sh[0] + 65536:#x}: .bss {lo:#x}..{be:#x} ({be - lo} B, the radio's from "
      f"{low:#x}: {lo - low} B to spare, the shimmer's state {pre} B of them), the stacks {ps:#x}, SH.sp {sh[0]:#x}")
