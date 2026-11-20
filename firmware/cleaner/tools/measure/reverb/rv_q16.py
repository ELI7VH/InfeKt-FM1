# SPDX-License-Identifier: GPL-3.0-only
"""rv_q16.py [SECS]: the 16-bit ring's words against the float ring's (rvlib.py: RV_NEW the float build, RV_NEW16 the
two-part build, Bluetooth's memory not lent: HALF on the 16-bit ring; RV_OLD / RV_OLD16 the development engine's the same): an
impulse (0.25) into room 2 at decay 104, 120 and 127, the output's level (dB) every SECS / 10 seconds. Shows the words'
floor (where a tail stops, 0: -240 dB) and limit cycles (a level that stops falling above 0); the sizing of
sy_cln_rv.c's RVQ_GATE and the rounding (q16)."""
import sys
import numpy as np
import rvlib as R

T = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
E = {'own float': R.engines('nhalf'), 'own 16-bit': R.engines('nhalf16'),
     'hyb float': R.engines('half'), 'hyb 16-bit': R.engines('half16')}
for dec in (104, 120, 127):
    p = R.patch(mode=2, room=2, decay=dec)
    for k, (exe, env) in E.items():
        y = R.run(exe, p, R.impulse(T, amp=0.25), T, env)
        s = int(R.FS); st = max(1, int(T) // 10)
        lv = [20 * np.log10(np.sqrt((y[i * s:(i + 1) * s] ** 2).mean()) + 1e-12) for i in range(0, int(T), st)]
        print('decay %3d %-11s' % (dec, k), ' '.join('%6.1f' % v for v in lv), flush=True)
