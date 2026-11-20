#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
# rv_slot_check.py ELF: the reverb's swap slot (engine sy_cln_rv.c RV_SLOT, CPU plan section 15): the network's
# variants (rv_net_f, rv_net_h or rv_net_fh, rv_net_hf) are linked in the flash and run from rv_slot (.rt_spare), copied there by
# rv_slot_load. Checked on the build's disassembly:
#   - each variant fits the slot (its size <= rv_slot's);
#   - each is position-independent: every branch with a target ("goto", "call" to an address) lands inside the variant
#     itself; every call out goes through a register (no PC-relative call into another function); no other use of pc
#     but the return;
#   - every function whose address a variant loads (the targets of its register calls) is in RAM code (.ram_text), or
#     one of COLD (per block, the flash on purpose): the slot extends the RAM code's flash-off / RAM <-> flash check;
#   - rv_slot itself lies in the RAM code's region (.rt_spare) and is word-aligned;
#   - (1.18C36b) it lies inside the hardware's PC limit for RAM code, [_rt_start, _rt_pc_end) (build_variant.py
#     --rt-spare; felucca-bsp fm1_guard.h): 1.18C36 had it past _rt_end, outside the window the boot arms, so the
#     first block the reverb's network ran from it faulted on the device and the FM-1 restarted (the emulator does
#     not model the PC limit).
# Without a slot in the build (rv_slot absent: the other builds) it says so and passes.
import os, re, subprocess, sys
from pathlib import Path

VARIANTS = ("rv_net_f", "rv_net_h", "rv_net_hf")   # (1.18C41, RV_HALF_F: HALF's variant rv_net_fh, the float ring)
COLD = {"rv_mod_block"}                 # (sy_cln_rv.c RVCOLD: once a block, the flash's by design, as before the slot)

elf = sys.argv[1]
tc = Path(os.environ.get("JIELI_TOOLCHAIN", Path.home() / ".jieli/toolchain"))
od = str(tc / "common/bin/objdump")
syms = {}
for ln in subprocess.run([od, "-t", elf], capture_output=True, text=True, check=True).stdout.splitlines():
    m = re.match(r"^([0-9a-f]{8}) (.{7}) (\S+)\s+([0-9a-f]{8}) ?(.*)$", ln)
    if m:
        a, f, sect, z, name = m.groups()
        syms.setdefault(name.strip(), (int(a, 16), int(z, 16), sect, "F" in f))
if "rv_slot" not in syms:
    print("rv_slot_check: ok   no reverb slot in this build (RV_SLOT 0)")
    sys.exit(0)
slot_a, slot_n, slot_sect, _ = syms["rv_slot"]
fails = []
if slot_sect != ".rt_spare" or slot_a & 3:
    fails.append("rv_slot at %#x in %s (want .rt_spare, word-aligned)" % (slot_a, slot_sect))
if "_rt_pc_end" not in syms:
    fails.append("no _rt_pc_end: the PC limit ends at _rt_end, before rv_slot (build_variant.py --rt-spare)")
elif not (syms["_rt_start"][0] <= slot_a and slot_a + slot_n <= syms["_rt_pc_end"][0]):
    fails.append("rv_slot %#x..%#x outside the PC limit's RAM window [%#x, %#x)" % (slot_a, slot_a + slot_n,
                 syms["_rt_start"][0], syms["_rt_pc_end"][0]))
funcs = {a: (n, z, s) for n, (a, z, s, isf) in syms.items() if isf and z}
dis = subprocess.run([od, "-d", "-mcpu=r3", elf], capture_output=True, text=True, check=True).stdout.splitlines()   # (r3: its FPU)
report = []
if "rv_net_fh" in syms and "rv_net_h" not in syms:
    VARIANTS = ("rv_net_f", "rv_net_fh", "rv_net_hf")
for v in VARIANTS:
    if v not in syms:
        fails.append(v + " missing")
        continue
    a0, n, sect, _ = syms[v]
    if sect != ".text":
        fails.append("%s in %s (want the flash's .text)" % (v, sect))
    if n > slot_n:
        fails.append("%s %d B > the slot's %d B" % (v, n, slot_n))
    if a0 & 3:
        fails.append("%s at %#x not word-aligned" % (v, a0))
    loads, ncall = set(), 0
    for j, ln in enumerate(dis):
        m = re.match(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)$", ln)
        if not m:
            continue
        pc = int(m.group(1), 16)
        if not a0 <= pc < a0 + n:
            continue
        ins = m.group(3)
        if re.search(r"\bgoto -?\d+\s*$", ins) and j + 1 < len(dis):   # (a loop's branch: its target on the next line)
            ins += " " + dis[j + 1].strip()
        if re.search(r"\b(call|goto)\b", ins):
            t = re.search(r"<\S+ : ([0-9a-f]+) >", ins)
            if re.search(r"\bcall r\d+\b", ins):
                ncall += 1
            elif t:
                ta = int(t.group(1), 16)
                if not a0 <= ta < a0 + n:
                    fails.append("%s+%#x: a PC-relative branch out of it: %s" % (v, pc - a0, ins.strip()))
            else:
                fails.append("%s+%#x: a branch not understood: %s" % (v, pc - a0, ins.strip()))
        elif re.search(r"\bpc\b", ins) and not re.match(r"\{pc(, r\d+(-r\d+)?)*\} = \[sp\+\+\]", ins.strip()):
            fails.append("%s+%#x: pc used: %s" % (v, pc - a0, ins.strip()))
        k = re.match(r"r\d+ = (\d+)\b", ins.strip())   # (an address loaded: a function's, a call through a register)
        if k and int(k.group(1)) in funcs:
            loads.add(int(k.group(1)))
    out = []
    for la in sorted(loads):
        name, z, s = funcs[la]
        out.append(name)
        if s != ".ram_text" and name not in COLD:
            fails.append("%s calls %s in %s (the slot's calls: RAM code or COLD)" % (v, name, s))
    report.append("%s %d B (%d register calls: %s)" % (v, n, ncall, ", ".join(out)))
if fails:
    for f in fails:
        print("rv_slot_check: FAIL " + f)
    sys.exit(1)
print("rv_slot_check: ok   rv_slot %d B at %#x (.rt_spare, inside the PC limit to %#x); %s" % (slot_n, slot_a,
      syms["_rt_pc_end"][0], "; ".join(report)))
