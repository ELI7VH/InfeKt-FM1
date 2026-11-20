# SPDX-License-Identifier: GPL-3.0-only
"""fit_dist89.py HYB.json: the laws of distortion types 8 and 9 from meas_dist89.py's measurement of the development engine.
  8  the quantiser's step: 1 / step is linear in the intensity between nodes every 16 (the steps' differences are
     constant within 16: read off the measurement), the node steps x 512 printed; the law in sy_fx.c uses the
     nodes rounded to the small numbers they are (1, 2, 4, 8, 12, 16, 24, 32, 64 / 512)
  9  the rate reducer's phase step (1 / the mean hold): linear between nodes; their places found from the kinks
     (where the differences change), then the node values least-squares fitted; above the second node they fall by
     2^(-1/4) a node, the nodes at 1 + k 127 / 22. Prints the nodes and the model's error against the measurement."""
import sys, json
import numpy as np
from scipy.optimize import least_squares

R = json.load(open(sys.argv[1]))
st = np.array(R['step8']); inc = 1 / np.array(R['hold9']); it = np.arange(128.0)
# 8
nodes8 = 1 / np.array([np.polyval(np.polyfit(it[k:k + 16], 1 / st[k:k + 16], 1), k) for k in range(0, 128, 16)] +
                      [np.polyval(np.polyfit(it[112:128], 1 / st[112:128], 1), 128)])
print('dist 8 node steps x 512 at 0, 16, .., 128:', np.round(nodes8 * 512, 4).tolist())
S = np.array([1, 2, 4, 8, 12, 16, 24, 32, 64.0]) / 512
m8 = 1 / np.interp(it, np.arange(9) * 16.0, 1 / S)
print('  the law with the rounded nodes: max error %.2e' % np.abs(m8 / st - 1).max())


# 9
def nodes_of(p):
    return np.concatenate([[2, p[-4], p[-3]], p[-2] + np.arange(0, 21) * p[-1]])


m = it >= 2
nd0 = np.concatenate([[2, 6.77, 12.53], 18.3 + np.arange(0, 21) * 5.7727])
p0 = list(np.interp(nd0, it, inc)) + [6.77, 12.53, 18.3, 5.7727]
r = least_squares(lambda p: np.interp(it, nodes_of(p), p[:-4])[m] / inc[m] - 1, p0)
print('dist 9 free fit: node places %s ..., spacing %.5f (127 / 22 = %.5f), max error %.1e' %
      (np.round(nodes_of(r.x)[:5], 4).tolist(), r.x[-1], 127 / 22, np.abs(r.fun).max()))
print('  node values:', np.round(r.x[:-4], 7).tolist())
print('  ratios:', np.round(r.x[4:-5] / r.x[3:-6], 5).tolist())
nd = [0, 1, 2, 1 + 127 / 22.0] + [1 + k * 127 / 22.0 for k in range(2, 23)]
vl = [1, 1, 0.4810875, 2 ** -1.5] + [0.088725 if k == 8 else 2 ** (-1.5 - k / 4) for k in range(2, 23)]
m9 = np.interp(it, nd, vl)
print('  the law (1 to 1, 0.4810875 at 2, 2^(-1.5 - k/4) at 1 + k 127/22, node 8 0.088725): max error %.2e' %
      np.abs(m9[1:] / inc[1:] - 1).max())
