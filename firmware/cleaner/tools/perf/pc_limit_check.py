#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
# pc_limit_check.py ELF LOG: checks an fm1-emu run made with FM1_PC_LIMIT=1 and FM1_EXEC_WATCH=<rv_slot's range>
# (pc_limit_run.sh) against the build's map (1.18C36c: the device's restart at the first reverb block of 1.18C36):
#   - the boot arms the chip's PC limit, its RAM window ending at the map's _rt_pc_end (or _rt_end without one) and
#     no later write leaves it narrower (fm1_cpu1_start opens the windows while core 1 boots and puts them back);
#   - no fetch of either core outside the windows (the emulator's "pc-limit: VIOLATION": a fault on the device);
#   - every time code runs in rv_slot, the slot holds one of the variants (rv_net_f, rv_net_h / rv_net_fh, rv_net_hf: their bytes
#     in the image) and never its boot contents (zero): nothing runs the slot before rv_slot_load filled it.
# Without a slot in the build only the first two. Exit 1 on a failure.
import re
import subprocess
import sys

OD = "/root/.jieli/toolchain/common/bin/objdump"
OC = "/root/.jieli/toolchain/common/bin/objcopy"
elf, log = sys.argv[1], sys.argv[2]
syms = {}
for l in subprocess.run([OD, "-t", elf], capture_output=True, text=True, check=True).stdout.splitlines():
    p = l.split()
    if len(p) >= 4 and re.fullmatch("[0-9a-f]{8}", p[0]) and re.fullmatch("[0-9a-f]{8}", p[-2]):
        syms[p[-1]] = (int(p[0], 16), int(p[-2], 16))
txt = open(log, errors="replace").read()
fails = []
ram_end = syms.get("_rt_pc_end", syms["_rt_end"])[0]
writes = [(int(a, 16), int(v, 16), int(ms)) for a, v, ms in re.findall(r"pc-limit: write (0x[0-9a-f]+) = (0x[0-9a-f]+) at (\d+) ms", txt)]
h1 = [v for a, v, _ in writes if a == 0x1EEE388]
if not h1:
    fails.append("the PC limit is never armed (no write to PC_LIMIT1_H): run with FM1_PC_LIMIT=1, a patched fm1-emu")
else:
    if ram_end - 1 not in h1:
        fails.append("PC_LIMIT1_H never %#x (the RAM window's end, %#x): %s" % (ram_end - 1, ram_end, [hex(v) for v in h1]))
    if h1[-1] not in (ram_end - 1, 0xFFFFFFFF):
        fails.append("PC_LIMIT1_H left at %#x (want %#x)" % (h1[-1], ram_end - 1))
m = re.search(r"pc-limit: (\d+) violations; windows at the end \[(0x[0-9a-f]+), (0x[0-9a-f]+)\] \[(0x[0-9a-f]+), (0x[0-9a-f]+)\]", txt)
if not m:
    fails.append("no pc-limit summary in the log (the run did not finish?)")
else:
    if int(m.group(1)):
        fails.append("%s fetches outside the PC limit: %s" % (m.group(1), "; ".join(re.findall(r"pc-limit: VIOLATION ([^\n]*)", txt)[:4])))
    if int(m.group(5), 16) != ram_end - 1:
        fails.append("the RAM window at the end of the run ends at %s (want %#x)" % (m.group(5), ram_end - 1))
report = "PC_LIMIT1_H %s" % ", ".join(hex(v) for v in h1)
if "rv_slot" in syms:
    sa, sn = syms["rv_slot"]
    subprocess.run([OC, "-O", "binary", "-j", ".text", elf, log + ".text.bin"], check=True)
    hdr = subprocess.run([OD, "-h", elf], capture_output=True, text=True, check=True).stdout
    va = int(re.search(r"\.text\s+[0-9a-f]+\s+([0-9a-f]+)", hdr).group(1), 16)
    t = open(log + ".text.bin", "rb").read()
    var = {v: t[syms[v][0] - va:syms[v][0] - va + sn] for v in ("rv_net_f", "rv_net_h", "rv_net_fh", "rv_net_hf") if v in syms}
    seen = []
    for c, pc, ms, words in re.findall(r"exec-watch: c(\d) pc=(0x[0-9a-f]+) at (\d+) ms \[\d+\] entry \d+ hash [0-9a-f]+ words ([0-9a-f ]+)", txt):
        data = b"".join(int(w, 16).to_bytes(4, "little") for w in words.split())
        name = next((v for v, b in var.items() if b == data), None)
        if int(pc, 16) != sa:
            fails.append("code entered rv_slot at %s, not its start (c%s, %s ms)" % (pc, c, ms))
        if name is None:
            fails.append("rv_slot ran (c%s, %s ms) holding none of the variants (%s)" % (c, ms, "all zero: before its fill"
                         if not any(data) else "first words " + " ".join(words.split()[:4])))
        seen.append("%s c%s %s ms" % (name, c, ms))
    m2 = re.search(r"exec-watch: 0x[0-9a-f]+\.\.0x[0-9a-f]+ entered (\d+) times", txt)
    report += "; rv_slot entered %s times, its contents: %s" % (m2.group(1) if m2 else "?", ", ".join(seen) or "never run")
for f in fails:
    print("pc_limit_check: FAIL " + f)
if fails:
    sys.exit(1)
print("pc_limit_check: ok   %s (window end %#x); no fetch outside the limit; %s" % (report.split(";")[0], ram_end,
      report.split("; ", 1)[1] if ";" in report else "no slot"))
