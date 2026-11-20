# SPDX-License-Identifier: GPL-3.0-only
"""ph_order.py [CLEANER]: the phase hand-over's order checked on the null against the development engine (both black boxes), on
note sequences where the voice under test can only sound the same if it takes the same kept phases (ph_meas.patch:
three sines, the amp envelope at once, so a released voice is silent):
  chain  A on, off, B on, off, C on, off, D on: every voice takes the one released before (the first note's phases on)
  stamp  A on, B on (a random phase), A off, B off, C on: C takes A's phases (the higher stamp: 4 against 3), not B's
         (the last released): only then does C null against the development engine, B's phases being random in either engine
  last   the same with B released first: now B has the higher stamp (and is the last released): C takes B's random
         phases in both engines, so this one must NOT null (the check that the test can tell)
Prints each segment's null (dB re the signal) after the voices before it have ended."""
import sys
import numpy as np
import mlab
from ph_meas import patch

CASES = {
    'chain': ([('on', 45, 100), ('run', 9), ('off', 45), ('run', 3), ('on', 52, 100), ('run', 11), ('off', 52), ('run', 2),
               ('on', 59, 100), ('run', 7), ('off', 59), ('run', 5), ('on', 50, 100), ('run', 30)], 0),
    'stamp': ([('on', 45, 100), ('run', 6), ('on', 50, 100), ('run', 7), ('off', 45), ('run', 5), ('off', 50), ('run', 4),
               ('on', 57, 100), ('run', 30)], 22),
    'last': ([('on', 45, 100), ('run', 6), ('on', 50, 100), ('run', 7), ('off', 50), ('run', 5), ('off', 45), ('run', 4),
              ('on', 57, 100), ('run', 30)], 22),
}

if __name__ == '__main__':
    C = sys.argv[1] if len(sys.argv) > 1 else mlab.CLN
    P = patch()
    for nm, (ev, frm) in CASES.items():
        a, b = mlab.render(mlab.HYB, P, ev), mlab.render(C, P, ev)
        s = frm * 128 + 64
        d = a[s:] - b[s:]
        print('%-6s null %.1f dB' % (nm, 10 * np.log10(np.sum(d ** 2) / np.sum(a[s:] ** 2) + 1e-30)))
