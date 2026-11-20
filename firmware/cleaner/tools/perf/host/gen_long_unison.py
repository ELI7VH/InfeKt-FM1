#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
# gen_long_unison.py SEED N > tests/long_unison_multi.txt (after 1.08): a long random session of N keys on two
# parts with unison 7 and HOLD (the test's own lines say what it checks); the test is seed 7, N 300
import random, sys
seed, N = int(sys.argv[1]), int(sys.argv[2])
R = random.Random(seed)
o = []; P = o.append
P('# requires: multi')
P('# (after 1.08) a long random session on two parts (LAYER, then SPLIT, then LAYER at a low voice cap), both')
P('# parts on a unison-7 patch of decays alone (sustain 0), UNISON MAX OFF, HOLD (REC) switched on and off now and')
P('# then, chords, legato and staccato: each key struck sounds on its parts 40 ms later (keyson), and once every key')
P('# is up and HOLD off nothing sounds or is held anywhere (vfree), and every key still plays. 1.08 failed it: a key')
P('# held by HOLD whose voices were taken, struck again, stayed silent (its note-off and note-on in one block:')
P('# app.c key_rs). Generated: gen_long_unison.py %d %d' % (seed, N))
P('wait 1500'); P('listen'); P('re hello'); P('wait 100'); P('mxmode 1'); P('mxset uni 0'); P('wait 200')
for part in (0, 1):
    P(f'partedit {part}'); P('wait 100')
    for by, v in ((97, 6), (98, 127), (60, 50), (61, 0), (63, 30), (129, 0)):
        P(f'reset {by} {v}')
P('partedit 0'); P('wait 300')
keys = [f'w{i}' for i in range(16)] + [f'b{i}' for i in range(11)]
down = []; hold = 0
for i in range(N):
    if i == N // 3: P('echo SPLIT'); P('mxmode 2'); P('wait 100')
    if i == 2 * N // 3: P('echo LAYER at 4 voices'); P('mxmode 1'); P('mxset maxv 4'); P('wait 100')
    if R.random() < 0.04:
        P('tap REC'); hold ^= 1
    k = R.choice([x for x in keys if x not in down])
    P(f'key {k} down {R.randint(20, 127)}'); down.append(k)
    P('wait 40'); P(f'keyson {k}')
    P(f'wait {R.randint(5, 250)}')
    keep = R.choice((0, 0, 1, 1, 2, 3))
    while len(down) > keep:
        P(f'key {down.pop(0)} up'); P('wait 15')
for k in down: P(f'key {k} up')
if hold: P('tap REC')
P('wait 100'); P('said Hold OFF') if hold else None
P('mxset maxv 14')
P('wait 3000'); P('vfree')
P('echo every key plays')
for k in keys:
    P(f'key {k} down 100'); P('wait 40'); P(f'keyson {k}'); P(f'key {k} up'); P('wait 400')
P('wait 3000'); P('vfree')
print('\n'.join(o))
