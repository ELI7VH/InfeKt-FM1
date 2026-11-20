# SPDX-License-Identifier: GPL-3.0-only
"""fit_shape.py HYB.json OWN.json OWN_TABLE: the oscillator's saw-to-square morph from 64 on, from meas_shape.py's
measurement of the development engine (HYB) and of the cleaner with its own table T_SHAPE_C (OWN; OWN_TABLE: probe_law shape,
the table's values). The measured c carries a small bias of the method (the band limit's ripple: the cleaner's
measured c against its known table); taken off, the development engine's c is fitted with c = a u / (1 - b u), u = (shape - 63.5)
/ 63.5 (and with polynomials, for comparison). Prints a, b and the fits' errors."""
import sys, json
import numpy as np
from scipy.optimize import least_squares

h, c = json.load(open(sys.argv[1])), json.load(open(sys.argv[2]))
own = np.array([float(l.split()[1]) for l in open(sys.argv[3])])
s = np.array(h['shape']); m = s >= 64
ch = np.array(h['c']) - (np.array(c['c']) - own[s])
u = (s[m] - 63.5) / 63.5
for nm, f, p0 in (('rational a u / (1 - b u)', lambda p, u: p[0] * u / (1 - p[1] * u), [0.7, 0.2]),
                  ('poly2', lambda p, u: p[0] * u + p[1] * u * u, [0.7, 0.3]),
                  ('poly3', lambda p, u: p[0] * u + p[1] * u * u + p[2] * u ** 3, [0.7, 0.3, 0])):
    r = least_squares(lambda p: f(p, u) - ch[m], p0)
    print('%-26s %s  max err %.2e' % (nm, np.round(r.x, 5), np.abs(r.fun).max()))
