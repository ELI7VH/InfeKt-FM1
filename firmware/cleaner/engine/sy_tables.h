/* SPDX-License-Identifier: GPL-3.0-only */
/* Per-value curves, built once by sy_tables_init from the measured points of docs/spec/. Each is 128 floats indexed
 * by a parameter value (0..127); sy_tab() reads one with linear interpolation for modulated (fractional) values. */
#ifndef SY_TABLES_H
#define SY_TABLES_H
#include "synth.h"

enum {
    T_ATK,          /* attack: seconds of the full linear ramp */
    T_T60,          /* decay / release: seconds to fall 60 dB */
    T_SUS,          /* amp sustain level, linear */
    T_SLOPE,        /* sustain slope, dB/s (negative falls) */
    T_LFO_HZ,       /* LFO 1-3 rate */
    T_CH_HZ,        /* chorus / delay / phaser rate */
    T_LFO_CENTS,    /* LFO 1 -> pitch, cents by |amount - 64| (0..64) */
    T_L3_CENTS,     /* LFO 3 amount, cents */
    T_FADE_D,       /* LFO 3 fade-in: the wait, s */
    T_FADE_R,       /* LFO 3 fade-in: the ramp, s */
    T_PORTA,        /* portamento: the time constant of the lag, s */
    T_UNI_DET,      /* unison detune: cents of the outer voices */
    T_DET2,         /* osc 2 detune, cents (up) */
    T_DET3,         /* osc 3 detune, cents (down) */
    T_FENV_G,       /* filter env amount: cutoff steps at level 1 */
    T_SHAPE_A,      /* shape 0..64: the saw's share a */
    T_SHAPE_C,      /* shape 64..127: the second saw's weight c */
    T_SHAPE_G,      /* shape: the overall gain (the measured RMS) */
    T_RESO_K,       /* resonance: 1/Q */
    T_RESO_PG,      /* resonance: the passband gain */
    T_OSCVOL,       /* osc volume 0..64 as a level */
    T_VELAMP_DB,    /* velocity -> amp at full amount, dB by velocity */
    T_PAN_LFO,      /* LFO 2 -> pan: pan steps by |amount - 64| */
    T_FM_I,         /* FM amount -> modulation index (mode 0) */
    T_COUNT
};

void sy_tables_init(sy_synth *s);
float sy_stretch_cents(float note);
float sy_sqrt(float x);

#ifdef SY_CTL_RAM
__attribute__((always_inline))          /* (into the control blocks' RAM code: no call to the flash) */
#endif
static inline float sy_tab(const sy_synth *s, int t, float v)
{
    int i;
    float f;
    if (v <= 0.f) return s->tables[t][0];
    if (v >= 127.f) return s->tables[t][127];
    i = (int)v;
    f = v - (float)i;
    return s->tables[t][i] + f * (s->tables[t][i + 1] - s->tables[t][i]);
}

#endif
