/* SPDX-License-Identifier: GPL-3.0-only */
/* probe_law [shape]: the cleaner's OWN filter law (or with an argument its own osc morph table T_SHAPE_C) (the specification team's cutoff and resonance law, without the correction fitted by
 * fit_filt.py: CLN_LAW_ONLY): per cutoff value 0..127 and resonance 0 / 32 / 64 / 96 / 112 / 120 / 124 / 127, the frequency g stands for
 * (Hz) and the damping k. Build: cc -O2 -DCLN_LAW_ONLY -I../../engine probe_law.c ../../engine/sy_*.c -lm (see
 * fit_filt.py). Only the cleaner's own code is read here; the development engine is measured as a black box (filt_fit.py). */
#include <stdio.h>
#include <math.h>
#include "../../engine/synth.c"

int main(int argc, char **argv)
{
    static sy_synth s;
    int c, r;
    static const int R[8] = {0, 32, 64, 96, 112, 120, 124, 127};
    sy_init(&s, 46875.f);
    if (argc > 1) {                     /* probe_law shape: the own morph table T_SHAPE_C (fit_shape.py) */
        for (c = 0; c < 128; c++) printf("%d %.9g\n", c, (double)s.tables[T_SHAPE_C][c]);
        return 0;
    }
    for (r = 0; r < 8; r++)
        for (c = 0; c < 128; c++) {
            float g = filt_g(&s, (float)c, 1), h, k, pg;
            filt_k(&s, (float)c, (float)R[r], g, &h, &k, &pg, 1);
            printf("%d %d %.9g %.9g\n", c, R[r], atan((double)g) * 46875.0 / 3.14159265358979, (double)k);
        }
    return 0;
}
