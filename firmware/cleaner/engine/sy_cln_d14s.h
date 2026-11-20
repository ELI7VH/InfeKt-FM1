/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SY_CLN_D14S_H
#define SY_CLN_D14S_H
#include "sy_math.h"
#include "sy_cln_d14.h"
/* CLEANER: distortion types 1-4 (sy_cln_d14.h, fitted by tools/measure/fit_dist14.py to the development engine's static curves,
 * measured as a black box by meas_dist14.py): each half of a type is one shape S over the intensities, y = A S(k |x|),
 * S linear in z between the points 2^(LO + e) (1 + q / 8) of its grid, addressed by z's exponent and three top
 * mantissa bits; below the grid a line from 0, above it S's last value */
static inline __attribute__((always_inline)) float sy_d14_s(const float *S, float z)
{
    sy_fu u;
    int e, j;
    u.f = z;
    e = (int)(u.u >> 23) - (127 + CLN_D14_LO);
    if (e < 0) return z * S[0] * (float)(1 << -CLN_D14_LO);
    j = e * CLN_D14_PO + (int)((u.u >> 20) & 7u);
    if (j >= CLN_D14_N - 1) return S[CLN_D14_N - 1];
    return S[j] + (float)(u.u & 0xFFFFFu) * (1.f / 1048576.f) * (S[j + 1] - S[j]);
}
#endif
