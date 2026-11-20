# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
# fdiff.py SHORT.d LONG.d DIS [N]: per-function instructions per block between two runs (the steady part), and ISR rate
import re, collections, sys
def hist(p):
    c = collections.Counter()
    for l in open(p + '/h.hist'):
        m = re.match(r'\s*(?:0x)?([0-9a-fA-F]+)\s+(\d+)', l)
        if m: c[int(m.group(1), 16)] += int(m.group(2))
    return c
def run(p):
    t = open(p + '/log.txt').read()
    r = float(re.search(r'\(([\d.]+) M ISR instructions per emulated s\)', t).group(1)); ms = int(re.search(r'time (\d+) ms', t).group(1))
    pc = float(re.search(r'cpu0: ([\d.]+)% in an ISR', t).group(1))
    return r * ms / 1000, ms, pc
a, b, dis = sys.argv[1], sys.argv[2], sys.argv[3]
N = int(sys.argv[4]) if len(sys.argv) > 4 else 25
ra, rb = run(a), run(b)
dt = (rb[1] - ra[1]) / 1000.
blocks = dt * 46875 / 128
ha, hb = hist(a), hist(b)
fn = {}; cur = None
for l in open(dis, errors='replace'):
    m = re.match(r'^([0-9a-f]+) <([^>]+)>:\s*$', l)
    if m: cur = m.group(2); continue
    m = re.match(r'^([A-Za-z_.$][\w.$]*):\s*$', l)
    if m:
        if not m.group(1).startswith('.'): cur = m.group(1)
        continue
    m = re.match(r'^\s*([0-9a-f]+):\s', l)
    if m and cur: fn[int(m.group(1), 16)] = cur
tot = collections.Counter()
for k in set(ha) | set(hb):
    d = hb.get(k, 0) - ha.get(k, 0)
    tot[fn.get(k, '?')] += d
print('ISR steady %.2f M instr/s = %.0f k/block (cpu0 in ISR %.1f%% / %.1f%%)' % ((rb[0] - ra[0]) / dt, (rb[0] - ra[0]) * 1e3 / blocks, ra[2], rb[2]))
for f, n in tot.most_common(N): print('  %-28s %9.0f' % (f, n / blocks))
