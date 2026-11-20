/* SPDX-License-Identifier: GPL-3.0-only */
/* CLEANER (own design; PROVENANCE.md 1, tools/measure/osc/): osc 1 and 2 as band-limited steps, minBLEP-style.
 *
 * Each oscillator is its naive wave (the saw, the pulse, the narrow pulse, the triangle: straight pieces), CO_LAT ticks
 * late, plus for every step a residual: the measured step response C (sy_cln_osc.h, five ticks long) less the naive
 * jump; for a corner of the triangle the same for a ramp's start (B, the sum of C). The sine from its values on the
 * two ticks around the delayed time (exact, as a step); the table waves through a short filter of the same response (CO_FIR). Then the emphasis eb x[n] - ea x[n-1].
 *
 * The ticks: the oscillators' steps are measured to land on a clock of P ticks a cycle of osc 1 (P by osc 1's pitch,
 * synth.c): osc 1 restarts on a tick at CO_T0 (its period exactly P ticks), osc 2's steps fall on the tick after them,
 * the pulse's edge on the tick nearest its width (the width's fraction of a tick shared with the next one). That is
 * what places the aliasing where the development engine has it (s->osc_clean 0, ORIGINAL, the default); osc_clean 1 (CLEAN) puts
 * every step at its own time through a smooth step (co_gc: less aliasing, the top octave softer), anything between
 * mixes the two. Only the steps' times
 * are kept on the clock: no wave is computed per tick.
 *
 * The ring modulator: the product of two straight-piece waves exactly (the product of the naive waves, its steps
 * scaled by the other wave at the step, the curvature of two ramps); with a sine or a table in it, their product's
 * steps the same way, its smooth parts (a sine or table with the other wave's present line) through the tables'
 * filter. A phase set from outside (a note's start) is a step from the last value to the new wave (the
 * clock's past as the development engine's: its last ticks are the old wave's). */
#include "sy_cln_osc.h"
#ifndef CO_V6_LAT1
#define CO_V6_LAT1 1
#endif
#ifndef CO_LAT
#define CO_LAT 3.f                      /* the naive wave's delay, ticks (the step response's middle, 2.5, + half a tick;
                                         * kernel.py) */
#endif
/* a step response C over five ticks (C(1) = c1, C(2) = c2, symmetric, straight between whole ticks): 1 - C at the
 * ticks (gn), B - tau + 3 there (bn, B the response to a ramp), its second moment (s2, a parabola's offset) and its
 * taps (ft). Three of them: the measured one (CO_C1, CO_C2), and the 6-voice state's (synth.c CO_V6): osc 1 a
 * straight line over one tick (C1 = C2 = 0), osc 2 its own (CO_V6_C1, CO_V6_C2) */
typedef struct { float gn[6], bn[6], s2, c1, c2, ft[5], lat; } co_resp;
#define CO_RESP(c1, c2) {{1.f, 1.f - (c1), 1.f - (c2), (c2), (c1), 0.f}, {3.f, 2.f, 1.f + (c1), (c1) + (c2), (c1), 0.f}, \
    2.f * (3.f * (c1) + (c2)), (c1), (c2), {(c1), (c2) - (c1), 1.f - 2.f * (c2), (c2) - (c1), (c1)}, CO_LAT}
static const co_resp CO_R[4] = {CO_RESP(CO_C1, CO_C2), CO_RESP(0.f, 0.f), CO_RESP(CO_V6_C1, CO_V6_C2),
    /* osc 1's line two ticks earlier: over in one tick, the naive wave a tick late (the steps' window 1 tick) */
    {{1.f, 0.f, 0.f, 0.f, 0.f, 0.f}, {1.f, 0.f, 0.f, 0.f, 0.f, 0.f}, 0.f, 0.f, 0.f, {0.f, 0.f, 1.f, 0.f, 0.f}, 1.f}};
#undef CO_RESP
/* 1 - C(tau): what is left of a step tau ticks after its tick (tau < 5: the callers' test) */
static inline __attribute__((always_inline)) float co_g(float tau, const co_resp *C)
{
    int i;
    if (tau < 0.f) return 1.f;
    i = (int)tau;
    return C->gn[i] + (tau - (float)i) * (C->gn[i + 1] - C->gn[i]);
}
/* B(tau) - tau + lat, B the response to a ramp starting on a tick (tau < 5), lat the naive wave's delay */
static inline __attribute__((always_inline)) float co_bm(float tau, const co_resp *C)
{
    int i;
    if (tau < 0.f) return C->lat - tau;
    i = (int)tau;
    return C->bn[i] + (tau - (float)i) * (C->bn[i + 1] - C->bn[i]);
}

typedef struct {
    float a, ia, t0, eps;               /* the phase a tick, ticks a cycle, the phase after a wrap (osc 1: its restart),
                                         * the margin of a whole tick count */
    float wr, wp, pd, tkn, htkn, tkm, tkf, ies, np, tri, sg, ws, dd;
    float prc, prs, pwc, pws, p6c, p6s;   /* (a sine: the turns of a tick, a sample, three samples: cos, sin) */
    const float *wt;
    float te[3], qw, tf;                /* (the quiet stretches: ne steps' phases te, the window qw after each, from a
                                         * tick before; the pulse's fall tf) */
    uint8_t r1;                         /* osc 1 (the restart: its jump D; the pulse's rise a tick after it) */
    uint8_t saw, pul, pul0, npl, trl, sin, tab, wrap, on;   /* its parts: the ramp, the pulse on the ticks (tkn > 0),
                                         * the pulse as it is (tkn 0), the narrow pulse, the triangle, the sine, a table */
    uint8_t fast, ne, sgn, lin;         /* (fast: no step within the response's window, the naive wave alone; sgn: co_sig
                                         * has pieces; lin: straight pieces) */
} co_o;
#define CO_NR 6                         /* (a record's steps, corners: more only in cycles of a tick or two, co_lin's own
                                         * value then, each sample) */
#define CO_NC 3
#define CO_NF 4
typedef struct {
    float b, s, bc, bk;                 /* the straight pieces (every recorded step taken) at the sample, their slope a
                                         * tick; bc + bk td (td the delayed phase) */
    uint8_t n, nc, ovf, zs, st, nf;      /* (zs: co_sig's at the record, st: none taken yet; nf steps still to come) */
    float fa, fm, fs;                     /* (the steps still to come: their ages less fa, the ticks since the record; fm
                                         * the latest's; their sizes are off bc, as the naive wave has them) */
    float fo[CO_NF], fd[CO_NF];
    float pr, xs, kc, ks, yc, ys;       /* (kc, ks: a sine at the tick, yc, ys: at the delayed time, co_sinn's, co_rsm's
                                         * phasors, taken exactly every 16 turns) */
    uint8_t kn, yn;
                                        /* (pr: CLEAN, a pulse's fall ages by its own time, pr a tick; xs: the steps or
                                         * corners past the record's room, their value at its sample) */
    float of[CO_NR], dl[CO_NR];         /* the steps: age (ticks, CO_STEP's) less the clock's fraction c, size */
    uint8_t pf[CO_NR];                  /* (the pulse's falls) */
    float co[CO_NC], cd[CO_NC], cs[CO_NC];   /* the triangle's corners: age less ex less c, the tick's fraction, the
                                         * slope's change */
} co_ev;

static void co_init(co_o *o, const sy_osc_t *s, uint32_t inc, int r1, int on, float sma)
{
    o->on = on;
    o->a = sy_maxf((float)inc * U2F, 1e-9f);
    o->ia = 1.f / o->a;
    o->r1 = r1;
    o->t0 = r1 ? CO_T0 : 0.f;
    o->eps = r1 ? 0.01f : 0.f;          /* (osc 1's ticks since its restart are whole: the float's error away) */
    o->ws = s->ws; o->wt = 0;
    o->wr = s->wsaw * (1.f - s->c);
    o->wp = s->wsaw * s->c;
    o->dd = (float)s->d * U2F;
    o->pd = 2.f * o->dd;
    o->tkn = s->tkn; o->htkn = 0.5f * s->tkn; o->tkm = s->tkm; o->tkf = s->tkf;
    o->ies = s->tkn > 0.f ? 1.f / (s->tkn * o->a) : 1.f;
    o->saw = on && o->wr != 0.f;
    o->pul = on && o->wp != 0.f && s->tkn > 0.f;
    o->pul0 = on && o->wp != 0.f && s->tkn <= 0.f;
    o->npl = on && s->ws != 0.f && s->wave == 64;
    o->trl = on && s->ws != 0.f && s->wave == 1;
    o->sin = on && s->ws != 0.f && s->wave == 0;
    o->tab = on && s->ws != 0.f && s->wave >= 2 && s->wave <= 63 && s->wt;
    if (o->tab) o->wt = s->wt;
    o->np = o->npl ? s->ws * 0.803f : 0.f;             /* (the narrow pulse's level: RMS 10 dB under the sine, as the plain oscillator's) */
    o->tri = s->ws * 1.23370055f;
    o->wrap = o->saw | o->pul | o->pul0 | o->npl;
    /* the sine's gain: the step response's taps (C's steps) at its frequency (sma) */
    o->sg = s->ws * sma;
    o->prc = sy_cos2pi(o->a); o->prs = sy_sin2pi(o->a);   /* (the sine's phasors: a tick, a sample, three samples) */
}

/* the naive value at phase t (rs: osc 1's restart tick; lin: its straight pieces alone): a start's step, the click's
 * sign, the ring's products with a sine or a table */
static __attribute__((noinline)) float co_nv(const co_o *o, float t, int rs, int lin)   /* (in the flash: a block's
                                         * few calls) */
{
    float y = 0.f, e, u;
    if (!o->on) return 0.f;
    if (o->saw) y += o->wr * (1.f - 2.f * t);
    if (o->pul) {
        e = t * o->tkn - o->htkn + o->tkm;
        y += o->wp * (rs ? o->pd - 2.f : e <= -0.5f ? o->pd : e >= 0.5f ? o->pd - 2.f : o->pd - 2.f * o->tkf);
    }
    if (o->pul0) { u = t + o->dd; u -= (float)(int)u; y += o->wp * 2.f * (u - t); }
    if (o->npl) { u = t + 0.02f; if (u >= 1.f) u -= 1.f; y += o->np * 2.f * (u - t); }
    if (o->trl) y += o->tri * (t < 0.25f ? 4.f * t : t < 0.75f ? 2.f - 4.f * t : 4.f * t - 4.f);
    if (lin) return y;
    if (o->sin) y += o->ws * sy_sin2pi(t);
    if (o->tab) {
        float x = t * (float)SY_WAVE_N, f;
        int j = (int)x;
        if (j >= SY_WAVE_N) j = SY_WAVE_N - 1;
        f = x - (float)j;
        y += o->ws * (o->wt[j] + f * (o->wt[j + 1] - o->wt[j]));
    }
    return y;
}
static float co_naive(const co_o *o, uint32_t p, int rs) { return co_nv(o, (float)p * U2F, rs, 0); }

/* the steps' response for the block: ORIGINAL (osc_clean 0) as measured; CLEAN (1) a smoother step instead (CO_GC,
 * co_design.py: the measured response up to 0.25 cycles a tick, none above 0.33; less aliasing),
 * the naive wave later by ex ticks; between them a mix of the two. The triangle's corners, the ring's steps and a
 * start's step keep the measured response, ex ticks later */
typedef struct { float lat, latn, ex, q, lim, rr; const co_resp *C; } co_k;   /* (rr: the ticks a sample, 2 r; C the
                                         * step response) */
static inline __attribute__((always_inline)) float co_gc(float tau)   /* 1 - the smooth step (tau < 2 CO_GC_L) */
{
    float x;
    int i;
    if (tau < 0.f) return 1.f;
    x = tau * (float)CO_GC_R;
    i = (int)x;
    return CO_GC[i] + (x - (float)i) * (CO_GC[i + 1] - CO_GC[i]);
}
static inline __attribute__((always_inline)) float co_gk(float tau, const co_k *K)   /* (tau < K->lim) */
{
    float o, g;
    o = tau - K->ex;
    if (K->q == 0.f) return o < 5.f ? co_g(o, K->C) : 0.f;
    g = o < 5.f ? co_g(o, K->C) : 0.f;
    return g + K->q * (co_gc(tau) - g);
}

/* the sine's two ticks (osc 1: a tick before its last restart by the cycle before, its jump D: the phase the clock
 * held there) */
static __attribute__((noinline)) float co_sinr(const co_o *o, float t, float c, float D, float k, float e, const co_resp *C)
                                        /* (in the flash: a few samples a cycle) */
{
    const float *FT = C->ft;
    float xs[6], f0 = 0.f, f1 = 0.f;
    int n;
    for (n = 0; n < 6; n++) {
        float u = t - ((float)n + e) * o->a;
        if ((float)n + e > k) u += D - 1.f;
        xs[n] = sy_sin2pi(u);
    }
    for (n = 0; n < 5; n++) { f1 += FT[n] * xs[n]; f0 += FT[n] * xs[n + 1]; }
    return o->ws * (f0 + c * (f1 - f0));
}
/* (a phasor taken exactly: sin and cos of 2 pi t, out of line: the same values) */
static SY_HOT __attribute__((noinline)) void co_sc(float t, float *sn, float *cs) { *sn = sy_sin2pi(t); *cs = sy_cos2pi(t); }

/* (the sine at the tick u3 and the next, a phasor turned a tick at each tick: nt this sample's ticks; w osc 1's
 * restart: taken again) */
static inline __attribute__((always_inline)) float co_sinn(const co_o *o, co_ev *R, float t, float c, float D, const co_k *K,
                                                          float nt, int w)
{
    float s3b;
    if (o->r1 && D > 0.f) {             /* (within the response's reach of osc 1's restart: the clock's ticks one by
                                         * one through the taps, the ticks before it a cycle back, by D) */
        float x = (t - o->t0) * o->ia;
        if (x >= 0.f && x < K->latn + 3.5f) {
            R->kn = 99;
            return SY_FAR(co_sinr)(o, t, c, D, (float)(int)(x + o->eps), K->latn - 3.f, K->C);
        }
    }
    if (w | (R->kn + (int)nt > 16)) {
        const float u3 = t - K->latn * o->a;
        co_sc(u3, &R->ks, &R->kc); R->kn = 0;
    } else {
        int k;
        for (k = 0; k < (int)nt; k++) {
            const float kc = R->kc * o->prc - R->ks * o->prs;
            R->ks = R->ks * o->prc + R->kc * o->prs; R->kc = kc;
        }
        R->kn += (int)nt;
    }
    s3b = R->ks * o->prc + R->kc * o->prs;
    return o->sg * (R->ks + c * (s3b - R->ks));
}


/* (co_gk out of line: a step past the record's room, cycles of a tick or two) */
static __attribute__((noinline)) float co_gkx(float tau, const co_k *K) { return co_gk(tau, K); }   /* (in the flash) */

/* (a step of size dl, tau ticks after its tick: recorded) */
static SY_HOT __attribute__((noinline)) void co_put(co_ev *R, const co_k *K, float tq, float dl, float c, float sg, int pf)
{
    if (tq < sg - 0.5f && tq < K->lim) {
        if (tq < 0.f && R->nf < CO_NF) {   /* (still to come: its size off the pieces, co_lin's end) */
            R->fo[R->nf] = tq - c; R->fd[R->nf++] = dl; R->fs += dl;
            if (tq - c > R->fm) R->fm = tq - c;
        } else if (R->n < CO_NR) { R->of[R->n] = tq - c; R->pf[R->n] = (uint8_t)pf; R->dl[R->n++] = dl; }
        else { R->ovf = 1; R->xs -= dl * SY_FAR(co_gkx)(tq, K); }
    }
}
#define CO_STEP(tau_, dl_) co_put(R, K, (tau_), (dl_), c, sg, pf_)

/* an oscillator's straight pieces at the delayed time (t: its phase at the last tick, c: the clock past it), with
 * every step's residual; D: osc 1's last restart's jump (0: none yet); q: osc_clean; sg: the age of a start (steps
 * older than it are not: its step holds them). R gets the record (CLEANER, 1.18C25): the straight pieces with every
 * recorded step taken (b), their slope a tick (s) and change a sample (db), the steps (their age tq as CO_STEP has
 * it, size dl) and the triangle's corners (age less ex, the tick's fraction, the slope's change); the value returned
 * (without the sine) is the record's (co_rv) at this sample */
static inline __attribute__((always_inline)) void co_lin(const co_o *o, float t, float c, float D, float q, float sg, co_ev *R,
                                                         const co_k *K)
{
    const float a = o->a, ia = o->ia, td = t + (c - K->lat) * a, Dc = D > 0.f ? D : 1.f;
    float y = 0.f, sl = 0.f, bc = 0.f, bk = 0.f, x, k, tau;
    int pf_ = 0;
    R->n = R->nc = R->ovf = R->nf = 0; R->xs = R->fa = R->fs = 0.f; R->fm = -1e30f;
    R->pr = 1.f + q * (o->tkn * a - 1.f);   /* (e moves tkn a a tick: CLEAN's own time of a fall) */
    if (o->saw) { y = o->wr * (1.f - 2.f * td); sl = -2.f * o->wr * a; bc = o->wr; bk = -2.f * o->wr; }
    if ((o->wrap || (o->r1 && o->trl)) && (!o->r1 || D > 0.f)) {   /* the wrap (osc 1: its restart) and, in a short
                                         * cycle, the one before */
        float dw = 2.f * Dc * o->wr + 2.f * o->np + (o->r1 ? (o->trl ? -4.f * o->tri * (Dc - 1.f) : 0.f) : 2.f * o->wp);   /* (osc 1's
                                         * triangle: the restart's phase jump, D - 1 of a cycle) */
        x = (t - o->t0) * ia;
        if (x >= 0.f) {
            int m;
            for (m = 0; m < 16; m++) {     /* (CLEAN's window takes up to three cycles of 7 ticks) */
                k = (float)(int)(x + o->eps);
                tau = k + c;
                if (!o->r1 && q != 0.f) tau += q * (x + c - 0.5f - tau);
                if (tau >= (o->r1 ? K->lim + 1.f : K->lim)) break;   /* (osc 1's rise a tick later) */
                CO_STEP(tau, dw);
                if (o->r1 && o->wp != 0.f) CO_STEP(tau - 1.f, 2.f * o->wp);   /* (osc 1's pulse rises a tick later) */
                x += Dc * ia;
            }
        }
    }
    if (o->pul) {                       /* the pulse's fall: on the tick within half a tick of its width, the width's
                                         * fraction there, the rest on the next tick */
        float e = t * o->tkn - o->htkn + o->tkm, ta, tb;   /* (from this cycle's fall, ahead or not: a fall still to
                                         * come is a whole step off, CLEAN's own time may be past already) */
        y += o->wp * (o->pd - 2.f); bc += o->wp * (o->pd - 2.f);
        for (;;) {                      /* (this cycle's fall, and in a short cycle the ones before) */
            ta = (float)sy_floori((e + 0.5f) * o->ies) + c;   /* (the first tick past -0.5, the first at 0.5: e moves
                                         * tkn a ticks a tick, a hair under 1) */
            tb = (float)sy_floori((e - 0.5f) * o->ies) + c;
            if (q != 0.f) {
                float te = e + o->tkf + c - 1.f;    /* (its own time: where the width puts it, as late on average) */
                ta += q * (te - ta); tb += q * (te - tb);
            }
            if (tb >= K->lim) break;
            pf_ = 1;
            CO_STEP(ta, -2.f * o->wp * o->tkf);
            CO_STEP(tb, -2.f * o->wp * (1.f - o->tkf));
            pf_ = 0;
            e += o->tkn * Dc;
        }
    }
    if (o->pul0) {                      /* (osc 2 above half the clock: the pulse as it is, its fall on the tick after) */
        float u = t + o->dd;
        u -= (float)(int)u;
        y += o->wp * 2.f * (u - t); bc += o->wp * 2.f * (u - t);
        for (x = u * ia; ; x += ia) {
            k = (float)(int)x;
            tau = k + c;
            if (q != 0.f) tau += q * (x + c - 0.5f - tau);
            if (tau >= K->lim) break;
            CO_STEP(tau, -2.f * o->wp);
        }
    }
    if (o->npl) {                       /* the narrow pulse (osc 1): its fall at 0.98 on the tick after, its rise with the
                                         * restart */
        float u = t + 0.02f;
        if (u >= 1.f) u -= 1.f;
        y += o->np * 2.f * (u - t); bc += o->np * 2.f * (u - t);
        for (x = (t >= 0.98f ? t - 0.98f : t + Dc - 0.98f) * ia; ; x += Dc * ia) {
            k = (float)(int)(x + (D > 0.f ? o->eps : 0.f));   /* (off the clock before the first restart: no margin) */
            tau = k + c;                /* (osc 1: the fall on the clock as its rise, CLEAN too) */
            if (tau >= K->lim) break;
            CO_STEP(tau, -2.f * o->np);
        }
    }
    if (o->trl) {                       /* the triangle: its corners where they are (no step: only the slope changes) */
        const float A = o->tri;
        float xc, ds, fl, d0, yb;
        if (t < 0.25f) { yb = A * 4.f * td; sl += 4.f * A * a; xc = (t + Dc - 0.75f) * ia; ds = 8.f * A * a; bk += 4.f * A; }
        else if (t < 0.75f) { yb = A * (2.f - 4.f * td); sl -= 4.f * A * a; xc = (t - 0.25f) * ia; ds = -8.f * A * a; bc += 2.f * A; bk -= 4.f * A; }
        else { yb = A * (4.f * td - 4.f); sl += 4.f * A * a; xc = (t - 0.75f) * ia; ds = 8.f * A * a; bc -= 4.f * A; bk += 4.f * A; }
        y += yb;
        {
            int m;
            for (m = 0; m < 32; m++) {
                fl = (float)(int)xc;
                tau = fl + c;
                if (tau - K->ex >= 5.f || tau >= sg - 0.5f) break;
                d0 = xc - fl;
                tau -= K->ex;                   /* (CLEAN: the corners as ORIGINAL, as late as the steps' middle) */
                if (R->nc < CO_NC) { R->co[R->nc] = tau - c; R->cd[R->nc] = d0; R->cs[R->nc++] = ds; }
                else { R->ovf = 1; R->xs += ds * (co_bm(tau, K->C) - d0 * co_g(tau, K->C)); }
                xc += 0.5f * ia; ds = -ds;      /* (a short cycle: the corner before) */
            }
        }
    }
    R->b = y - R->fs; R->s = sl; R->bc = bc - R->fs; R->bk = bk;
}
#undef CO_STEP

/* (the record taken: one copy in the RAM code, a call at a step's birth) */
static SY_HOT __attribute__((noinline)) void co_recx(const co_o *o, float t, float c, float D, float q, float sg, co_ev *R,
                                                    const co_k *K)
{
    co_lin(o, t, c, D, q, sg, R, K);
}

/* the record a sample on (nt ticks later, the clock's fraction c, the delayed phase td): its pieces where td is, its
 * steps older (one past the response's end dropped), a step still to come at its time taken on (its size back in bc);
 * 1: CLEAN's step still to come reached its time (the pulse's: its own time from the width, co_lin's again) */
static inline __attribute__((always_inline)) int co_prop(co_ev *R, const co_k *K, float c, float nt, float td)
{
    int j;
    R->fa += nt;
    if (R->nf && R->fm + R->fa + c >= 0.f) {
        if (K->q != 0.f) return 1;
        R->fm = -1e30f;
        for (j = 0; j < R->nf; ) {
            const float of = R->fo[j] + R->fa;
            if (of + c >= 0.f && R->n < CO_NR) {
                R->of[R->n] = of - nt; R->pf[R->n] = 0; R->dl[R->n++] = R->fd[j]; R->bc += R->fd[j]; R->fs -= R->fd[j];
                R->nf--; R->fo[j] = R->fo[R->nf]; R->fd[j] = R->fd[R->nf];
            } else { if (R->fo[j] > R->fm) R->fm = R->fo[j]; j++; }
        }
    }
    R->b = R->bc + R->bk * td;
    for (j = 0; j < R->n; ) {
        float of = R->of[j] + (R->pf[j] ? nt * R->pr : nt);
        if (of + c >= K->lim) { R->n--; R->of[j] = R->of[R->n]; R->dl[j] = R->dl[R->n]; R->pf[j] = R->pf[R->n]; continue; }
        if (K->q != 0.f && of + c >= 0.f && R->of[j] + c + nt - K->rr < 0.f) return 1;
        R->of[j++] = of;
    }
    for (j = 0; j < R->nc; ) {
        float co = R->co[j] + nt;
        if (co + c >= 5.f) { R->nc--; R->co[j] = R->co[R->nc]; R->cd[j] = R->cd[R->nc]; R->cs[j] = R->cs[R->nc]; }
        else R->co[j++] = co;
    }
    return 0;
}
/* the record nt ticks older across a quiet stretch (no birth in it; the steps past the response's end dropped at the
 * next sample; CLEAN: taken again, a fall's own time) */
static inline __attribute__((always_inline)) void co_skip(co_ev *R, const co_k *K, float nt)
{
    int j;
    if (K->q != 0.f) { R->st = 1; return; }
    R->fa += nt;
    for (j = 0; j < R->n; j++) R->of[j] += nt;
    for (j = 0; j < R->nc; j++) R->co[j] += nt;
}
/* the record's value: its pieces less every step through K's response, the corners (as co_lin's) */
static inline __attribute__((always_inline)) float co_rv(const co_ev *R, const co_k *K, float c)
{
    float y = R->b;
    int j;
    for (j = 0; j < R->n; j++) y -= R->dl[j] * co_gk(R->of[j] + c, K);
    for (j = 0; j < R->nc; j++) { float tc = R->co[j] + c; y += R->cs[j] * (co_bm(tc, K->C) - R->cd[j] * co_g(tc, K->C)); }
    return y;
}
/* what changes the record other than its age (a step's birth): osc 1's restart and osc 2's wrap (the clock's
 * flags), the triangle's half, the narrow pulse's fall, osc 2's own pulse's fall (co_lin's pieces by t) */
static inline __attribute__((always_inline)) int co_sig(const co_o *o, float t)
{
    int z = 0;
    if (o->trl) z = (t >= 0.25f) + (t >= 0.75f);
    if (o->npl) z += 4 * (t >= 0.98f);
    if (o->pul0) z += 8 * (int)(t + o->dd);
    return z;
}

/* an oscillator's value at the sample (with its sine; val 0: its record alone, for the ring): the record taken at a
 * step's birth (w: the clock's restart or wrap; co_sig's change; after a quiet stretch), else a sample older (nt the
 * sample's ticks) */
static SY_HOT __attribute__((noinline)) float co_step(const co_o *o, co_ev *R, const co_k *K, float t, float c, float D,
                                                    float sg, float nt, int w, int val)
{
    const int z = o->sgn ? co_sig(o, t) : 0;
    float y = 0.f;
    if (!o->lin) ;                      /* (no straight piece: no record) */
    else if (w | R->st | R->ovf | (z != R->zs) || co_prop(R, K, c, nt, t + (c - K->lat) * o->a)) {
        co_recx(o, t, c, D, K->q, sg, R, K);
        R->zs = z; R->st = 0;
    }
    if (!val) return 0.f;
    if (o->lin) y = co_rv(R, K, c) + R->xs;
    if (o->sin) y += co_sinn(o, R, t, c, D, K, nt, w);   /* (the sine: its values at the two ticks around the delayed time,
                                         * through the response's taps (sg), straight between them: exact, its images
                                         * too) */
    return y;
}

/* (co_step inline where the record holds no step within the response's reach: no birth, no step reaching its time; the
 * naive pieces alone) */
static inline __attribute__((always_inline)) int co_quick(const co_o *o, co_ev *R, const co_k *K, float t, float c,
                                                         float nt, int w, float *y)
{
    if (w | R->st | R->ovf | R->n | R->nc | o->sgn | o->sin | !o->lin) return 0;
    if (R->nf && R->fm + R->fa + nt + c >= 0.f) return 0;
    R->fa += nt;
    *y = R->b = R->bc + R->bk * (t + (c - K->lat) * o->a);
    return 1;
}

/* the quiet stretches, inline (most samples of a cycle longer than the steps' window): no step's response within
 * reach, the naive wave alone (as co_lin gives it there) */
static void co_quiet(co_o *o, const co_k *K)
{
    float w = (K->lim + 3.f) * o->a;
    o->fast = o->on && !o->trl && !o->pul0 && w < 0.3f;
    o->ne = 0;
    o->qw = w;
    o->sgn = o->trl || o->npl || o->pul0;
    o->lin = o->saw || o->wrap || o->trl;   /* (straight pieces: a record; else the sine, the table alone) */
    if (!o->fast) return;
    if (o->wrap || (o->r1 && o->sin)) o->te[o->ne++] = o->t0 - o->a;   /* (osc 1's sine: its restart's ticks, co_sinr) */
    o->tf = 0.f;
    if (o->pul) { o->tf = (o->htkn - o->tkm - 0.5f) / o->tkn; o->te[o->ne++] = o->tf - o->a; }
    if (o->npl) o->te[o->ne++] = 0.98f - o->a;
}
/* a quiet sample's value (co_lin's there: the naive pieces, a pulse's fall still to come counted off; no sine) */
static inline __attribute__((always_inline)) float co_qv(const co_o *o, float t, float c, const co_k *K)
{
    float y = o->wr * (1.f - 2.f * (t + (c - K->lat) * o->a));
    if (o->pul) y += o->wp * (t > o->tf ? o->pd - 2.f : o->pd);
    if (o->npl) y += o->np * (t >= 0.98f ? 0.04f - 2.f : 0.04f);
    return y;
}

/* how many samples (up to n) from now stay quiet for this oscillator: the samples before the next window opens (its
 * phase reached a tick early); a window open at the next sample: -1 less the samples it surely stays open */
static inline __attribute__((always_inline)) int co_span(const co_o *o, uint32_t p, float c, float r, int n)
{
    const float dps = 2.f * r * o->a, ph = (float)p * U2F + (c + 2.f * r) * o->a;   /* (the phase at the next sample) */
    int j, m = n;
    if (!o->on) return n;
    for (j = 0; j < o->ne; j++) {       /* (the windows in the phase at the sample: from te to te + qw, a tick or three
                                         * later than in the phase at the tick, co_fast's) */
        float d = ph - o->te[j], u;
        d -= (float)sy_floori(d);
        if (d < o->qw + 3.f * o->a) return -1 - (int)((o->qw + 3.f * o->a - d) / dps);
        u = (1.f - d - o->a) / dps;     /* (the samples before ph reaches te, a tick early) */
        if (u < (float)m) m = u > 0.f ? (int)u : 0;
    }
    return m;
}

/* the ring's product of two straight-piece waves at the delayed time, exactly: the naive product with the curvature
 * of two ramps (the response's second moment, less the interpolation's), each step scaled by the other wave at its
 * tick and its ramp, both waves' steps together; each wave's pieces ab, eb and slopes as, es (with a sine's or a
 * table's line), its steps the record's within the measured response (age less ex under 5) */
static SY_HOT __attribute__((noinline)) float co_ring(const co_ev *A, const co_ev *E, float ab, float as, float eb, float es,
                                                    float c, const co_k *K)
{
    const co_resp *C = K->C;
    const float cx = c - K->ex;
    float r = ab * eb + 2.f * as * es * (0.5f * C->s2 + 0.5f * c * (1.f - c));
    int i, j;
    for (i = 0; i < A->n; i++) {
        float t = A->of[i] + cx;
        if (t >= 5.f) continue;
        r -= A->dl[i] * (co_g(t, C) * (eb + es * (CO_LAT - t)) - es * co_bm(t, C));
    }
    for (j = 0; j < E->n; j++) {
        float t = E->of[j] + cx;
        if (t >= 5.f) continue;
        r -= E->dl[j] * (co_g(t, C) * (ab + as * (CO_LAT - t)) - as * co_bm(t, C));
    }
    for (i = 0; i < A->n; i++) {
        float ta = A->of[i] + cx;
        if (ta >= 5.f) continue;
        for (j = 0; j < E->n; j++) {
            float te = E->of[j] + cx;
            if (te >= 5.f) continue;
            r += A->dl[i] * E->dl[j] * co_g(ta > te ? ta : te, C);
        }
    }
    return r;
}

/* (the table waves' filter: CO_FIR's taps for the clock's rate, ticks a sample) */
static void co_fir_taps(float rho, float *h, int v6)   /* (in the flash: a call a block; v6: the 6-voice state's, a
                                         * straight line over a tick at half the ticks) */
{
    const float (*T)[4] = v6 ? CO_FIR6 : CO_FIR;
    const int N = v6 ? CO_FIR6_N : CO_FIR_N;
    float x = (rho - (v6 ? CO_FIR6_R0 : CO_FIR_R0)) * (1.f / (v6 ? CO_FIR6_DR : CO_FIR_DR)), f;
    int i, k;
    if (x < 0.f) x = 0.f;
    if (x > (float)(N - 1)) x = (float)(N - 1);
    i = (int)x;
    if (i > N - 2) i = N - 2;
    f = x - (float)i;
    for (k = 0; k < 4; k++) h[k] = T[i][k] + f * (T[i + 1][k] - T[i][k]);
}
static inline __attribute__((always_inline)) float co_tab(const co_o *o, float t)
{
    float x, f;
    int j;
    t -= (float)sy_floori(t);
    x = t * (float)SY_WAVE_N;
    j = (int)x;
    if (j >= SY_WAVE_N) j = SY_WAVE_N - 1;
    f = x - (float)j;
    return o->ws * (o->wt[j] + f * (o->wt[j + 1] - o->wt[j]));
}
/* the smooth parts alone at phase t in [0, 1) (the sine, a table: co_nv's less its straight pieces) */
static inline __attribute__((always_inline)) float co_sm(const co_o *o, float t)
{
    float y = 0.f;
    if (o->sin) y = o->ws * sy_sin2pi(t);
    if (o->tab) y += co_tab(o, t);
    return y;
}

/* 1 - C(tau) for any tau (0 from 5 on) */
static inline __attribute__((always_inline)) float co_g5(float tau, const co_resp *C) { return tau >= 5.f ? 0.f : co_g(tau, C); }
/* the new wave m ticks before phase t as the oscillator has it after a start: its straight pieces' present segment
 * back (no step), the sine and the table as they were (in the flash: a few calls at a note's start) */
static float co_ext(const co_o *o, float t, int m, int rs)
{
    float y = SY_FAR(co_nv)(o, t, rs, 1), u = t - (float)m * o->a, s = 0.f;
    if (!o->on) return 0.f;
    if (o->saw) s += -2.f * o->wr;
    if (o->trl) s += (t < 0.25f || t >= 0.75f ? 4.f : -4.f) * o->tri;
    y -= (float)m * o->a * s;
    if (o->sin || o->tab) { u -= (float)sy_floori(u); y += SY_FAR(co_nv)(o, u, 0, 0) - SY_FAR(co_nv)(o, u, 0, 1); }
    return y;
}

/* the naive value m ticks before the tick of phase p (osc 1: across its last restart, its tick holding the restart's
 * value, the cycle before by its jump D) */
static float co_back(const co_o *o, uint32_t p, uint32_t inc, float D, int m)
{
    uint32_t q = p - (uint32_t)m * inc;
    if (o->r1 && D > 0.f) {
        uint32_t k = (p - (uint32_t)(CO_T0 * 4294967296.f)) / inc;   /* (ticks since the restart, whole when on its grid) */
        if ((uint32_t)m == k) return SY_FAR(co_nv)(o, CO_T0, 1, 0);
        if ((uint32_t)m > k) {
            float t = (float)p * U2F + D - (float)m * o->a;
            t -= (float)sy_floori(t);
            return SY_FAR(co_nv)(o, t, 0, 0);
        }
    }
    return SY_FAR(co_nv)(o, (float)q * U2F, 0, 0);
}

/* a sine's or a table's straight piece at the delayed time td: its value and slope a tick */
static inline __attribute__((always_inline)) void co_sline(const co_o *o, co_ev *R, float td, int w, float *sb, float *ss)
{
    *sb = *ss = 0.f;
    if (o->sin) {                       /* (a phasor turned a sample at each sample: w osc 1's restart, taken again) */
        if (w | (R->yn >= 16)) { co_sc(td, &R->ys, &R->yc); R->yn = 0; }
        else {
            const float yc = R->yc * o->pwc - R->ys * o->pws;
            R->ys = R->ys * o->pwc + R->yc * o->pws; R->yc = yc; R->yn++;
        }
        *sb = o->ws * R->ys; *ss = o->ws * 6.28318531f * o->a * R->yc;
    }
    if (o->tab) {
        float u = td - (float)sy_floori(td), x = u * (float)SY_WAVE_N, f;
        int j = (int)x;
        if (j >= SY_WAVE_N) j = SY_WAVE_N - 1;
        f = x - (float)j;
        *sb += o->ws * (o->wt[j] + f * (o->wt[j + 1] - o->wt[j]));
        *ss += o->ws * (o->wt[j + 1] - o->wt[j]) * (float)SY_WAVE_N * o->a;
    }
}
/* the block's setup (in the flash: a call a block): the oscillators' constants, the responses (K: the block's; K1,
 * K2 each oscillator's), the flags, what the clock's last ticks held (sdm, nsd: a start's or new weights' step), the
 * tables' filter and its past three samples; x0 the waves' naive mix at the first tick (the start transient's sign,
 * as before) */
/* (the tables' filter's response to a sine of f turns a sample: H, its value's gain, and G, the gain of its slope's
 * sum, h[1] (x[+1] - x[-1]) + 2 h[2] (..) + 3 h[3] (..), on the sine's cosine) */
static float co_fh(const float *h, float f)
{
    return h[0] + 2.f * (h[1] * sy_cos2pi(f) + h[2] * sy_cos2pi(2.f * f) + h[3] * sy_cos2pi(3.f * f));
}
static float co_fg(const float *h, float f)
{
    return 2.f * (h[1] * sy_sin2pi(f) + 2.f * h[2] * sy_sin2pi(2.f * f) + 3.f * h[3] * sy_sin2pi(3.f * f));
}
typedef struct {
    co_o o1, o2;
    co_k K, K1, K2;
    float sdm[6], vt[8], va[8], ve[8], h[4];   /* (the tables' filter: its last seven inputs, a ring; the ring
                                         * modulator's smooth parts) */
    float D, sg, sa, g1, g2, rgr, r, ea, eb;
    float qa1, qb1, qa2, qb2, qc, qd;   /* (co_rsmq: the tables' filter on its sines, by their frequencies) */
    uint32_t i1, i2;
    int ring, rsm, rf, tab, qs, qr, nsd, on1, on2;
} co_blk;
static void co_setup(co_blk *S, sy_voice_t *v, const sy_vblk_t *b, float *x0)
{
    const float r = b->tk_r, g1 = b->g1, g2 = b->g2, rg = b->ring_g, rgr = rg * b->o1.ring * b->o2.ring, q = b->co_q;
    const uint32_t i1 = b->tk_inc1, i2 = b->tk_inc2;
    const float c = v->tk_c;
    const uint32_t p1 = v->ph1, p2 = v->ph2;
    co_o *o1 = &S->o1, *o2 = &S->o2;
    co_k *K = &S->K;
    float sma;
    int i;
    S->on1 = g1 != 0.f || rg != 0.f; S->on2 = g2 != 0.f || rg != 0.f;
    S->g1 = g1; S->g2 = g2; S->rgr = rgr; S->r = r; S->ea = b->tk_ea; S->eb = b->tk_eb; S->i1 = i1; S->i2 = i2;
    S->D = v->tk_D1; S->sg = 1e9f;
    K->q = q;
    K->ex = q > 0.f ? CO_GC_L - 2.5f : 0.f;      /* (whole ticks) */
    K->lat = CO_LAT + K->ex; K->latn = K->lat;
    K->lim = q > 0.f ? 2.f * CO_GC_L : 5.f;
    K->rr = r + r;
    K->C = &CO_R[b->co_v6 ? 1 : 0];     /* (the 6-voice state: osc 1's line; osc 2, the ring and a start: below) */
    {   /* the sine's gain by the phase a tick: the response's taps (C's steps) at its frequency */
        const co_resp *C2 = &CO_R[b->co_v6 ? 2 : 0];
        float a1 = (float)i1 * U2F, a2 = (float)i2 * U2F;
        sma = K->C->ft[2] + 2.f * K->C->ft[1] * sy_cos2pi(a1) + 2.f * K->C->ft[0] * sy_cos2pi(2.f * a1);
        co_init(o1, &b->o1, i1, 1, S->on1, sma);
        sma = C2->ft[2] + 2.f * C2->ft[1] * sy_cos2pi(a2) + 2.f * C2->ft[0] * sy_cos2pi(2.f * a2);
        co_init(o2, &b->o2, i2, 0, S->on2, sma);
    }
    if (o1->sin) { o1->pwc = sy_cos2pi(2.f * r * o1->a); o1->pws = sy_sin2pi(2.f * r * o1->a); o1->p6c = sy_cos2pi(6.f * r * o1->a); o1->p6s = sy_sin2pi(6.f * r * o1->a); }
    if (o2->sin) { o2->pwc = sy_cos2pi(2.f * r * o2->a); o2->pws = sy_sin2pi(2.f * r * o2->a); o2->p6c = sy_cos2pi(6.f * r * o2->a); o2->p6s = sy_sin2pi(6.f * r * o2->a); }
    S->ring = rg != 0.f;
    S->K1 = S->K2 = *K;                 /* (CLEAN: the narrow pulse or the ring in a cycle shorter than the window keeps
                                         * the measured response, as late; the saw and the pulse take it to the top) */
    if (b->co_v6) {                     /* (the 6-voice state: osc 1 its line, osc 2 its own taps; the ring's product
                                         * and a start's step osc 2's: the ring read 0.0-0.4 dB off below 10 kHz that
                                         * way, osc 1's put it 1-3 dB bright from 8 kHz on) */
        S->K1.C = &CO_R[1];
        if (!S->ring) {                 /* (the steps' windows shorter, the CPU: osc 1's line is over at 3 ticks, the
                                         * same values; osc 2's response cut at 4 ticks, its last tap (-0.03) dropped:
                                         * its lines within 0.3 dB of the full response's, as far from the
                                         * reference; the ring weighs its steps by the full one) */
#if CO_V6_LAT1
            S->K1.C = &CO_R[3];         /* (osc 1 two ticks earlier than osc 2: its window one tick) */
            S->K1.lat = S->K1.latn = CO_R[3].lat + K->ex;
            S->K1.lim = 1.f;
#else
            S->K1.lim = 3.f;
#endif
            S->K2.lim = 4.f;
        }
        S->K2.C = K->C = &CO_R[2];
    }
    if (q > 0.f && (o1->npl || rg != 0.f) && o1->a * (K->lim + 4.f) > 1.f) { S->K1.q = 0.f; S->K1.lim = 5.f + K->ex; }
    if (q > 0.f && (o2->pul0 || rg != 0.f) && o2->a * (K->lim + 4.f) > 1.f) { S->K2.q = 0.f; S->K2.lim = 5.f + K->ex; }
    co_quiet(o1, &S->K1); co_quiet(o2, &S->K2);
    S->rsm = S->ring && (o1->sin || o1->tab || o2->sin || o2->tab);   /* (the ring with a sine or a table: those
                                         * products through the tables' filter) */
    S->rf = o1->ia >= 40.f && o2->ia >= 40.f;   /* (the smooth parts' cross products filtered: cycles of 40 ticks or
                                         * more) */
    S->tab = o1->tab || o2->tab || S->rsm;
    S->qs = (o1->fast || !o1->on) && (o2->fast || !S->on2) && !S->rsm && !S->tab && (o1->on || S->on2) && !o1->sin
        && !o2->sin;                    /* (the quiet stretches: the naive waves alone, the ring their product) */
    S->qr = (S->rsm && o1->fast && o2->fast && o1->wrap && !o1->tab && !o2->tab && !o1->npl && !o2->npl && o1->wp == 0.f
             && o2->wp == 0.f)          /* (the ring with sines, no step near: co_rsmq) */
        || (!S->ring && !S->tab && (o1->sin || o2->sin) && o1->on && o1->fast && o1->ne && (o2->fast || !S->on2));   /* (the
                                         * plain mix with a sine) */
    {   /* what the clock's last ticks held: a phase set from outside (a note's start) is a step from the last tick's
         * value to the new wave's at the next tick (and the steps the new phase had before are not); new weights (a
         * shape, a level) a step from the last tick's value to the new weights' there */
        uint32_t q1 = p1 + i1;
        int rs = q1 < p1, m;
        *x0 = g1 * co_naive(o1, q1, 0) + g2 * co_naive(o2, p2 + i2, 0);
        S->nsd = 0;
        if (p1 != v->tk_pe1 || p2 != v->tk_pe2) {   /* (the new wave's past as the oscillator has it: its present
                                         * segment back, the steps before the start not taken) */
            float t1 = (rs ? CO_T0 : (float)q1 * U2F), t2 = (float)(p2 + i2) * U2F;
            for (m = 1; m <= 6; m++) {
                float a = co_ext(o1, t1, m, rs), e = co_ext(o2, t2, m, 0);
                S->sdm[m - 1] = v->tk_xh[m - 1] - (g1 * a + g2 * e - rgr * a * e);
            }
            S->nsd = 6;
            S->sg = c - 1.f;
            S->D = 0.f;                 /* (osc 1 off its clock until its next restart) */
        } else if (g1 != v->tk_g1 || g2 != v->tk_g2 || b->o1.ws != v->tk_w1 || b->o2.ws != v->tk_w2
                   || b->o1.wsaw != v->tk_s1 || b->o2.wsaw != v->tk_s2 || b->o1.c != v->tk_c1 || b->o2.c != v->tk_c2
                   || b->o1.d != v->tk_d1 || b->o2.d != v->tk_d2 || b->o1.tkf != v->tk_f1 || b->o2.tkf != v->tk_f2) {
            for (m = 1; m <= 6; m++) {  /* (new weights: their wave at the old ticks' phases) */
                float a = co_back(o1, p1, i1, S->D, m - 1), e = co_back(o2, p2, i2, 1.f, m - 1);
                S->sdm[m - 1] = v->tk_xh[m - 1] - (g1 * a + g2 * e - rgr * a * e);
            }
            S->nsd = 6;
        }
        S->sa = c - 1.f;
    }
    if (S->tab) {                       /* the table waves' past three samples (the filter's), from the phases now */
        float ra = 2.f * r, t1 = (float)p1 * U2F, t2 = (float)p2 * U2F;
        co_fir_taps(ra, S->h, b->co_v6);   /* (both oscillators' tables: osc 1's response, ASSUMPTION for osc 2) */
        for (i = 0; i < 6; i++) {
            float dc = c + ra * (float)(i - 2) - K->lat, u, u1, u2;
            u1 = t1 + dc * o1->a; u1 -= (float)sy_floori(u1); S->va[(i + 5) & 7] = S->rsm ? co_sm(o1, u1) : 0.f;
            u2 = t2 + dc * o2->a; u2 -= (float)sy_floori(u2); S->ve[(i + 5) & 7] = S->rsm ? co_sm(o2, u2) : 0.f;
            u = 0.f;
            if (o1->tab) u += g1 * co_tab(o1, u1);
            if (o2->tab) u += g2 * co_tab(o2, u2);
            S->vt[(i + 5) & 7] = u - (S->rsm ? rgr * S->va[(i + 5) & 7] * S->ve[(i + 5) & 7] : 0.f);
        }
    }
    if (S->qr && S->ring) {             /* (co_rsmq: in a quiet stretch the filter's inputs are its sines, the filter's
                                         * output a sine's times the filter's response at its frequency: H the even
                                         * taps' (a sine's value), G the odd sums' (its slope), the product's by the
                                         * sum and the difference of the two) */
        const float w1 = o1->sin ? o1->ws : 0.f, w2 = o2->sin ? o2->ws : 0.f, f1 = 2.f * r * o1->a, f2 = 2.f * r * o2->a;
        const float *h = S->h, hd = co_fh(h, f1 - f2), hp = co_fh(h, f1 + f2);
        S->qa1 = w1 * co_fh(h, f1); S->qb1 = 2.f * r * w1 * co_fg(h, f1);
        S->qa2 = w2 * co_fh(h, f2); S->qb2 = 2.f * r * w2 * co_fg(h, f2);
        S->qc = -rgr * w1 * w2 * 0.5f * (hd - hp); S->qd = -rgr * w1 * w2 * 0.5f * (hd + hp);
    }
}
/* the tables' filter's sample (the table waves; the ring with a sine or a table, rsm: the straight pieces' product
 * (co_ring) with the smooth parts as lines, less what the filter takes: the smooth parts' product 3 samples ahead,
 * into va, ve, and each smooth part filtered with the other's present line, cycles of 40 ticks or more, rf): its input
 * 3 samples ahead in, the filter's output for this sample with the ring's part taken off by rgr */
static SY_HOT __attribute__((noinline)) float co_tabx(co_blk *S, co_ev *A, co_ev *E, float t1, float t2, float c, int i,
                                                    int w1)
{
    const co_o *o1 = &S->o1, *o2 = &S->o2;
    const float r = S->r, dc = c + 6.f * r - S->K1.lat;
    float *va = S->va, *ve = S->ve, *vt = S->vt, *h = S->h;
    float u1 = t1 + dc * o1->a, u2 = t2 + dc * o2->a, b1 = 0.f, b2 = 0.f, y = 0.f, vr = 0.f;
    u1 -= (float)sy_floori(u1); u2 -= (float)sy_floori(u2);
    if (o1->tab) b1 = co_tab(o1, u1);   /* (the tables 3 samples on) */
    if (o2->tab) b2 = co_tab(o2, u2);
    if (S->rsm) {
        float sb1, ss1, sb2, ss2, x, sa0, sa1, se0, se1;
        const float kc = S->K.C->s2 + c * (1.f - c);
        co_sline(o1, A, t1 + (c - S->K1.lat) * o1->a, w1, &sb1, &ss1);
        co_sline(o2, E, t2 + (c - S->K2.lat) * o2->a, 0, &sb2, &ss2);
        {
            const float ab = A->b + sb1, as = A->s + ss1, eb = E->b + sb2, es = E->s + ss2;
            const float al = A->b + A->fs, el = E->b + E->fs;   /* (the lines with the steps still to come taken, as
                                         * the smooth parts' products have them) */
            x = co_ring(A, E, ab, as, eb, es, c, &S->K);
            if (S->rf) x -= ((al + sb1) * (el + sb2) - al * el) + (as * es - A->s * E->s) * kc;
            else x -= sb1 * sb2 + ss1 * ss2 * kc;   /* (short cycles: the smooth parts' line through the step response
                                         * as it is) */
        }
        va[(i + 3) & 7] = (o1->sin ? o1->ws * (A->ys * o1->p6c + A->yc * o1->p6s) : 0.f) + b1;   /* (the sines 3
                                         * samples on from the phasors) */
        ve[(i + 3) & 7] = (o2->sin ? o2->ws * (E->ys * o2->p6c + E->yc * o2->p6s) : 0.f) + b2;
        vr = va[(i + 3) & 7] * ve[(i + 3) & 7];
        if (S->rf) {                    /* (each smooth part filtered with the other's present line: the line's value
                                         * and slope against the filter's taps and their moments) */
            const int i1 = (i + 1) & 7, i2 = (i + 2) & 7, i3 = (i + 3) & 7, j1 = (i - 1) & 7, j2 = (i - 2) & 7, j3 = (i - 3) & 7;
            const float h2 = 2.f * h[2], h3 = 3.f * h[3];
            sa0 = h[0] * va[i & 7] + h[1] * (va[j1] + va[i1]) + h[2] * (va[j2] + va[i2]) + h[3] * (va[j3] + va[i3]);
            sa1 = h[1] * (va[i1] - va[j1]) + h2 * (va[i2] - va[j2]) + h3 * (va[i3] - va[j3]);
            se0 = h[0] * ve[i & 7] + h[1] * (ve[j1] + ve[i1]) + h[2] * (ve[j2] + ve[i2]) + h[3] * (ve[j3] + ve[i3]);
            se1 = h[1] * (ve[i1] - ve[j1]) + h2 * (ve[i2] - ve[j2]) + h3 * (ve[i3] - ve[j3]);
            x += sa0 * (E->b + E->fs) + 2.f * r * sa1 * E->s + se0 * (A->b + A->fs) + 2.f * r * se1 * A->s;
        }
        y = -S->rgr * x;
    }
    vt[(i + 3) & 7] = (S->g1 * b1 + S->g2 * b2) - S->rgr * vr;   /* (v(i + 3) in, the filter's output for sample i) */
    return y + h[0] * vt[i & 7] + h[1] * (vt[(i - 1) & 7] + vt[(i + 1) & 7]) + h[2] * (vt[(i - 2) & 7] + vt[(i + 2) & 7])
        + h[3] * (vt[(i - 3) & 7] + vt[(i + 3) & 7]);
}

/* (CLEANER, 1.18C25) a quiet stretch of the ring with sines (S->qr: saws and sines, no step's window open for either
 * oscillator, co_span's n samples from sample i): co_step's, co_tabx's and the mix's values sample by sample as they
 * give them there, without the calls (the records hold no step: the pieces a ramp; the sines' phasors as co_sinn and
 * co_sline turn them); the clock, the phases and the emphasis' state in and out through the pointers. The tables'
 * filter on the sines (co_tabx's va, ve, vt): its first six samples through the taps (the filter still holds samples
 * from before the stretch, a restart's), then by the filter's response at the sines' frequencies (S->qa1 .. qd,
 * co_setup: the same within a float's rounding); the filter's inputs kept for co_tabx in its last six */
static SY_HOT __attribute__((noinline)) void co_rsmq(co_blk *S, co_ev *A, co_ev *E, float *out, int i, int n, float *pc,
                                                   uint32_t *pp1, uint32_t *pp2, float *pem, uint32_t *pn1)
{
    const co_o *o1 = &S->o1, *o2 = &S->o2;
    const float r = S->r, g1 = S->g1, g2 = S->g2, rgr = S->rgr, *h = S->h, eb = S->eb, ea = S->ea, cs2 = S->K.C->s2;
    const float s1 = -2.f * o1->wr * o1->a, s2 = -2.f * o2->wr * o2->a, d1 = 2.f * o1->wr * o1->a * (r + r),
        d2 = 2.f * o2->wr * o2->a * (r + r);
    const float pc1 = o1->prc, ps1 = o1->prs, pc2 = o2->prc, ps2 = o2->prs, sg1 = o1->sg, sg2 = o2->sg;
    const uint32_t i1 = S->i1, i2 = S->i2;
    float c = *pc, em = *pem, *va = S->va, *ve = S->ve, *vt = S->vt;
    float ks1 = A->ks, kc1 = A->kc, ks2 = E->ks, kc2 = E->kc, ys1 = A->ys, yc1 = A->yc, ys2 = E->ys, yc2 = E->yc;
    int kn1 = A->kn, kn2 = E->kn, yn1 = A->yn, yn2 = E->yn;
    uint32_t p1 = *pp1, p2 = *pp2, n1 = 0;
    float z1 = co_qv(o1, (float)p1 * U2F, c + r + r, &S->K1), z2 = S->on2 ? co_qv(o2, (float)p2 * U2F, c + r + r, &S->K2) : 0.f;
    const int k1 = o1->sin && g1 != 0.f, k2 = o2->sin && g2 != 0.f, y1 = o1->sin, y2 = o2->sin, rf = S->rf, ring = S->ring;
    const int e = i + n, ex = i + 6, eo = e - 6;
    {
        const float w1 = o1->ws, w2 = o2->ws, wc1 = w1 * 6.28318531f * o1->a, wc2 = w2 * 6.28318531f * o2->a;
        const float pw1 = o1->pwc, qw1 = o1->pws, pw2 = o2->pwc, qw2 = o2->pws, p61 = o1->p6c, q61 = o1->p6s,
            p62 = o2->p6c, q62 = o2->p6s;
        const float qa1 = S->qa1, qb1 = S->qb1, qa2 = S->qa2, qb2 = S->qb2, qc = S->qc, qd = S->qd;
        for (; i < e; i++) {
            float y = 0.f, x, sb1 = 0.f, ss1 = 0.f, sb2 = 0.f, ss2 = 0.f, kc;
            int nt = 0;
            c += r; if (c >= 1.f) { c -= 1.f; nt++; }
            c += r; if (c >= 1.f) { c -= 1.f; nt++; }
            if (nt) { p1 += i1; p2 += i2; if (nt > 1) { p1 += i1; p2 += i2; } }
            n1 += (uint32_t)nt;
            if (k1) {
                if (kn1 + nt > 16) { co_sc((float)p1 * U2F - S->K1.latn * o1->a, &ks1, &kc1); kn1 = 0; }
                else {
                    int k;
                    for (k = 0; k < nt; k++) { const float q = kc1 * pc1 - ks1 * ps1; ks1 = ks1 * pc1 + kc1 * ps1; kc1 = q; }
                    kn1 += nt;
                }
                y += g1 * (z1 + sg1 * (ks1 + c * ((ks1 * pc1 + kc1 * ps1) - ks1)));
            } else y += g1 * z1;
            if (k2) {
                if (kn2 + nt > 16) { co_sc((float)p2 * U2F - S->K2.latn * o2->a, &ks2, &kc2); kn2 = 0; }
                else {
                    int k;
                    for (k = 0; k < nt; k++) { const float q = kc2 * pc2 - ks2 * ps2; ks2 = ks2 * pc2 + kc2 * ps2; kc2 = q; }
                    kn2 += nt;
                }
                y += g2 * (z2 + sg2 * (ks2 + c * ((ks2 * pc2 + kc2 * ps2) - ks2)));
            } else y += g2 * z2;
            if (!ring) goto put;        /* (the plain mix: the sines at the ticks) */
            if (y1) {                   /* (co_sline's) */
                if (yn1 >= 16) { co_sc((float)p1 * U2F + (c - S->K1.lat) * o1->a, &ys1, &yc1); yn1 = 0; }
                else { const float q = yc1 * pw1 - ys1 * qw1; ys1 = ys1 * pw1 + yc1 * qw1; yc1 = q; yn1++; }
            }
            if (y2) {
                if (yn2 >= 16) { co_sc((float)p2 * U2F + (c - S->K1.lat) * o2->a, &ys2, &yc2); yn2 = 0; }
                else { const float q = yc2 * pw2 - ys2 * qw2; ys2 = ys2 * pw2 + yc2 * qw2; yc2 = q; yn2++; }
            }
            kc = cs2 + c * (1.f - c);
            if (rf) x = z1 * z2 + s1 * s2 * kc;   /* (co_tabx's: co_ring with no step, the lines with no step still
                                         * to come, less the sines' products the filter gives: the ramps' alone) */
            else {
                if (y1) { sb1 = w1 * ys1; ss1 = wc1 * yc1; }
                if (y2) { sb2 = w2 * ys2; ss2 = wc2 * yc2; }
                x = (z1 + sb1) * (z2 + sb2) + 2.f * (s1 + ss1) * (s2 + ss2) * (0.5f * cs2 + 0.5f * c * (1.f - c))
                    - (sb1 * sb2 + ss1 * ss2 * kc);
            }
            if (i < ex || i >= eo) {    /* (the filter's inputs: through its taps, or kept for co_tabx) */
                const int i3 = (i + 3) & 7;
                const float vr = (va[i3] = y1 ? w1 * (ys1 * p61 + yc1 * q61) : 0.f) * (ve[i3] = y2 ? w2 * (ys2 * p62 + yc2 * q62) : 0.f);
                vt[i3] = -rgr * vr;
                if (i < ex) {
                    const int i0 = i & 7;
                    float sa0 = h[0] * va[i0], sa1 = 0.f, se0 = h[0] * ve[i0], se1 = 0.f, st = h[0] * vt[i0];
                    int k;
                    for (k = 1; k <= 3; k++) {
                        const int a = (i + k) & 7, b = (i - k) & 7;
                        const float hk = h[k], hm = (float)k * hk;
                        sa0 += hk * (va[b] + va[a]); sa1 += hm * (va[a] - va[b]);
                        se0 += hk * (ve[b] + ve[a]); se1 += hm * (ve[a] - ve[b]);
                        st += hk * (vt[b] + vt[a]);
                    }
                    if (rf) x += sa0 * z2 + 2.f * r * sa1 * s2 + se0 * z1 + 2.f * r * se1 * s1;
                    y += st - rgr * x;
                    goto put;
                }
            }
            if (rf) x += qa1 * ys1 * z2 + qb1 * yc1 * s2 + qa2 * ys2 * z1 + qb2 * yc2 * s1;
            y += (qc * yc1 * yc2 + qd * ys1 * ys2) - rgr * x;
        put:
            out[i] = eb * y - ea * em;
            em = y;
            z1 -= d1; z2 -= d2;
        }
    }
    A->ks = ks1; A->kc = kc1; E->ks = ks2; E->kc = kc2; A->ys = ys1; A->yc = yc1; E->ys = ys2; E->yc = yc2;
    A->kn = (uint8_t)kn1; E->kn = (uint8_t)kn2; A->yn = (uint8_t)yn1; E->yn = (uint8_t)yn2;
    *pc = c; *pem = em; *pp1 = p1; *pp2 = p2; *pn1 = n1;
}

/* the block's end (in the flash): the last tick's phases and their naive mix (a start's step from it), the weights */
static void co_save(const co_o *o1, const co_o *o2, sy_voice_t *v, const sy_vblk_t *b, uint32_t p1, uint32_t p2, float D)
{
    const float g1 = b->g1, g2 = b->g2, rgr = b->ring_g * b->o1.ring * b->o2.ring;
    int m;
    const int n1 = g1 != 0.f || rgr != 0.f, n2 = g2 != 0.f || rgr != 0.f;   /* (an oscillator at level 0 outside a ring
                                         * adds nothing) */
    for (m = 0; m < 6; m++) {
        float a = n1 ? co_back(o1, p1, b->tk_inc1, D, m) : 0.f, e = n2 ? co_back(o2, p2, b->tk_inc2, 1.f, m) : 0.f;
        v->tk_xh[m] = g1 * a + g2 * e - rgr * a * e;
    }
    v->tk_g1 = g1; v->tk_g2 = g2; v->tk_w1 = b->o1.ws; v->tk_w2 = b->o2.ws; v->tk_s1 = b->o1.wsaw; v->tk_s2 = b->o2.wsaw;
    v->tk_c1 = b->o1.c; v->tk_c2 = b->o2.c; v->tk_d1 = b->o1.d; v->tk_d2 = b->o2.d; v->tk_f1 = b->o1.tkf; v->tk_f2 = b->o2.tkf;
}

/* the block: x0 gets the waves' naive mix at its first tick (the start transient's sign, as before); scr: the block's
 * values and the records (co_scr) in the voice's buffers osc 1 and 2 leave unused on the clock (o1b, o2b: not on the
 * stack, the second core's is small) */
typedef struct { co_blk S; co_ev A, E; } co_scr;
typedef char co_scr_fits[sizeof(co_scr) <= 2u * SY_BLOCK * sizeof(float) ? 1 : -1];
static SY_HOT __attribute__((noinline)) void cln_osc(sy_voice_t *v, const sy_vblk_t *b, float *out, float *x0, void *scr)
{
    const float r = b->tk_r, g1 = b->g1, g2 = b->g2, rgr = b->ring_g * b->o1.ring * b->o2.ring;
    const float ea = b->tk_ea, eb = b->tk_eb;
    const uint32_t i1 = b->tk_inc1, i2 = b->tk_inc2, T0 = (uint32_t)(CO_T0 * 4294967296.f);
    co_scr *const Z = (co_scr *)scr;
    float c = v->tk_c, em = v->tk_em, D, sa, sg;
    uint32_t p1 = v->ph1, p2 = v->ph2;
    int i, nsd, ring, rsm, tab, qs, on2, qk = 0;
#define S (Z->S)
#define A (Z->A)                        /* (each oscillator's record: co_lin's at a step's birth, a sample older at each
                                         * sample between) */
#define E (Z->E)
#define o1 S.o1
#define o2 S.o2
#define K S.K
#define K1 S.K1
#define K2 S.K2
#define sdm S.sdm
    SY_FAR(co_setup)(&S, v, b, x0);
    D = S.D; sa = S.sa; sg = S.sg; nsd = S.nsd; ring = S.ring; rsm = S.rsm; tab = S.tab; qs = S.qs; on2 = S.on2;
    A.n = A.nc = E.n = E.nc = A.ovf = E.ovf = A.zs = E.zs = A.nf = E.nf = 0; A.fs = E.fs = A.fa = E.fa = 0.f; A.st = E.st = 1; A.xs = E.xs = 0.f;
    A.kn = A.yn = E.kn = E.yn = 99;   /* (the sines' phasors: taken at their first use) */
    A.b = A.s = A.bc = A.bk = E.b = E.s = E.bc = E.bk = 0.f;
    for (i = 0; i < SY_BLOCK; i++) {
        float y = 0.f, t1, t2;
        int k, w1 = 0, w2 = 0;
        float nt = 0.f;
        if (S.qr && i >= qk && (!nsd || sa - K.ex >= 6.f)) {   /* the ring with sines: its quiet stretches in co_rsmq */
            int n = co_span(&o1, p1, c, r, SY_BLOCK - i);
            if (n > 1) n = co_span(&o2, p2, c, r, n);
            if (n < 0) qk = i - n;
            if (n > 1) {
                uint32_t n1;
                co_rsmq(&S, &A, &E, out, i, n, &c, &p1, &p2, &em, &n1);
                sa += (float)n * (r + r); sg += (float)n * (r + r);
                i += n - 1;
                co_skip(&A, &K1, (float)n1); co_skip(&E, &K2, (float)n1);
                continue;
            }
        }
        if (qs && i >= qk && (!nsd || sa - K.ex >= 6.f)) {   /* a quiet stretch of both: the naive waves alone, their
                                         * ramps a step a sample (the ring: their product, the curvature of the two
                                         * ramps), the emphasis with them */
            int n = co_span(&o1, p1, c, r, SY_BLOCK - i);
            if (on2 && n > 1) n = co_span(&o2, p2, c, r, n);
            if (n < 0) qk = i - n;      /* (a window open: no stretch before it closes) */
            if (n > 1) {
                float z1 = o1.on ? co_qv(&o1, (float)p1 * U2F, c + r + r, &K1) : 0.f;
                float z2 = on2 ? co_qv(&o2, (float)p2 * U2F, c + r + r, &K2) : 0.f;
                const float d1 = 2.f * o1.wr * o1.a * (r + r), d2 = 2.f * o2.wr * o2.a * (r + r);
                uint32_t n1 = 0;
                int j;
                if (ring) {
                    const float kc = 4.f * o1.wr * o1.a * o2.wr * o2.a, cs2 = K.C->s2;   /* (co_ring with no step: the product and the
                                         * two ramps' curvature; both oscillators on: no restart) */
                    for (j = 0; j < n; j++) {
                        float y;
                        c += r;
                        if (c >= 1.f) { c -= 1.f; n1++; }
                        c += r;
                        if (c >= 1.f) { c -= 1.f; n1++; }
                        y = g1 * z1 + g2 * z2 - rgr * (z1 * z2 + kc * (cs2 + c * (1.f - c)));
                        out[i + j] = eb * y - ea * em;
                        em = y;
                        z1 -= d1; z2 -= d2;
                    }
                    p1 += n1 * i1;
                } else {
                    float z = g1 * z1 + g2 * z2;
                    const float dz = g1 * d1 + g2 * d2;
                    if (o1.on && o1.wrap) {     /* (osc 1 on with its restart's window: no restart in the stretch) */
                        for (j = 0; j < n; j++) {
                            c += r;
                            if (c >= 1.f) { c -= 1.f; n1++; }
                            c += r;
                            if (c >= 1.f) { c -= 1.f; n1++; }
                            out[i + j] = eb * z - ea * em;
                            em = z;
                            z -= dz;
                        }
                        p1 += n1 * i1;
                    } else {
                        for (j = 0; j < n; j++) {
                            for (k = 0; k < 2; k++) {
                                c += r;
                                if (c >= 1.f) {
                                    uint32_t qn = p1 + i1;
                                    c -= 1.f;
                                    if (qn < p1) {      /* (osc 1 silent: its restart still keeps the phases handed
                                         * over) */
                                        D = 1.f + ((float)qn - (float)T0) * U2F;
                                        p1 = T0;
                                    } else p1 = qn;
                                    n1++;
                                }
                            }
                            out[i + j] = eb * z - ea * em;
                            em = z;
                            z -= dz;
                        }
                    }
                }
                p2 += n1 * i2;                  /* (osc 2: no wrap in a quiet stretch) */
                sa += (float)n * (r + r); sg += (float)n * (r + r);
                i += n - 1;
                co_skip(&A, &K1, (float)n1); co_skip(&E, &K2, (float)n1);   /* (the records: the stretch's ticks older) */
                continue;
            }
        }
        sa += r + r; sg += r + r;       /* (the step's age, ticks) */
        for (k = 0; k < 2; k++) {       /* the clock: two passes a sample, at most a tick each */
            c += r;
            if (c >= 1.f) {
                uint32_t qn = p1 + i1;
                c -= 1.f;
                if (qn < p1) {          /* osc 1's restart: its jump, a cycle and the overshoot */
                    D = 1.f + ((float)qn - (float)T0) * U2F;
                    p1 = T0;
                    w1 = 1;
                } else p1 = qn;
                nt += 1.f;
                qn = p2 + i2;
                w2 |= qn < p2;
                p2 = qn;
            }
        }
        t1 = (float)p1 * U2F; t2 = (float)p2 * U2F;
        if (o1.on) {                    /* the records and the values (a level 0: the ring's record alone) */
            float y1;
            if (!co_quick(&o1, &A, &K1, t1, c, nt, w1, &y1)) y1 = co_step(&o1, &A, &K1, t1, c, D, sg, nt, w1, g1 != 0.f);
            if (g1 != 0.f) y += g1 * y1;
        }
        if (o2.on) {
            float y2;
            if (!co_quick(&o2, &E, &K2, t2, c, nt, w2, &y2)) y2 = co_step(&o2, &E, &K2, t2, c, 1.f, sg, nt, w2, g2 != 0.f);
            if (g2 != 0.f) y += g2 * y2;
        }
        if (ring && !rsm)               /* the straight pieces' product exactly, each step scaled by the other wave (with
                                         * a sine or a table: co_tabx) */
            y -= rgr * co_ring(&A, &E, A.b, A.s, E.b, E.s, c, &K);
        if (nsd && sa - K.ex < 6.f) {   /* (the old ticks' values against the new wave's, through each tick's own
                                         * response: C(tau) - C(tau - 1)) */
            float ta = sa - K.ex, gp = co_g5(ta, K.C);
            int m;
            for (m = 1; m <= 6; m++) {
                float gm = co_g5(ta + (float)m, K.C);
                y += sdm[m - 1] * (gp - gm);
                gp = gm;
            }
        }
        if (tab) y += co_tabx(&S, &A, &E, t1, t2, c, i, w1);   /* (the tables' filter; the ring with a sine or a
                                         * table) */
        out[i] = eb * y - ea * em;      /* the emphasis */
        em = y;
    }
    SY_FAR(co_save)(&o1, &o2, v, b, p1, p2, D);
    v->tk_c = c; v->tk_em = em; v->tk_D1 = D;
    v->ph1 = p1; v->ph2 = p2; v->tk_pe1 = p1; v->tk_pe2 = p2;
#undef o1
#undef o2
#undef K
#undef K1
#undef K2
#undef sdm
#undef S
#undef A
#undef E
}
