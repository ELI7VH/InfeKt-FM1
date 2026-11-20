#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
# stack_check.py DIS [ROOT ...]: the second core's deepest call chain from the disassembly (the build's .dis), against
# its stack: 2816 B with the trap 256 B above its bottom (fm1_cpu1.h), so a chain past 2560 B faults CPU1 and the FM-1
# restarts. Each function's frame is its pushed registers plus its "sp += -N"; its callees are its direct calls and
# every function whose address it loads (the calls by pointer, SY_FAR: an over-count, never an under-count). The
# roots: voices_job (the second core's job: the voices' share and the effects, sy_fx_render). Fails above LIMIT, the
# trap less a margin for the job's entry (fm1_cpu1_main's frames and an interrupt's: the emulator's mark is ~50 B below
# this count). 1.18C3 restarted at the first reverb block with signal: rv_run's buffers inlined into hyb_rv_run
# (2340 B), a chain of ~3.2 KB. After 1.18C30: the tail calls too (a goto to another function's start: the compiler's
# sibling call, the caller's frame popped or not, counted as a call: an over-count at worst); rvbal's rvlf_clear (2.2 KB
# of the bank's modes kept on the stack, reached by rv_states_clear's tail jump) was missed so: ~3 KB at the reverb's
# first block or a FULL / HALF switch on the second core.
# STACK_SKIP=f,g (1.18C6): edges into these functions left out, for a chain the emulator's own mark of the stack
# showed never runs that deep (the release builds set none)
import os
import re
import sys

LIMIT = 2560 - 512
SKIP = set(x for x in os.environ.get('STACK_SKIP', '').split(',') if x)
dis = sys.argv[1]
roots = sys.argv[2:] or ['voices_job']
cur, fr, calls = None, {}, {}
for line in open(dis, errors='replace'):
    m = re.match(r'^([A-Za-z_][\w.]*):\s*$', line)
    if m:
        cur = m.group(1)
        fr[cur] = [None, 0]
        calls[cur] = set()
        continue
    if cur is None:
        continue
    m = re.search(r'\[--sp\] = \{([^}]*)\}', line)
    if m and fr[cur][0] is None:
        n = 0
        for p in m.group(1).split(','):
            mm = re.match(r'r(\d+)-r(\d+)', p.strip())
            n += int(mm.group(1)) - int(mm.group(2)) + 1 if mm else 1
        fr[cur][0] = 4 * n
    m = re.search(r'sp \+= (-\d+)', line)
    if m and fr[cur][1] == 0:
        fr[cur][1] = -int(m.group(1))
    m = (re.search(r'call [-\d]+ <([\w.]+)', line) or re.search(r'= \d+ <([\w.]+) : ', line)
         or re.search(r'goto [-\d]+ <([\w.]+) : ', line))   # (a tail call: a jump to another function's start, its
                                                              # frame on top of this one's; after 1.18C30)
    if m:
        calls[cur].add(m.group(1))
memo = {}


def deep(f, path=()):
    if f in path or f not in fr:
        return 0, []
    if f in memo:
        return memo[f]
    own = (fr[f][0] or 0) + fr[f][1]
    best = (0, [])
    for c in calls[f]:
        if c in SKIP:
            continue
        d = deep(c, path + (f,))
        if d[0] > best[0]:
            best = d
    memo[f] = (own + best[0], ['%s %d' % (f, own)] + best[1])
    return memo[f]


bad = 0
for r in roots:
    if r not in fr:
        print('stack_check: FAIL %s not in %s' % (r, dis))
        bad = 1
        continue
    n, chain = deep(r)
    ok = n <= LIMIT
    bad |= not ok
    print('stack_check: %s %s %d B (limit %d of 2816, trap at 2560): %s%s' % ('ok  ' if ok else 'FAIL', r, n, LIMIT,
                                                                              ', '.join(chain), ' (left out: %s)' % ', '.join(sorted(SKIP)) if SKIP else ''))
sys.exit(1 if bad else 0)
