#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
# wdiff.py RUN.d DIS T0 T1 [T2 T3] [N] [RX]: instructions per block per function between the histograms at T0 and T1
# (h.hist.T); with T2 T3 a second (steady) window beside it, and the flash code run in the first window but not in the
# second (distinct bytes: the cold lines a chord's first blocks fetch)
# (1.18C37) RUN.d: an fm1-emu run made with FM1_PC_HIST=RUN.d/h.hist and FM1_PC_HIST_AT=T0,T1,.. (fm1-emu-pc-limit.patch);
# DIS: the build's disassembly. The onset analysis of cpu-plan.md section 16.2 (the chord's flash code not run in the
# steady blocks)
import re, sys, collections
d, dis = sys.argv[1], sys.argv[2]
ts = [int(x) for x in sys.argv[3:7] if x.isdigit()]
N = int(sys.argv[7]) if len(sys.argv) > 7 else 40
RX = sys.argv[8] if len(sys.argv) > 8 else None
def hist(t):
    c = collections.Counter()
    for l in open('%s/h.hist.%d' % (d, t)):
        a, n = l.split(); c[int(a, 16)] = int(n)
    return c
fn = {}; cur = None; size = collections.Counter()
for l in open(dis, errors='replace'):
    m = re.match(r'^([0-9a-f]+) <([^>]+)>:\s*$', l)
    if m: cur = m.group(2); continue
    m = re.match(r'^([A-Za-z_.$][\w.$]*):\s*$', l)
    if m:
        if not m.group(1).startswith('.'): cur = m.group(1)
        continue
    m = re.match(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)', l)
    if m and cur:
        a = int(m.group(1), 16); fn[a] = cur; size[a] = len(m.group(2).split())
def win(t0, t1):
    a, b = hist(t0), hist(t1)
    blocks = (t1 - t0) / 1000. * 46875 / 128
    w = collections.Counter({k: b[k] - a.get(k, 0) for k in b if b[k] - a.get(k, 0) > 0})
    f = collections.Counter()
    for k, v in w.items(): f[fn.get(k, '?')] += v
    return w, f, blocks
w1, f1, b1 = win(ts[0], ts[1])
if len(ts) == 4:
    w2, f2, b2 = win(ts[2], ts[3])
else:
    w2, f2, b2 = None, None, None
tot1 = sum(f1.values()) / b1
print('window %d-%d ms: %.1f blocks, %.0f instr/block%s' % (ts[0], ts[1], b1, tot1,
      '' if w2 is None else '; steady %d-%d: %.0f instr/block' % (ts[2], ts[3], sum(f2.values()) / b2)))
keys = sorted(f1, key=lambda k: -f1[k] / b1 + (0 if w2 is None else f2.get(k, 0) / b2))
for k in keys[:N]:
    if RX and not re.search(RX, k): continue
    a = f1[k] / b1
    s = '' if w2 is None else '%9.0f %+9.0f' % (f2.get(k, 0) / b2, a - f2.get(k, 0) / b2)
    print('  %-28s %9.0f %s' % (k, a, s))
if w2 is not None:
    new = [k for k in w1 if k >= 0x2000000 and k not in w2]
    byf = collections.Counter()
    for k in new: byf[fn.get(k, '?')] += size.get(k, 2)
    print('flash code run in the first window only: ~%d B (instruction bytes) in %d functions'
          % (sum(byf.values()), len(byf)))
    for k, v in byf.most_common(N): print('  %-28s %5d B' % (k, v))
