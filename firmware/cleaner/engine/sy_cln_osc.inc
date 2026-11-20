/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef CO_FAST
#define CO_FAST 1                       /* CLEANER (CPU, 1.18C32): the own oscillator's per-sample path made cheaper:
                                         * saws and pulses by events, co_runp; the rest per sample, co_rung; cpu-plan P1, P2;
                                         * 0: the 1.18C31 code (sy_cln_osc_ref.inc) */
#endif
#if !CO_FAST
#include "sy_cln_osc_ref.inc"
#else
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
#define CO_RESTRICT restrict            /* (the block's output: not the records' buffer, scr) */
#ifndef CO_PTRI
#define CO_PTRI 1                       /* (CLEANER, cpu-p1b) the triangle and the narrow pulse by events too (co_runp: a
                                         * corner's residual pushed at its tick); 0: per sample (co_rung) */
#endif
#ifndef CO_PCL
#define CO_PCL 1                        /* (CLEANER, cpu-p1b) CLEAN (osc_clean 1, no ring) by events too: co_runp with the
                                         * smooth step's residuals pushed (co_pushq); 0: per sample (co_rung) */
#endif
#ifndef CO_PTAB
#define CO_PTAB 1                       /* (CLEANER, cpu-p1b) the table waves (no ring) by events too: co_runp with the
                                         * tables' filter a sample (co_tabp); 0: per sample (co_rung) */
#endif
#ifndef CO_PKF
#define CO_PKF 1                        /* (CLEANER, cpu-p1b) co_pev: the steps still to come looked at only from the first
                                         * tick one may reach (kf, kept per oscillator) or at a new record */
#endif
#ifndef CO_PSIN
#define CO_PSIN 1                       /* (CLEANER, cpu-cave) the sines (no table) by events too, the ring with them: co_runs
                                         * (co_runp's events and pushed steps, the sines a sample from their phasors, the
                                         * ring's sine terms by the pushed steps' sums); 0: per sample (co_rung) */
#endif
#ifndef CO_RAMH
#define CO_RAMH 3                       /* (CLEANER, 1.18C35) the small event helpers called many times a block (co_nv,
                                         * co_back, co_ftap, co_sinw, co_sinr: up to ~150 calls a block with sines and
                                         * the ring) in the RAM code (.ram_text, CO_RH) instead of the flash: the room
                                         * cpu-filt freed; calls from the RAM code to them direct (CO_RD), from the flash
                                         * through SY_FAR (CO_FD); co_runs keeps its SY_FAR calls (direct ones made it
                                         * 282 B longer). The same code: the same sound (the moved functions' instructions
                                         * as in the flash, their callers' float operations the same). co_fh (a block's
                                         * setup) did not fit. Bit 0: co_nv, co_back; bit 1:
                                         * co_sinr, co_sinw, co_ftap; 0: all in the flash as 1.18C34 */
#endif
#if CO_RAMH & 1                         /* (bit 0: co_nv, co_back; bit 1: co_sinr, co_sinw, co_ftap) */
#define CO_RH SY_HOT
#define CO_RD(fn) fn
#define CO_FD(fn) SY_FAR(fn)
#else
#define CO_RH
#define CO_RD(fn) SY_FAR(fn)
#define CO_FD(fn) fn
#endif
#if CO_RAMH & 2
#define CO_RHS SY_HOT
#define CO_RDS(fn) fn
#else
#define CO_RHS
#define CO_RDS(fn) SY_FAR(fn)
#endif
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
    float te[3], qw, tf, ql, idps;              /* (the quiet stretches: ne steps' phases te, the window qw after each, from a
                                         * tick before; the pulse's fall tf) */
    uint8_t r1;                         /* osc 1 (the restart: its jump D; the pulse's rise a tick after it) */
    uint8_t saw, pul, pul0, npl, trl, sin, tab, wrap, on;   /* its parts: the ramp, the pulse on the ticks (tkn > 0),
                                         * the pulse as it is (tkn 0), the narrow pulse, the triangle, the sine, a table */
    uint8_t fast, ne, sgn, lin;        /* (fast: no step within the response's window, the naive wave alone; sgn: co_sig
                                         * has pieces; lin: straight pieces) */
} co_o;
#define CO_NR 6                         /* (a record's steps, corners: more only in cycles of a tick or two, co_lin's own
                                         * value then, each sample) */
#define CO_NC 3
#define CO_NF 4
typedef struct {
    float b, s, bc, bk;                 /* the straight pieces (every recorded step taken) at the sample, their slope a
                                         * tick; bc + bk td (td the delayed phase) */
    uint8_t n, nc, ovf, zs, st, nf, kn, yn;   /* (zs: co_sig's at the record, st: none taken yet; nf steps still to come) */
    float fa, fm, fs;                     /* (the steps still to come: their ages less fa, the ticks since the record; fm
                                         * the latest's; their sizes are off bc, as the naive wave has them) */
    float fo[CO_NF], fd[CO_NF];
    float pr, xs, kc, ks, yc, ys;       /* (kc, ks: a sine at the tick, yc, ys: at the delayed time, co_sinn's, co_rsm's
                                         * phasors, taken exactly every 16 turns) */
                                        /* (pr: CLEAN, a pulse's fall ages by its own time, pr a tick; xs: the steps or
                                         * corners past the record's room, their value at its sample) */
    float of[CO_NR], dl[CO_NR];         /* the steps: age (ticks, CO_STEP's) less the clock's fraction c, size */
    uint8_t pf[CO_NR], fp[CO_NF];       /* (the pulse's falls; fp: the steps still to come's) */
    float co[CO_NC], cd[CO_NC], cs[CO_NC];   /* the triangle's corners: age less ex less c, the tick's fraction, the
                                         * slope's change */
} co_ev;

SY_BLKR static void co_init(co_o *o, const sy_osc_t *s, uint32_t inc, int r1, int on, float sma)
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
static CO_RH __attribute__((noinline)) float co_nv(const co_o *o, float t, int rs, int lin)   /* (in the flash: a block's
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
static float co_naive(const co_o *o, uint32_t p, int rs) { return CO_FD(co_nv)(o, (float)p * U2F, rs, 0); }   /* (co_setup's, in the flash) */

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
static CO_RHS __attribute__((noinline)) float co_sinr(const co_o *o, float t, float c, float D, float k, float e, const co_resp *C)
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
            return CO_RDS(co_sinr)(o, t, c, D, (float)(int)(x + o->eps), K->latn - 3.f, K->C);
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
            R->fp[R->nf] = (uint8_t)pf; R->fo[R->nf] = tq - c; R->fd[R->nf++] = dl; R->fs += dl;
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
        pf_ = -1;
        for (;;) {                      /* (this cycle's fall, and in a short cycle the ones before) */
            ta = (float)sy_floori((e + 0.5f) * o->ies) + c;   /* (the first tick past -0.5, the first at 0.5: e moves
                                         * tkn a ticks a tick, a hair under 1) */
            tb = (float)sy_floori((e - 0.5f) * o->ies) + c;
            if (q != 0.f) {
                float te = e + o->tkf + c - 1.f;    /* (its own time: where the width puts it, as late on average) */
                ta += q * (te - ta); tb += q * (te - tb);
            }
            if (tb >= K->lim) break;
            pf_ = pf_ < 0 ? 2 : 1;      /* (2: this cycle's fall, 1: an earlier cycle's; co_pev) */
            CO_STEP(ta, -2.f * o->wp * o->tkf);
            CO_STEP(tb, -2.f * o->wp * (1.f - o->tkf));
            e += o->tkn * Dc;
        }
        pf_ = 0;
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
        pf_ = q == 0.f && t >= 0.98f ? 3 : 0;   /* (3: this cycle's fall, just past 0.98: co_pev pushes it at that
                                         * tick, whatever its tick's rounding) */
        for (x = (t >= 0.98f ? t - 0.98f : t + Dc - 0.98f) * ia; ; x += Dc * ia) {
            k = (float)(int)(x + (D > 0.f ? o->eps : 0.f));   /* (off the clock before the first restart: no margin) */
            tau = k + c;                /* (osc 1: the fall on the clock as its rise, CLEAN too) */
            if (tau >= K->lim) break;
            CO_STEP(tau, -2.f * o->np);
            pf_ = 0;
        }
        pf_ = 0;
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
static SY_HOT __attribute__((noinline)) void co_recx(const co_o *o, float t, float c, float D, float sg, co_ev *R,
                                                    const co_k *K)
{
    co_lin(o, t, c, D, K->q, sg, R, K);   /* (co_runp's ORIGINAL: K's q 0) */
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
        co_recx(o, t, c, D, sg, R, K);
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
#ifndef CO_QS
#define CO_QS 0.5f                      /* (CO_FAST: the windows' margin past the last step's end, ticks) */
#endif
SY_BLKR static void co_quiet(co_o *o, const co_k *K)
{
    /* (each window from a tick before the step's phase te + a to its end: the step's tick within a tick after te +
     * a, its record until lim ticks after that tick (osc 1's pulse rises a tick later: lim + 1 after the restart);
     * the pulse's second tick 1 / tkn later (e from -0.5 to 0.5); CLEAN: a step's own time half a tick off its tick,
     * a pulse's fall up to 1.5 ticks before its tick: a tick more each side) */
    float w = (K->lim + 2.f + CO_QS + (K->q != 0.f ? 1.f : 0.f)) * o->a + (o->pul ? 1.f / o->tkn : 0.f);
    o->ql = K->q != 0.f ? o->a : 0.f;
    if (o->sin) { w = (K->lim + 3.f) * o->a; o->ql = o->a; }   /* (osc 1's sine: co_sinr's ticks after its restart, the
                                         * ring's sines: 1.18C31's windows, 3 ticks more in co_span) */
    o->fast = o->on && !o->trl && !o->pul0 && w < 0.3f;
    o->ne = 0;
    o->qw = o->sin ? w + 3.f * o->a : w;
    o->sgn = o->trl || o->npl || o->pul0;
    o->lin = o->saw || o->wrap || o->trl;   /* (straight pieces: a record; else the sine, the table alone) */
    if (!o->fast) return;
    if (o->wrap || (o->r1 && o->sin)) o->te[o->ne++] = o->t0 - o->a;   /* (osc 1's sine: its restart's ticks, co_sinr) */
    o->tf = 0.f;
    if (o->pul) { o->tf = (o->htkn - o->tkm - 0.5f) / o->tkn; o->te[o->ne++] = o->tf - o->a; }
    if (o->npl) o->te[o->ne++] = 0.98f - o->a;
}
/* a quiet sample's value (co_lin's there: the naive pieces, a pulse's fall still to come counted off; no sine) */
static SY_HOT __attribute__((noinline)) float co_qv(const co_o *o, float t, float c, const co_k *K)
{
    float y = o->wr * (1.f - 2.f * (t + (c - K->lat) * o->a));
    if (o->pul) y += o->wp * (t > o->tf ? o->pd - 2.f : o->pd);
    if (o->npl) y += o->np * (t >= 0.98f ? 0.04f - 2.f : 0.04f);
    return y;
}

/* how many samples (up to n) from now stay quiet for this oscillator: the samples before the next window opens (its
 * phase reached a tick early); a window open at the next sample: -1 less the samples it surely stays open; *x: the
 * samples to the end of the next window (n: none) */
static SY_HOT __attribute__((noinline)) int co_span(const co_o *o, uint32_t p, float c, float r, int n, int *x)
{
    const float ph = (float)p * U2F + (c + 2.f * r) * o->a;   /* (the phase at the next sample) */
    float e = 2.f;
    int j, m = n;
    *x = n;
    if (!o->on) return n;
    for (j = 0; j < o->ne; j++) {       /* (the windows in the phase at the sample: from te to te + qw) */
        float d = ph - o->te[j];
        d -= (float)sy_floori(d);
        if (d < o->qw) return -1 - (int)((o->qw - d) * o->idps);
        if (1.f - d < e) e = 1.f - d;   /* (the next window's start, a phase) */
    }
    if (e < 2.f) {
        const float u = (e - o->ql) * o->idps;   /* (the samples before ph reaches te; CLEAN, a sine: a tick early) */
        if (u < (float)m) m = u > 0.f ? (int)u : 0;
        if ((e + o->qw) * o->idps + 1.f < (float)n) *x = (int)((e + o->qw) * o->idps) + 1;
    }
    return m;
}

static const co_ev CO_EV0;              /* (a record without steps: a quiet oscillator's in the ring) */
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
static CO_RH __attribute__((noinline)) float co_back(const co_o *o, uint32_t p, uint32_t inc, float D, int m)
{
    uint32_t q = p - (uint32_t)m * inc;
    if (o->r1 && D > 0.f) {
        uint32_t k = (p - (uint32_t)(CO_T0 * 4294967296.f)) / inc;   /* (ticks since the restart, whole when on its grid) */
        if ((uint32_t)m == k) return CO_RD(co_nv)(o, CO_T0, 1, 0);
        if ((uint32_t)m > k) {
            float t = (float)p * U2F + D - (float)m * o->a;
            t -= (float)sy_floori(t);
            return CO_RD(co_nv)(o, t, 0, 0);
        }
    }
    return CO_RD(co_nv)(o, (float)q * U2F, 0, 0);
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
SY_BLKR static float co_fh(const float *h, float f)
{
    return h[0] + 2.f * (h[1] * sy_cos2pi(f) + h[2] * sy_cos2pi(2.f * f) + h[3] * sy_cos2pi(3.f * f));
}
SY_BLKR static float co_fg(const float *h, float f)
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
    int ring, rsm, rf, tab, qs, qp, qr, nsd, on1, on2, qs2;
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
        SY_BLKC(co_init)(o1, &b->o1, i1, 1, S->on1, sma);
        sma = C2->ft[2] + 2.f * C2->ft[1] * sy_cos2pi(a2) + 2.f * C2->ft[0] * sy_cos2pi(2.f * a2);
        SY_BLKC(co_init)(o2, &b->o2, i2, 0, S->on2, sma);
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
    SY_BLKC(co_quiet)(o1, &S->K1); SY_BLKC(co_quiet)(o2, &S->K2);
    o1->idps = 1.f / (2.f * r * o1->a); o2->idps = 1.f / (2.f * r * o2->a);   /* (the samples a phase: co_span's) */
    S->rsm = S->ring && (o1->sin || o1->tab || o2->sin || o2->tab);   /* (the ring with a sine or a table: those
                                         * products through the tables' filter) */
    S->rf = o1->ia >= 40.f && o2->ia >= 40.f;   /* (the smooth parts' cross products filtered: cycles of 40 ticks or
                                         * more) */
    S->tab = o1->tab || o2->tab || S->rsm;
    S->qs = (o1->fast || !o1->on) && (o2->fast || !S->on2) && !S->rsm && !S->tab && (o1->on || S->on2) && !o1->sin
        && !o2->sin;                    /* (the quiet stretches: the naive waves alone, the ring their product) */
#if CO_PTRI
#define CO_QP(o) (!(o)->on || (((o)->wrap || (o)->trl || (CO_PTAB && (o)->tab)) && !(o)->sin && (CO_PTAB || !(o)->tab) \
                                 && !(o)->pul0 \
                                 && (o)->ia >= (q != 0.f && (o)->pul ? 24.f : 12.f)   /* (CLEAN: two cycles' steps in a record) */ \
                                 && !(q != 0.f && (o)->npl)   /* (CLEAN's narrow pulse: its fall a tick older in the restart's record) */ \
                                 && !((o)->pul && ((o)->trl || (o)->npl))))   /* (pul's pf 2: in its wrap's record alone) */
#else
#define CO_QP(o) (!(o)->on || ((o)->wrap && !(o)->sin && !(o)->tab && !(o)->trl && !(o)->npl && !(o)->pul0 && (o)->ia >= 12.f))
#endif
#if CO_PCL
    S->qp = (q == 0.f || (q == 1.f && !S->ring && r >= 0.7f)) && !S->rsm && CO_QP(o1) && CO_QP(o2);   /* (CLEAN: the smooth step's
                                         * 21 ticks within the 16 samples of co_pushq's buffer) */
#else
    S->qp = q == 0.f && !S->rsm && CO_QP(o1) && CO_QP(o2);
#endif   /* (the events' block, co_runp: saws and pulses, cycles of 12 ticks
                                         * or more: a record's steps within CO_NR) */
#undef CO_QP
#if CO_PSIN
#define CO_QS2(o) (!(o)->on || (!(o)->tab && !(o)->pul0 && (o)->ia >= (q != 0.f && (o)->pul ? 24.f : 12.f) \
                                  && !(q != 0.f && (o)->npl) && !((o)->pul && ((o)->trl || (o)->npl))))
    S->qs2 = !S->qp && (o1->sin || o2->sin) && (q == 0.f || (q == 1.f && !S->ring && r >= 0.7f)) && CO_QS2(o1)
        && CO_QS2(o2);                  /* (co_runs: sines with saws, pulses, triangles, the narrow pulse, the ring of them;
                                         * no table) */
#undef CO_QS2
#else
    S->qs2 = 0;
#endif
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
                float a = CO_FD(co_back)(o1, p1, i1, S->D, m - 1), e = CO_FD(co_back)(o2, p2, i2, 1.f, m - 1);
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
    if ((S->qr || S->qs2) && S->ring) {   /* (co_rsmq, co_runs: in a quiet stretch the filter's inputs are its sines, the
                                         * filter's output a sine's times the filter's response at its frequency: H the even
                                         * taps' (a sine's value), G the odd sums' (its slope), the product's by the
                                         * sum and the difference of the two) */
        const float w1 = o1->sin ? o1->ws : 0.f, w2 = o2->sin ? o2->ws : 0.f, f1 = 2.f * r * o1->a, f2 = 2.f * r * o2->a;
        const float *h = S->h, hd = SY_BLKC(co_fh)(h, f1 - f2), hp = SY_BLKC(co_fh)(h, f1 + f2);
        S->qa1 = w1 * SY_BLKC(co_fh)(h, f1); S->qb1 = 2.f * r * w1 * SY_BLKC(co_fg)(h, f1);
        S->qa2 = w2 * SY_BLKC(co_fh)(h, f2); S->qb2 = 2.f * r * w2 * SY_BLKC(co_fg)(h, f2);
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
#ifndef CO_RSMQ
#define CO_RSMQ 1                       /* (co_rung's quiet stretches of the ring with sines; with co_runs (CO_PSIN) left
                                         * to CLEAN's ring and osc_clean between 0 and 1) */
#endif
#if CO_PSIN
#define CO_RSMQ_HOT                     /* (in the flash: co_runs takes its cases but CLEAN's ring and osc_clean between
                                         * 0 and 1) */
#define CO_RSMQ_QV SY_FAR(co_qv)        /* (1.18C34: co_qv and co_sc are RAM code, too far for the flash's direct */
#define CO_RSMQ_SC SY_FAR(co_sc)        /* call) */
#else
#define CO_RSMQ_HOT SY_HOT
#define CO_RSMQ_QV co_qv
#define CO_RSMQ_SC co_sc
#endif
static CO_RSMQ_HOT __attribute__((noinline)) void co_rsmq(co_blk *S, co_ev *A, co_ev *E, float *out, int i, int n, float *pc,
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
    float z1 = CO_RSMQ_QV(o1, (float)p1 * U2F, c + r + r, &S->K1),
        z2 = S->on2 ? CO_RSMQ_QV(o2, (float)p2 * U2F, c + r + r, &S->K2) : 0.f;
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
                if (kn1 + nt > 16) { CO_RSMQ_SC((float)p1 * U2F - S->K1.latn * o1->a, &ks1, &kc1); kn1 = 0; }
                else {
                    int k;
                    for (k = 0; k < nt; k++) { const float q = kc1 * pc1 - ks1 * ps1; ks1 = ks1 * pc1 + kc1 * ps1; kc1 = q; }
                    kn1 += nt;
                }
                y += g1 * (z1 + sg1 * (ks1 + c * ((ks1 * pc1 + kc1 * ps1) - ks1)));
            } else y += g1 * z1;
            if (k2) {
                if (kn2 + nt > 16) { CO_RSMQ_SC((float)p2 * U2F - S->K2.latn * o2->a, &ks2, &kc2); kn2 = 0; }
                else {
                    int k;
                    for (k = 0; k < nt; k++) { const float q = kc2 * pc2 - ks2 * ps2; ks2 = ks2 * pc2 + kc2 * ps2; kc2 = q; }
                    kn2 += nt;
                }
                y += g2 * (z2 + sg2 * (ks2 + c * ((ks2 * pc2 + kc2 * ps2) - ks2)));
            } else y += g2 * z2;
            if (!ring) goto put;        /* (the plain mix: the sines at the ticks) */
            if (y1) {                   /* (co_sline's) */
                if (yn1 >= 16) { CO_RSMQ_SC((float)p1 * U2F + (c - S->K1.lat) * o1->a, &ys1, &yc1); yn1 = 0; }
                else { const float q = yc1 * pw1 - ys1 * qw1; ys1 = ys1 * pw1 + yc1 * qw1; yc1 = q; yn1++; }
            }
            if (y2) {
                if (yn2 >= 16) { CO_RSMQ_SC((float)p2 * U2F + (c - S->K1.lat) * o2->a, &ys2, &yc2); yn2 = 0; }
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
SY_BLKR static void co_save(const co_o *o1, const co_o *o2, sy_voice_t *v, const sy_vblk_t *b, uint32_t p1, uint32_t p2, float D)
{
    const float g1 = b->g1, g2 = b->g2, rgr = b->ring_g * b->o1.ring * b->o2.ring;
    int m;
    const int n1 = g1 != 0.f || rgr != 0.f, n2 = g2 != 0.f || rgr != 0.f;   /* (an oscillator at level 0 outside a ring
                                         * adds nothing) */
    for (m = 0; m < 6; m++) {
        float a = n1 ? CO_FD(co_back)(o1, p1, b->tk_inc1, D, m) : 0.f, e = n2 ? CO_FD(co_back)(o2, p2, b->tk_inc2, 1.f, m) : 0.f;
        v->tk_xh[m] = g1 * a + g2 * e - rgr * a * e;
    }
    v->tk_g1 = g1; v->tk_g2 = g2; v->tk_w1 = b->o1.ws; v->tk_w2 = b->o2.ws; v->tk_s1 = b->o1.wsaw; v->tk_s2 = b->o2.wsaw;
    v->tk_c1 = b->o1.c; v->tk_c2 = b->o2.c; v->tk_d1 = b->o1.d; v->tk_d2 = b->o2.d; v->tk_f1 = b->o1.tkf; v->tk_f2 = b->o2.tkf;
}

/* (y and a start's step at ta ticks: the old ticks' values against the new wave's, through each tick's own response:
 * C(tau) - C(tau - 1); in the flash: a few samples a note) */
SY_BLKR_NS static __attribute__((noinline)) float co_sdm(const co_blk *S, float ta, float y)
{
    float gp = co_g5(ta, S->K.C);
    int m;
    for (m = 1; m <= 6; m++) {
        float gm = co_g5(ta + (float)m, S->K.C);
        y += S->sdm[m - 1] * (gp - gm);
        gp = gm;
    }
    return y;
}

/* the block: x0 gets the waves' naive mix at its first tick (the start transient's sign, as before); scr: the block's
 * values and the records (co_scr) in the voice's buffers osc 1 and 2 leave unused on the clock (o1b, o2b: not on the
 * stack, the second core's is small) */
typedef struct { co_blk S; co_ev A, E; } co_scr;
typedef char co_scr_fits[sizeof(co_scr) <= 2u * SY_BLOCK * sizeof(float) ? 1 : -1];
typedef char co_vt_va[__builtin_offsetof(co_blk, va) == __builtin_offsetof(co_blk, vt) + 8u * sizeof(float) ? 1 : -1];   /* (co_runp's
                                         * CLEAN buffer: vt and va as one) */
/* (the loop's state in and out: the clock, the emphasis, osc 1's jump, the ages of a start's steps, the phases) */
typedef struct { float c, em, D, sa, sg; uint32_t p1, p2; } co_lst;
/* the block's samples, each by the records (co_step), in the quiet stretches the naive waves (both
 * oscillators' windows closed; with sines co_rsmq): what co_runp does not take (the triangle, sines, tables, CLEAN,
 * the narrow pulse, osc 2's pulse above half the clock, high pitches) */
static SY_HOT __attribute__((noinline)) void co_rung(co_scr *const Z, float *CO_RESTRICT out, co_lst *const L)
{
    const float r = Z->S.r, g1 = Z->S.g1, g2 = Z->S.g2, rgr = Z->S.rgr, ea = Z->S.ea, eb = Z->S.eb;
    const uint32_t i1 = Z->S.i1, i2 = Z->S.i2, T0 = (uint32_t)(CO_T0 * 4294967296.f);
    float c = L->c, em = L->em, D = L->D, sa = L->sa, sg = L->sg;
    uint32_t p1 = L->p1, p2 = L->p2;
    const int nsd = Z->S.nsd, ring = Z->S.ring, rsm = Z->S.rsm, tab = Z->S.tab, on2 = Z->S.on2, qs = Z->S.qs;
    int i, qk = 0;
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
    const int v1 = g1 != 0.f, v2 = g2 != 0.f;   /* (block constants: the values wanted) */
    const float cs2 = K.C->s2;
    for (i = 0; i < SY_BLOCK; i++) {
        float y = 0.f, t1, t2;
        int k, w1 = 0, w2 = 0;
        float nt = 0.f;
        if (CO_RSMQ && S.qr && i >= qk && (!nsd || sa - K.ex >= 6.f)) {   /* the ring with sines: its quiet stretches
                                         * in co_rsmq */
            int x, n = co_span(&o1, p1, c, r, SY_BLOCK - i, &x);
            if (n > 1) n = co_span(&o2, p2, c, r, n, &x);
            if (n < 0) qk = i - n;
            else if (n <= 1) qk = i + x;
            if (n > 1) {
                uint32_t n1;
                SY_FAR(co_rsmq)(&S, &A, &E, out, i, n, &c, &p1, &p2, &em, &n1);
                sa += (float)n * (r + r); sg += (float)n * (r + r);
                i += n - 1;
                co_skip(&A, &K1, (float)n1); co_skip(&E, &K2, (float)n1);
                continue;
            }
        }
        if (qs && i >= qk && (!nsd || sa - K.ex >= 6.f)) {   /* a quiet stretch of both: the naive waves alone, their
                                         * ramps a step a sample (the ring: their product, the curvature of the two
                                         * ramps), the emphasis with them */
            int x, n = co_span(&o1, p1, c, r, SY_BLOCK - i, &x);
            if (on2 && n > 1) n = co_span(&o2, p2, c, r, n, &x);
            if (n < 0) qk = i - n;      /* (a window open: no stretch before it closes; one opening: its end) */
            else if (n <= 1) qk = i + x;
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
        if (o1.on) {                    /* the records and the values (a level 0: the ring's record alone; quiet: the
                                         * naive ramp, the record aged at its window) */
            float y1;
            if (!co_quick(&o1, &A, &K1, t1, c, nt, w1, &y1)) y1 = co_step(&o1, &A, &K1, t1, c, D, sg, nt, w1, v1);
            if (v1) y += g1 * y1;
        }
        if (o2.on) {
            float y2;
            if (!co_quick(&o2, &E, &K2, t2, c, nt, w2, &y2)) y2 = co_step(&o2, &E, &K2, t2, c, 1.f, sg, nt, w2, v2);
            if (v2) y += g2 * y2;
        }
        if (ring && !rsm) {             /* the straight pieces' product exactly, each step scaled by the other wave (with
                                         * a sine or a table: co_tabx); no step in either record: co_ring's first line */
            if (A.n | E.n) y -= rgr * co_ring(&A, &E, A.b, A.s, E.b, E.s, c, &K);
            else y -= rgr * (A.b * E.b + 2.f * A.s * E.s * (0.5f * cs2 + 0.5f * c * (1.f - c)));
        }
        if (nsd && sa - K.ex < 6.f) y = SY_FAR(co_sdm)(&S, sa - K.ex, y);   /* (the old ticks' values against the new
                                         * wave's, co_sdm) */
        if (tab) y += co_tabx(&S, &A, &E, t1, t2, c, i, w1);   /* (the tables' filter; the ring with a sine or a
                                         * table) */
        out[i] = eb * y - ea * em;      /* the emphasis */
        em = y;
    }
    L->c = c; L->em = em; L->D = D; L->sa = sa; L->sg = sg; L->p1 = p1; L->p2 = p2;
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

/* (CLEANER, 1.18C32) the block of saws and pulses with ORIGINAL's response, by events (S.qp): each oscillator's straight
 * pieces as a ramp a sample (z, d), its record taken at its wrap (osc 1's restart) as before and each step's residual
 * pushed at its birth into the samples it lives (a step still to come at its tick), the ring's terms with it: the
 * steps' sums (co_ring's) by the other wave's line at each sample (G1, G2) and the rest into T, the value less T. The
 * events by the clock's ticks (nk): a wrap's tick from the phase and its step, a step still to come at its tick. The
 * same values as the per-sample path within the floats' rounding (the sums' order) */
#define CO_PH 8                         /* (the push buffers' samples: a step lives lim / 2 r samples, r 0.79-1: under 4;
                                         * the 6-voice state's clock r 0.39-0.5: lim 5 under 7) */
typedef struct co_pst_ {
    float c, rr, sg, sa, z[2], d[2], s[2], tb[2];   /* (the clock at the event, ticks a sample, the ages of a start's
                                         * steps and its step at the sample; each
                                         * oscillator's ramp, its step a sample, its slope a tick (the ring's), its
                                         * steps' age at sample 0 less their record's of) */
    float D;
#if CO_PCL
    float hs[2], hp[2], h0[4];          /* (CLEAN: the latest births pushed (the steps, the pulse's falls by their own
                                         * time), h0 at the event's start) */
    int mk;                             /* (the push buffer's samples - 1) */
    void (*psh)(co_scr *, struct co_pst_ *, int, float, float, int);   /* (co_push, CLEAN co_pushq) */
#endif
    uint32_t p[2];
    int nk, nt, i, ev, xl, lv[2], ns[2], kw[2], kr[2], kc[2], kf[2];   /* (kc: the tick of the next corner or narrow fall,
                                         * co_sig's change) */
    void (*pev)(co_scr *, struct co_pst_ *);   /* (co_pev; co_pevt with a triangle or a narrow pulse; co_pevq CLEAN) */   /* (ticks so far and in this sample, the sample, the next
                                         * event, the last sample with the ring's cross terms or a start's step;
                                         * per oscillator the last sample a pushed step lives, the tick its phase is
                                         * at, its next wrap's, its record's) */
#if CO_PSIN
    float gm[2], *hb[2];                /* (co_runs: the ring's step sums G at the sample, the buffers of the steps' slope
                                         * sums H (0: not kept)) */
#endif
} co_pst;
#if CO_PSIN
/* (co_runs' state and buffers past co_scr: the voice's o3b, unused while cln_osc runs; vr_osc renders osc 3 after it) */
typedef struct { co_scr Z; float hb[2][CO_PH], sv[3][8]; co_pst P; int krs; float sc[2]; } co_scrs;   /* (sc: co_scq's) */
typedef char co_scrs_fits[sizeof(co_scrs) <= 3u * SY_BLOCK * sizeof(float)
                          && __builtin_offsetof(sy_vwork_t, o3b) == __builtin_offsetof(sy_vwork_t, o1b) + 2u * SY_BLOCK * sizeof(float)
                          ? 1 : -1];
#endif
/* (a step of size dl, age ta at sample P->i, pushed: its residual by K's response into T (gv its level), the ring's
 * sums by S.K's response into G, with the other's slope into T) */
#if CO_PCL
static SY_HOT __attribute__((noinline)) void co_pushq(co_scr *Z, co_pst *P, int k, float ta, float dl, int pf);
#define CO_PUSH(Z, P, k, ta, dl, pf) (P)->psh(Z, P, k, ta, dl, pf)   /* (co_push; CLEAN co_pushq) */
#else
#define CO_PUSH co_push
#endif
#define CO_NTA (1 << 20)                /* (P->nt in CLEAN's events: every step of a record to co_pushq) */
static SY_HOT __attribute__((noinline)) void co_push(co_scr *Z, co_pst *P, int k, float ta, float dl, int pf)
{
    const co_k *const KX = k ? &Z->S.K2 : &Z->S.K1;
    const co_resp *const C = KX->C, *const CR = Z->S.K.C;
    const float rr = P->rr, gv = (k ? Z->S.g2 : Z->S.g1) * dl, rq = Z->S.rgr * P->s[k ^ 1] * dl;
    const float lim = KX->lim < 5.f ? KX->lim : 5.f;
    float *const T = Z->S.vt, *const G = k ? Z->S.ve : Z->S.va;
    const int e = P->i + CO_PH < SY_BLOCK ? P->i + CO_PH : SY_BLOCK;
    int m;
    if (!Z->S.ring)
        for (m = P->i; m < e && ta < lim; m++, ta += rr) {   /* (ta >= 0: a step taken at its tick) */
            const int j = (int)ta;
            T[m & (CO_PH - 1)] += gv * (C->gn[j] + (ta - (float)j) * (C->gn[j + 1] - C->gn[j]));
        }
#if CO_PSIN
    else if (P->hb[k]) {                /* (co_runs: H, the step's slope sum, the other's sine slope's factor) */
        float *const H = P->hb[k];
        for (m = P->i; m < e && ta < lim; m++, ta += rr) {
            const int j = (int)ta, b = m & (CO_PH - 1);
            const float f = ta - (float)j, gr = CR->gn[j] + f * (CR->gn[j + 1] - CR->gn[j]);
            const float hv = gr * (CO_LAT - ta) - (CR->bn[j] + f * (CR->bn[j + 1] - CR->bn[j]));
            T[b] += gv * (C == CR ? gr : C->gn[j] + f * (C->gn[j + 1] - C->gn[j])) - rq * hv;
            G[b] += dl * gr;
            H[b] += dl * hv;
        }
    }
#endif
    else
        for (m = P->i; m < e && ta < lim; m++, ta += rr) {   /* (the ring's response at the same tick: co_g, co_bm) */
            const int j = (int)ta, b = m & (CO_PH - 1);
            const float f = ta - (float)j, gr = CR->gn[j] + f * (CR->gn[j + 1] - CR->gn[j]);
            T[b] += gv * (C == CR ? gr : C->gn[j] + f * (C->gn[j + 1] - C->gn[j]))
                - rq * (gr * (CO_LAT - ta) - (CR->bn[j] + f * (CR->bn[j + 1] - CR->bn[j])));
            G[b] += dl * gr;
        }
    if (m - 1 > P->lv[k]) P->lv[k] = m - 1;
}
#if CO_PCL
/* (CLEAN's step, age ta at sample P->i, pushed when new (its birth past the latest pushed at the event's start: the
 * records hold the steps of earlier cycles too): its residual by co_gk into T (16 samples); a pulse's fall ages by its
 * own time, pr a tick (co_prop's), the clock run on for its ticks) */
static SY_HOT __attribute__((noinline)) void co_pushq(co_scr *Z, co_pst *P, int k, float ta, float dl, int pf)
{
    const co_k *const KX = k ? &Z->S.K2 : &Z->S.K1;
    const float pr = pf ? (k ? Z->E.pr : Z->A.pr) : 1.f, rr = P->rr, r = Z->S.r, lim = KX->lim;
    const float b = (float)P->nk * pr + P->c - ta, gv = (k ? Z->S.g2 : Z->S.g1) * dl;
    float *const T = Z->S.vt, *const h = pf ? &P->hp[k] : &P->hs[k], cc = P->c;
    const int mk = P->mk, e = P->i + mk + 1 < SY_BLOCK ? P->i + mk + 1 : SY_BLOCK;
    int m;
    if (b <= P->h0[(pf ? 2 : 0) + k] + 0.1f) return;   /* (pushed before: an earlier event's) */
    if (b > *h) *h = b;
    for (m = P->i; m < e && ta < lim; m++) {
        T[m & mk] += gv * co_gk(ta, KX);
        if (pf) {
            int n = 0;
            cc += r; if (cc >= 1.f) { cc -= 1.f; n++; }
            cc += r; if (cc >= 1.f) { cc -= 1.f; n++; }
            ta += rr + (pr - 1.f) * (float)n;
        } else ta += rr;
    }
    if (m - 1 > P->lv[k]) P->lv[k] = m - 1;
}
#endif
#if CO_PTRI
/* (a triangle's corner, age ta at sample P->i, pushed: its residual (co_rv's) by K's response into T; the ring has
 * the corners in its lines alone, co_ring's) */
static SY_HOT __attribute__((noinline)) void co_pushc(co_scr *Z, co_pst *P, int k, float ta, float cd, float cs)
{
    const co_resp *const C = (k ? &Z->S.K2 : &Z->S.K1)->C;
    const float rr = P->rr, gv = (k ? Z->S.g2 : Z->S.g1) * cs;
    float *const T = Z->S.vt;
#if CO_PCL
    const int mk = P->mk;
#else
    const int mk = CO_PH - 1;
#endif
    const int e = P->i + mk + 1 < SY_BLOCK ? P->i + mk + 1 : SY_BLOCK;
    int m;
    for (m = P->i; m < e && ta < 5.f; m++, ta += rr) {
        if (ta < 0.f) { T[m & mk] -= gv * ((C->lat - ta) - cd); continue; }   /* (CLEAN: a corner ex ticks later) */
        {
            const int j = (int)ta;
            const float f = ta - (float)j;
            T[m & mk] -= gv * ((C->bn[j] + f * (C->bn[j + 1] - C->bn[j])) - cd * (C->gn[j] + f * (C->gn[j + 1] - C->gn[j])));
        }
    }
    if (m - 1 > P->lv[k]) P->lv[k] = m - 1;   /* (the ring's cross terms looked at there too: none of its own) */
}
/* (a step of osc k pushed before, age ta at sample P->i, its ring term by the other's slope changed by x / dl; in the
 * flash: the ring with a triangle, at its corners) */
static __attribute__((noinline)) void co_pushr(co_scr *Z, co_pst *P, int k, float ta, float x)
{
    const co_k *const KX = k ? &Z->S.K2 : &Z->S.K1;
    const co_resp *const CR = Z->S.K.C;
    const float rr = P->rr, rq = Z->S.rgr * x, lim = KX->lim < 5.f ? KX->lim : 5.f;
    float *const T = Z->S.vt;
    const int e = P->i + CO_PH < SY_BLOCK ? P->i + CO_PH : SY_BLOCK;
    int m;
    for (m = P->i; m < e && ta < lim; m++, ta += rr) {
        const int j = (int)ta;
        const float f = ta - (float)j;
        T[m & (CO_PH - 1)] -= rq * ((CR->gn[j] + f * (CR->gn[j + 1] - CR->gn[j])) * (CO_LAT - ta)
                                    - (CR->bn[j] + f * (CR->bn[j + 1] - CR->bn[j])));
    }
}
/* (the ticks from phase p (increment inc) to the first at or past x as co_sig reads it: (float)p U2F >= x; u its phase
 * as an integer, roughly) */
static __attribute__((noinline)) int co_ctk(uint32_t p, uint32_t inc, uint32_t u, float x)
{
    int k = p < u ? (int)((u - 1u - p) / inc) + 1 : 1;
    if (k > 1 && (float)(p + (uint32_t)(k - 1) * inc) * U2F >= x) return k - 1;
    if (p + (uint32_t)k * inc < p) return k;   /* (past the wrap: its own event) */
    if ((float)(p + (uint32_t)k * inc) * U2F < x) return k + 1;
    return k;
}
/* (the next tick at which co_sig changes other than at a wrap: a triangle's corner, the narrow pulse's fall) */
static __attribute__((noinline)) int co_nkc(const co_o *o, uint32_t p, uint32_t inc, int nk)
{
    const float t = (float)p * U2F;
    if (o->trl) {
        if (t < 0.25f) return nk + co_ctk(p, inc, 0x3FFFFFE0u, 0.25f);
        if (t < 0.75f) return nk + co_ctk(p, inc, 0xBFFFFF80u, 0.75f);
    }
    if (o->npl && t < 0.98f) return nk + co_ctk(p, inc, 0xFAE14780u, 0.98f);
    return 0x7FFFFFFF;
}
#endif
/* (the ring's cross terms at sample i: both records' steps alive there, co_ring's last loop) */
static SY_HOT __attribute__((noinline)) float co_rxp(const co_scr *Z, const co_pst *P, int i)
{
    const co_ev *const A = &Z->A, *const E = &Z->E;
    const co_resp *const C = Z->S.K.C;
    const float ua = P->tb[0] + (float)i * P->rr, ue = P->tb[1] + (float)i * P->rr;
    float x = 0.f;
    int j, q;
    for (j = 0; j < A->n; j++) {
        const float ta = A->of[j] + ua;
        if (ta >= 5.f || ta >= Z->S.K1.lim) continue;
        for (q = 0; q < E->n; q++) {
            const float te = E->of[q] + ue;
            if (te >= 5.f || te >= Z->S.K2.lim) continue;
            x += A->dl[j] * E->dl[q] * co_g(ta > te ? ta : te, C);
        }
    }
    return x;
}
/* (the events at sample P->i: the phases to the clock's tick, osc 1's restart, a record taken at a wrap (or at the
 * block's first sample) and its new steps pushed (a step older than this sample's ticks was at its birth), a step still
 * to come taken at its tick; the next event) */
static SY_HOT __attribute__((noinline)) void co_pev(co_scr *Z, co_pst *P)
{
    const uint32_t T0 = (uint32_t)(CO_T0 * 4294967296.f);
    const float c = P->c;
    const int nk = P->nk, i = P->i;
    int k, j, nw[2] = {0, 0};
    if (nk >= P->kw[0]) {               /* (osc 1's restart: its jump, its phase from T0) */
        const uint32_t qn = P->p[0] + (uint32_t)(P->kw[0] - P->ns[0]) * Z->S.i1;
        P->D = 1.f + ((float)qn - (float)T0) * U2F;
        P->p[0] = T0 + (uint32_t)(nk - P->kw[0]) * Z->S.i1;
        P->kw[0] = nk + (int)((0xFFFFFFFFu - P->p[0]) / Z->S.i1) + 1;
        nw[0] = 1;
    } else P->p[0] += (uint32_t)(nk - P->ns[0]) * Z->S.i1;
    P->p[1] += (uint32_t)(nk - P->ns[1]) * Z->S.i2;
    if (nk >= P->kw[1]) { P->kw[1] = nk + (int)((0xFFFFFFFFu - P->p[1]) / Z->S.i2) + 1; nw[1] = 1; }
    P->ns[0] = P->ns[1] = nk;
    for (k = 0; k < 2; k++) {           /* (the records: both taken before any step is pushed, the ring's slopes) */
        const co_o *const o = k ? &Z->S.o2 : &Z->S.o1;
        co_ev *const R = k ? &Z->E : &Z->A;
        if (!o->on || !(nw[k] || P->kr[k] < 0)) continue;
        co_recx(o, (float)P->p[k] * U2F, c, k ? 1.f : P->D, P->sg, R, k ? &Z->S.K2 : &Z->S.K1);
        P->z[k] = R->b; P->d[k] = R->bk * o->a * P->rr; P->s[k] = R->s;
        P->tb[k] = c - (float)i * P->rr;
        nw[k] = P->kr[k] < 0 ? 2 : 1;   /* (2: the block's first record, all its steps pushed) */
        P->kr[k] = nk;
    }
    P->ev = P->kw[0];
    for (k = 0; k < 2; k++) {
        co_ev *const R = k ? &Z->E : &Z->A;
        if (!(k ? Z->S.o2.on : Z->S.o1.on)) continue;
        if (nw[k])
            for (j = 0; j < R->n; j++) {
                const float ta = R->of[j] + c;
                if (nw[k] == 2 || ta < (float)P->nt || R->pf[j] == 2) CO_PUSH(Z, P, k, ta, R->dl[j], R->pf[j]);   /* (pf 2: this
                                         * cycle's pulse fall, new in this record even when older than this sample's
                                         * ticks: a width near 100 % puts a part of it a tick before its own wrap) */
            }
#if CO_PKF
        if (R->nf && !nw[k] && nk < P->kf[k]) {   /* (none of the steps still to come at its tick yet: ORIGINAL's fo
                                         * whole, kf the first; the next event as below) */
            int e = P->kw[k];
            if (P->kf[k] < e) e = P->kf[k];
            if (e < P->ev) P->ev = e;
            continue;
        }
#endif
        for (j = 0; j < R->nf; ) {      /* (the steps still to come at their tick: taken on, pushed) */
            const float ta = R->fo[j] + (float)(nk - P->kr[k]) + c;
            if (ta >= 0.f && R->n == CO_NR) {   /* (no room: the steps past the response's end dropped) */
                const float u = P->tb[k] + (float)i * P->rr, lim = k ? Z->S.K2.lim : Z->S.K1.lim;
                int q;
                for (q = 0; q < R->n; )
                    if (R->of[q] + u >= lim) { R->n--; R->of[q] = R->of[R->n]; R->dl[q] = R->dl[R->n]; }
                    else q++;
            }
            if (ta >= 0.f && R->n < CO_NR) {
                P->z[k] += R->fd[j];
                R->of[R->n] = R->fo[j]; R->dl[R->n++] = R->fd[j];
                co_push(Z, P, k, ta, R->fd[j], 0);
                R->nf--; R->fo[j] = R->fo[R->nf]; R->fd[j] = R->fd[R->nf];
            } else j++;
        }
        {   /* (the next event: the wrap, or a step still to come's tick, at the earliest a tick on) */
            int e = P->kw[k];
            for (j = 0; j < R->nf; j++) {
                const int t = P->kr[k] + sy_floori(0.5f - R->fo[j]);   /* (ORIGINAL: its age less c whole) */
#if CO_PKF
                if (!j || t < P->kf[k]) P->kf[k] = t;   /* (the first; none: kf unread) */
#endif
                if (t < e) e = t > nk ? t : nk + 1;
            }
            if (e < P->ev) P->ev = e;
        }
    }
}
#if CO_PTRI
/* (co_pev with the triangle's corners and the narrow pulse's fall (P->pt): at such a tick (no wrap at it) the
 * oscillator's record again, the ring's terms of the other's steps already pushed by the slope's change, the newest
 * corner or the narrow pulse's fall pushed; then co_pev; the next such tick after each record (the block's first: all
 * its corners pushed) */
static __attribute__((noinline)) void co_pevt(co_scr *Z, co_pst *P)
{
    const int nk = P->nk, i = P->i;
    const float c = P->c;
    int k, j, r0[2];
    for (k = 0; k < 2; k++) {
        const co_o *const o = k ? &Z->S.o2 : &Z->S.o1;
        co_ev *const R = k ? &Z->E : &Z->A;
        r0[k] = P->kr[k];
        if (nk < P->kc[k] || nk >= P->kw[k]) continue;   /* (with a wrap: co_pev's record) */
        {
            const uint32_t inc = k ? Z->S.i2 : Z->S.i1;
            const float s0 = P->s[k];
            P->p[k] += (uint32_t)(nk - P->ns[k]) * inc; P->ns[k] = nk;
            SY_FAR(co_recx)(o, (float)P->p[k] * U2F, c, k ? 1.f : P->D, P->sg, R, k ? &Z->S.K2 : &Z->S.K1);
            P->z[k] = R->b; P->d[k] = R->bk * o->a * P->rr; P->s[k] = R->s;

            P->tb[k] = c - (float)i * P->rr;
            P->kr[k] = nk; P->kf[k] = -1;   /* (its steps still to come new: co_pev looks) */
            if (R->nc) SY_FAR(co_pushc)(Z, P, k, R->co[0] + c, R->cd[0], R->cs[0]);   /* (the corner just past) */
            for (j = 0; j < R->n; j++)
                if (R->pf[j] == 3 || P->nt == CO_NTA) CO_PUSH(Z, P, k, R->of[j] + c, R->dl[j], R->pf[j]);   /* (the
                                         * narrow pulse's fall; CLEAN: all, co_pushq takes the new) */
            if (Z->S.ring && R->s != s0) {   /* (the other's steps pushed before: the ring's term by the new slope) */
                const co_ev *const Q = k ? &Z->A : &Z->E;
                const float u = P->tb[k ^ 1] + (float)i * P->rr, x = R->s - s0;
                for (j = 0; j < Q->n; j++) SY_FAR(co_pushr)(Z, P, k ^ 1, Q->of[j] + u, Q->dl[j] * x);
            }
        }
    }
    SY_FAR(co_pev)(Z, P);
    for (k = 0; k < 2; k++) {
        const co_o *const o = k ? &Z->S.o2 : &Z->S.o1;
        const co_ev *const R = k ? &Z->E : &Z->A;
        if (!(o->trl || o->npl) || P->kr[k] != nk || !o->on) continue;   /* (its record taken now) */
        P->kc[k] = SY_FAR(co_nkc)(o, P->p[k], k ? Z->S.i2 : Z->S.i1, nk);
        if (r0[k] < 0)
            for (j = 0; j < R->nc; j++) SY_FAR(co_pushc)(Z, P, k, R->co[j] + c, R->cd[j], R->cs[j]);
    }
    if (P->kc[0] < P->ev) P->ev = P->kc[0];
    if (P->kc[1] < P->ev) P->ev = P->kc[1];
}
#endif
#if CO_PCL
/* (CLEAN's events: the steps still to come reaching their own time taken here (a pulse's fall ages by pr a tick: its
 * own time) and kept from co_pev; the births pushed so far kept; co_pev (or co_pevt) with every step of a new record
 * offered to co_pushq; the steps still to come's next event, the first tick they may reach 0) */
static __attribute__((noinline)) void co_pevq(co_scr *Z, co_pst *P)
{
    const int nk = P->nk;
    const float c = P->c;
    int k, j, nf[2] = {0, 0};
    for (k = 0; k < 2; k++) {
        co_ev *const R = k ? &Z->E : &Z->A;
        if (P->kr[k] < 0) continue;     /* (no record yet: co_pev's first) */
        for (j = 0; j < R->nf; ) {
            const float ta = R->fo[j] + (float)(nk - P->kr[k]) * (R->fp[j] ? R->pr : 1.f) + c;
            if (ta >= 0.f && R->n < CO_NR) {
                P->z[k] += R->fd[j];
                R->of[R->n] = R->fo[j]; R->dl[R->n++] = R->fd[j];
                SY_FAR(co_pushq)(Z, P, k, ta, R->fd[j], R->fp[j]);
                R->nf--; R->fo[j] = R->fo[R->nf]; R->fd[j] = R->fd[R->nf]; R->fp[j] = R->fp[R->nf];
            } else j++;
        }
        nf[k] = R->nf; R->nf = 0;
    }
    P->h0[0] = P->hs[0]; P->h0[1] = P->hs[1]; P->h0[2] = P->hp[0]; P->h0[3] = P->hp[1];
    P->nt = CO_NTA;
#if CO_PTRI
    if (Z->S.o1.trl || Z->S.o2.trl) co_pevt(Z, P);
    else
#endif
        SY_FAR(co_pev)(Z, P);
    for (k = 0; k < 2; k++) {
        co_ev *const R = k ? &Z->E : &Z->A;
        if (P->kr[k] != nk) R->nf = (uint8_t)nf[k];   /* (no new record: its steps still to come back) */
        for (j = 0; j < R->nf; j++) {
            int t = P->kr[k] + sy_floori((-1.f - R->fo[j]) / (R->fp[j] ? R->pr : 1.f));
            if (t <= nk) t = nk + 1;
            if (t < P->ev) P->ev = t;
        }
    }
}
#endif
/* (a sample with an event, the ring's cross terms or a start's step: the event (co_pev), the value as co_runp's with
 * those terms; P->xl the next such sample) */
static SY_HOT __attribute__((noinline)) float co_pslow(co_scr *Z, co_pst *P)
{
#if CO_PCL
    const int i = P->i, m = i & P->mk, ring = Z->S.ring;
#else
    const int i = P->i, m = i & (CO_PH - 1), ring = Z->S.ring;
#endif
    const float c = P->c, ex = Z->S.K.ex, rgr = Z->S.rgr;
    float *const T = Z->S.vt, *const G1 = Z->S.va, *const G2 = Z->S.ve;
    float y;
    if (P->nk >= P->ev) P->pev(Z, P);
    y = Z->S.g1 * P->z[0] + Z->S.g2 * P->z[1] - T[m];
    T[m] = 0.f;
    if (ring) {
        const float z1 = P->z[0], z2 = P->z[1];
#if CO_PSIN
        P->gm[0] = G1[m]; P->gm[1] = G2[m];
#endif
        y -= rgr * (z1 * z2 + P->s[0] * P->s[1] * (Z->S.K.C->s2 + c * (1.f - c)) - z2 * G1[m] - z1 * G2[m]);
        G1[m] = G2[m] = 0.f;
        if (i <= P->lv[0] && i <= P->lv[1]) y -= rgr * co_rxp(Z, P, i);   /* (both with steps: their cross terms) */
    }
    P->xl = ring ? (P->lv[0] < P->lv[1] ? P->lv[0] : P->lv[1]) : -1;
    if (Z->S.nsd && P->sa - ex < 6.f) {   /* (a start: as the per-sample path) */
        y = SY_FAR(co_sdm)(&Z->S, P->sa - ex, y);
        P->xl = i + 1;
    }
    return y;
}
static SY_HOT __attribute__((noinline)) void co_runp(co_scr *Z, float *CO_RESTRICT out, co_lst *L)
{
    const float r = Z->S.r, rr = r + r, irr = 1.f / rr, g1 = Z->S.g1, g2 = Z->S.g2, rgr = Z->S.rgr, ea = Z->S.ea;
    const float eb = Z->S.eb, cs2 = Z->S.K.C->s2;
    const int ring = Z->S.ring;
    float *const T = Z->S.vt, *const G1 = Z->S.va, *const G2 = Z->S.ve;
    float c = L->c, em = L->em, z1 = 0.f, z2 = 0.f, d1 = 0.f, d2 = 0.f, s1 = 0.f, s2 = 0.f;
    co_pst P;
    int i, nk = 0, ev = 0, xl = -1, lm = -1;   /* (lm: the last sample a pushed step lives) */
#if CO_PCL
    const int mk = Z->S.K.q != 0.f ? 2 * CO_PH - 1 : CO_PH - 1;   /* (CLEAN: T 16 samples, vt and va; no ring) */
    for (i = 0; i < CO_PH; i++) T[i] = G1[i] = G2[i] = 0.f;
    P.mk = mk; P.psh = co_push;
    P.hs[0] = P.hs[1] = P.hp[0] = P.hp[1] = -1e30f;
#else
    for (i = 0; i < CO_PH; i++) T[i] = G1[i] = G2[i] = 0.f;
#endif
    P.rr = rr; P.D = L->D; P.p[0] = L->p1; P.p[1] = L->p2; P.ns[0] = P.ns[1] = 0;
    P.kw[0] = (int)((0xFFFFFFFFu - L->p1) / Z->S.i1) + 1;
    P.kw[1] = (int)((0xFFFFFFFFu - L->p2) / Z->S.i2) + 1;
    P.kr[0] = P.kr[1] = -1;             /* (the records: taken at the first sample) */
    P.lv[0] = P.lv[1] = -1;
    P.kf[0] = P.kf[1] = -1;             /* (co_pev: the steps still to come looked at) */
#if CO_PSIN
    P.hb[0] = P.hb[1] = 0;
#endif
    P.pev = co_pev;
#if CO_PTRI
    P.kc[0] = P.kc[1] = 0x7FFFFFFF;
    if (Z->S.o1.trl || Z->S.o1.npl || Z->S.o2.trl || Z->S.o2.npl) P.pev = co_pevt;
#endif
#if CO_PCL
    if (Z->S.K.q != 0.f) { P.pev = co_pevq; P.psh = co_pushq; }
#endif
    P.z[0] = P.z[1] = P.d[0] = P.d[1] = P.s[0] = P.s[1] = P.tb[0] = P.tb[1] = 0.f;
    P.ev = 0;
    for (i = 0; i < SY_BLOCK; i++) {
        float y;
        int n0;
        if (i > lm && i > xl) {         /* (no step's residual pending: the naive ramps alone up to the next event, the
                                         * ring their product and the two ramps' curvature) */
            const int i0 = i;
            float zz = g1 * z1 + g2 * z2, u1 = z1, u2 = z2;
            const float dz = g1 * d1 + g2 * d2, kc = s1 * s2;
#define CO_CLK                          /* (the clock a sample on) */ \
                c += r;                                                                             \
                if (c >= 1.f) { c -= 1.f; nk++; }                                                   \
                c += r;                                                                             \
                if (c >= 1.f) { c -= 1.f; nk++; }
#define CO_TICK                         /* (the clock a sample on; the event's sample: below) */ \
                float cc = c + r;                                                                   \
                int kk = nk;                                                                        \
                if (cc >= 1.f) { cc -= 1.f; kk++; }                                                 \
                cc += r;                                                                            \
                if (cc >= 1.f) { cc -= 1.f; kk++; }                                                 \
                if (kk >= ev) break;                                                                \
                c = cc; nk = kk;
            int e = i + (int)((float)(ev - nk) * irr) - 2;   /* (the samples surely before the event's tick: the clock
                                         * unchecked) */
            if (e > SY_BLOCK) e = SY_BLOCK;
            if (!ring) {
                for (; i < e; i++) {
                    CO_CLK
                    out[i] = eb * zz - ea * em;
                    em = zz;
                    zz += dz;
                }
                for (; i < SY_BLOCK; i++) {
                    CO_TICK
                    out[i] = eb * zz - ea * em;
                    em = zz;
                    zz += dz;
                }
            } else {
                for (; i < e; i++) {
                    CO_CLK
                    y = zz - rgr * (u1 * u2 + kc * (cs2 + c * (1.f - c)));
                    out[i] = eb * y - ea * em;
                    em = y;
                    zz += dz; u1 += d1; u2 += d2;
                }
                for (; i < SY_BLOCK; i++) {
                    CO_TICK
                    y = zz - rgr * (u1 * u2 + kc * (cs2 + c * (1.f - c)));
                    out[i] = eb * y - ea * em;
                    em = y;
                    zz += dz; u1 += d1; u2 += d2;
                }
            }
#undef CO_TICK
#undef CO_CLK
            if (i >= SY_BLOCK) break;
            z1 += (float)(i - i0) * d1; z2 += (float)(i - i0) * d2;
        }
        n0 = nk;
        c += r;
        if (c >= 1.f) { c -= 1.f; nk++; }
        c += r;
        if (c >= 1.f) { c -= 1.f; nk++; }
        if (nk >= ev || i <= xl) {      /* (an event, the cross terms, a start: co_pslow with the ramps' state) */
            P.c = c; P.nk = nk; P.nt = nk - n0; P.i = i; P.sg = L->sg + (float)(i + 1) * rr;
            P.sa = L->sa + (float)(i + 1) * rr; P.z[0] = z1; P.z[1] = z2;
            y = co_pslow(Z, &P);
            z1 = P.z[0]; z2 = P.z[1]; d1 = P.d[0]; d2 = P.d[1]; s1 = P.s[0]; s2 = P.s[1]; ev = P.ev; xl = P.xl;
            lm = P.lv[0] > P.lv[1] ? P.lv[0] : P.lv[1];
        } else {
#if CO_PCL
            const int m = i & mk;
#else
            const int m = i & (CO_PH - 1);
#endif
            y = g1 * z1 + g2 * z2 - T[m];
            T[m] = 0.f;
            if (ring) {
                y -= rgr * (z1 * z2 + s1 * s2 * (cs2 + c * (1.f - c)) - z2 * G1[m] - z1 * G2[m]);
                G1[m] = G2[m] = 0.f;
            }
        }
        out[i] = eb * y - ea * em;
        em = y;
        z1 += d1; z2 += d2;
    }
    {   /* (the phases at the block's last tick; osc 1's restart in its last sample already taken by an event) */
        int k;
        for (k = 0; k < 2; k++) P.p[k] += (uint32_t)(nk - P.ns[k]) * (k ? Z->S.i2 : Z->S.i1);
    }
    L->c = c; L->em = em; L->D = P.D; L->sa = L->sa + (float)SY_BLOCK * rr; L->sg = L->sg + (float)SY_BLOCK * rr;
    L->p1 = P.p[0]; L->p2 = P.p[1];
}

#if CO_PSIN
/* (CLEANER, cpu-cave) the block with sines (S.qs2: no table; ORIGINAL, the ring too, or CLEAN without it): co_runp's
 * events and pushed steps for the straight pieces, a loop a sample with the sines added: each sine's value as co_sinn
 * gives it (its phasor turned a tick at each tick; osc 1's restart through co_sinr), the ring's sine terms as co_tabx
 * gives them: the steps' sums by the sines' lines (G by the value, H by the slope, pushed with the steps), the sines'
 * lines with the other's line and its steps still to come, the tables' filter on the sines by its response at their
 * frequencies (co_rsmq's qa1 .. qd), through its taps for six samples after osc 1's restart (its sine's phase jumps:
 * the filter's inputs, three samples ahead, kept in sv). The same values as co_rung within the floats' rounding */
/* (a sine's state in co_runs: its phasor at the ticks (k, co_sinn's) and at the delayed time (y, co_sline's), the counts
 * since they were taken exactly, two ticks' turn) */
typedef struct { float ks, kc, ys, yc, rc[3], rs[3], *v; int kn, yn; } co_sst;   /* (rc, rs: the turn of 0, 1, 2 ticks) */
#ifndef CO_SKN
#define CO_SKN 64                       /* (co_runs: the phasors taken exactly again after these ticks (at the ticks) and
                                         * samples (at the delayed time); co_sinn's, co_sline's 16, 16) */
#endif
#ifndef CO_SYN
#define CO_SYN 32
#endif
/* (co_runs' set-up, in the flash: a call a block; the records' events as co_runp's, the buffers cleared, co_setup's past
 * of the tables' filter (the sines 3 samples on) into sv) */
static void co_pini(co_scr *Z, co_pst *P, const co_lst *L, int rs)
{
    co_scrs *const X = (co_scrs *)(void *)Z;
    const co_o *const o1 = &Z->S.o1, *const o2 = &Z->S.o2;
    int i;
    if (rs) for (i = 0; i < 8; i++) { X->sv[0][i] = Z->S.va[i]; X->sv[1][i] = Z->S.ve[i]; X->sv[2][i] = Z->S.vt[i]; }
    for (i = 0; i < CO_PH; i++) Z->S.vt[i] = Z->S.va[i] = Z->S.ve[i] = X->hb[0][i] = X->hb[1][i] = 0.f;
#if CO_PCL
    P->mk = Z->S.K.q != 0.f ? 2 * CO_PH - 1 : CO_PH - 1; P->psh = co_push;
    P->hs[0] = P->hs[1] = P->hp[0] = P->hp[1] = -1e30f;
#endif
    P->rr = 2.f * Z->S.r; P->D = L->D; P->p[0] = L->p1; P->p[1] = L->p2; P->ns[0] = P->ns[1] = 0;
    P->kw[0] = (int)((0xFFFFFFFFu - L->p1) / Z->S.i1) + 1;
    P->kw[1] = (int)((0xFFFFFFFFu - L->p2) / Z->S.i2) + 1;
    P->kr[0] = P->kr[1] = -1;
    P->lv[0] = P->lv[1] = -1;
    P->kf[0] = P->kf[1] = -1;
    P->hb[0] = rs && o2->sin ? X->hb[0] : 0; P->hb[1] = rs && o1->sin ? X->hb[1] : 0;   /* (a step's slope sum: by the
                                         * other's sine) */
    P->gm[0] = P->gm[1] = 0.f;
    X->krs = -1000;                     /* (co_runs: the last tick within co_sinr's reach of osc 1's last restart; none:
                                         * before the block) */
    if (L->D > 0.f && L->p1 >= (uint32_t)(CO_T0 * 4294967296.f))
        X->krs = -(int)((L->p1 - (uint32_t)(CO_T0 * 4294967296.f)) / Z->S.i1) + (int)(Z->S.K1.latn + 4.f);
    P->pev = co_pev;
#if CO_PTRI
    P->kc[0] = P->kc[1] = 0x7FFFFFFF;
    if (o1->trl || o1->npl || o2->trl || o2->npl) P->pev = co_pevt;
#endif
#if CO_PCL
    if (Z->S.K.q != 0.f) { P->pev = co_pevq; P->psh = co_pushq; }
#endif
    P->z[0] = P->z[1] = P->d[0] = P->d[1] = P->s[0] = P->s[1] = P->tb[0] = P->tb[1] = 0.f;
    P->ev = 0;
}
/* (osc 1's sine within co_sinr's reach of its restart (the ticks one by one through the taps): 1 and its value in *v;
 * in the flash, a few samples a cycle) */
static CO_RHS __attribute__((noinline)) int co_sinw(const co_o *o, const co_k *K, uint32_t p, float c, float D, float *v)
{
    const float t = (float)p * U2F, x = (t - o->t0) * o->ia;
    if (!(x >= 0.f && x < K->latn + 3.5f)) return 0;
    *v = CO_RDS(co_sinr)(o, t, c, D, (float)(int)(x + o->eps), K->latn - 3.f, K->C);
    return 1;
}
/* (a sine's value as co_sinn's, the state in registers; kt the ticks of this sample, w taken exactly) */
/* (a phasor taken exactly at the phase p U2F + off into v: sin, cos; out of line, a call in 16-32 samples) */
static SY_HOT __attribute__((noinline)) void co_scq(uint32_t p, float off, float *v) { co_sc((float)p * U2F + off, v, v + 1); }
static inline __attribute__((always_inline)) float co_sinv(const co_o *o, co_sst *q, const co_k *K, uint32_t p, float c,
                                                          int kt, int w)
{
    if (w | (q->kn + kt > CO_SKN)) {    /* (p: the phase at this tick; v: the scratch's) */
        co_scq(p, -K->latn * o->a, q->v); q->ks = q->v[0]; q->kc = q->v[1]; q->kn = 0;
    }
    else {                              /* (0, 1 or 2 ticks: the phasor turned once, by their turn; 0: exact) */
        const float rc = q->rc[kt], rs = q->rs[kt], u = q->kc * rc - q->ks * rs;
        q->ks = q->ks * rc + q->kc * rs; q->kc = u;
        q->kn += kt;
    }
    return o->sg * (q->ks + c * ((q->ks * o->prc + q->kc * o->prs) - q->ks));
}
/* (co_sline's phasor, the state in registers) */
static inline __attribute__((always_inline)) void co_slv(const co_o *o, co_sst *q, uint32_t p, float c, float lat, int w)
{
    if (w | (q->yn >= CO_SYN)) { co_scq(p, (c - lat) * o->a, q->v); q->ys = q->v[0]; q->yc = q->v[1]; q->yn = 0; }
    else { const float u = q->yc * o->pwc - q->ys * o->pws; q->ys = q->ys * o->pwc + q->yc * o->pws; q->yc = u; q->yn++; }
}
/* (the tables' filter's taps on v at sample i: its value and slope sums; *f the slope's; in the flash: six samples after
 * osc 1's restart) */
static CO_RHS __attribute__((noinline)) float co_ftap(const float *v, const float *h, int i, float *f)
{
    const int i0 = i & 7, j1 = (i - 1) & 7, j2 = (i - 2) & 7, j3 = (i - 3) & 7, k1 = (i + 1) & 7, k2 = (i + 2) & 7,
        k3 = (i + 3) & 7;
    *f = h[1] * (v[k1] - v[j1]) + 2.f * h[2] * (v[k2] - v[j2]) + 3.f * h[3] * (v[k3] - v[j3]);
    return h[0] * v[i0] + h[1] * (v[j1] + v[k1]) + h[2] * (v[j2] + v[k2]) + h[3] * (v[j3] + v[k3]);
}
static SY_HOT __attribute__((noinline)) void co_runs(co_scr *Z, float *CO_RESTRICT out, co_lst *L)
{
    co_scrs *const X = (co_scrs *)(void *)Z;
    const float r = Z->S.r, rr = r + r, g1 = Z->S.g1, g2 = Z->S.g2, rgr = Z->S.rgr, ea = Z->S.ea, eb = Z->S.eb;
    const float cs2 = Z->S.K.C->s2;
    const uint32_t i1 = Z->S.i1, i2 = Z->S.i2;
    const co_o *const o1 = &Z->S.o1, *const o2 = &Z->S.o2;
    const co_k *const K1 = &Z->S.K1, *const K2 = &Z->S.K2;
    const int ring = Z->S.ring, rf = Z->S.rf, n1 = o1->sin, n2 = o2->sin;
    const int v1 = n1 && g1 != 0.f, v2 = n2 && g2 != 0.f, rs = ring && (n1 || n2), pr = rs && n1 && n2;
    const int kwn = (int)(K1->latn + 4.f), mk = Z->S.K.q != 0.f ? 2 * CO_PH - 1 : CO_PH - 1;   /* (kwn: co_sinr's reach,
                                         * the ticks since osc 1's restart, a margin) */
    float *const T = Z->S.vt, *const G1 = Z->S.va, *const G2 = Z->S.ve, *const H1 = X->hb[0], *const H2 = X->hb[1];
    float *const va = X->sv[0], *const ve = X->sv[1], *const vt = X->sv[2];
    float c = L->c, em = L->em, z1 = 0.f, z2 = 0.f, d1 = 0.f, d2 = 0.f, s1 = 0.f, s2 = 0.f, fs1 = 0.f, fs2 = 0.f;
    co_sst q1, q2;                      /* (the sines' phasors: taken at their first use) */
    co_pst *const P = &X->P;            /* (in the scratch: the second core's stack is small) */
    int i, nk = 0, ev = 0, xl = -1, lm = -1, lw = -100, krs = -1000;   /* (krs: co_sinr's last tick; lm: the last
                                         * sample a pushed step lives) */
    const float irr = 1.f / rr;
    SY_FAR(co_pini)(Z, P, L, rs);
    q1.v = q2.v = X->sc;
    q1.kn = q1.yn = q2.kn = q2.yn = 99; q1.ks = q1.kc = q1.ys = q1.yc = q2.ks = q2.kc = q2.ys = q2.yc = 0.f;
    q1.rc[0] = q2.rc[0] = 1.f; q1.rs[0] = q2.rs[0] = 0.f;
    q1.rc[1] = o1->prc; q1.rs[1] = o1->prs; q2.rc[1] = o2->prc; q2.rs[1] = o2->prs;
    q1.rc[2] = o1->prc * o1->prc - o1->prs * o1->prs; q1.rs[2] = 2.f * o1->prc * o1->prs;
    q2.rc[2] = o2->prc * o2->prc - o2->prs * o2->prs; q2.rs[2] = 2.f * o2->prc * o2->prs;
    krs = X->krs;
#define CO_P1 (P->p[0] + (uint32_t)(nk - P->ns[0]) * i1)   /* (the phases at this tick) */
#define CO_P2 (P->p[1] + (uint32_t)(nk - P->ns[1]) * i2)
    for (i = 0; i < SY_BLOCK; i++) {
        int m, n0, w1 = 0, w2 = 0, kt;
        float y, gA = 0.f, gE = 0.f;
        if (i > lm && i > xl && nk > krs && (!rs || (rf && i - lw >= 6))) {   /* (no step's residual pending, past
                                         * co_sinr's reach and the filter's taps after osc 1's restart: the naive ramps
                                         * (the ring: their product), the sines and the ring's sine terms by the filter's
                                         * response alone up to the next event's sample, the clock unchecked:
                                         * co_runp's) */
            const int i0 = i;
            float zz = g1 * z1 + g2 * z2, u1 = z1, u2 = z2;
            const float dz = g1 * d1 + g2 * d2, kq = s1 * s2;
            int e = i + (int)((float)(ev - nk) * irr) - 2;
            if (e > SY_BLOCK) e = SY_BLOCK;
            for (; i < e; i++) {
                n0 = nk;
                c += r;
                if (c >= 1.f) { c -= 1.f; nk++; }
                c += r;
                if (c >= 1.f) { c -= 1.f; nk++; }
                kt = nk - n0;
                y = zz;
                if (ring) y -= rgr * (u1 * u2 + kq * (cs2 + c * (1.f - c)));
                if (v1) y += g1 * co_sinv(o1, &q1, K1, CO_P1, c, kt, 0);
                if (v2) y += g2 * co_sinv(o2, &q2, K2, CO_P2, c, kt, 0);
                if (rs) {               /* (the main loop's below without steps, taps, a restart) */
                    const int i3 = (i + 3) & 7;
                    float x = 0.f;
                    if (n1) {
                        co_slv(o1, &q1, CO_P1, c, K1->lat, 0);
                        va[i3] = o1->ws * (q1.ys * o1->p6c + q1.yc * o1->p6s);
                        x += Z->S.qa1 * q1.ys * (u2 + fs2) + Z->S.qb1 * q1.yc * s2 - o1->ws * q1.ys * fs2;
                    }
                    if (n2) {
                        co_slv(o2, &q2, CO_P2, c, K2->lat, 0);
                        ve[i3] = o2->ws * (q2.ys * o2->p6c + q2.yc * o2->p6s);
                        x += Z->S.qa2 * q2.ys * (u1 + fs1) + Z->S.qb2 * q2.yc * s1 - o2->ws * q2.ys * fs1;
                    }
                    y -= rgr * x;
                    if (pr) {
                        vt[i3] = -rgr * va[i3] * ve[i3];
                        y += Z->S.qc * q1.yc * q2.yc + Z->S.qd * q1.ys * q2.ys;
                    }
                }
                out[i] = eb * y - ea * em;
                em = y;
                zz += dz; u1 += d1; u2 += d2;
            }
            z1 += (float)(i - i0) * d1; z2 += (float)(i - i0) * d2;
            if (i >= SY_BLOCK) break;
        }
        m = i & mk; n0 = nk;
        c += r;
        if (c >= 1.f) { c -= 1.f; nk++; }
        c += r;
        if (c >= 1.f) { c -= 1.f; nk++; }
        if (nk >= ev || i <= xl) {      /* (an event, the cross terms, a start: co_pslow, as co_runp) */
            const int k0 = P->kw[0], k1 = P->kw[1];
            P->c = c; P->nk = nk; P->nt = nk - n0; P->i = i; P->sg = L->sg + (float)(i + 1) * rr;
            P->sa = L->sa + (float)(i + 1) * rr; P->z[0] = z1; P->z[1] = z2;
            y = co_pslow(Z, P);
            z1 = P->z[0]; z2 = P->z[1]; d1 = P->d[0]; d2 = P->d[1]; s1 = P->s[0]; s2 = P->s[1]; ev = P->ev; xl = P->xl;
            lm = P->lv[0] > P->lv[1] ? P->lv[0] : P->lv[1];
            gA = P->gm[0]; gE = P->gm[1];
            if (rs && rf) {             /* (the steps still to come's sums: the records' left) */
                int j;
                fs1 = fs2 = 0.f;
                for (j = 0; j < Z->A.nf; j++) fs1 += Z->A.fd[j];
                for (j = 0; j < Z->E.nf; j++) fs2 += Z->E.fd[j];
            }
            if (P->kw[0] != k0) { w1 = 1; krs = k0 + kwn; }   /* (osc 1's restart, osc 2's wrap at this sample) */
            w2 = P->kw[1] != k1;
        } else {
            y = g1 * z1 + g2 * z2 - T[m];
            T[m] = 0.f;
            if (ring) {
                gA = G1[m]; gE = G2[m];
                y -= rgr * (z1 * z2 + s1 * s2 * (cs2 + c * (1.f - c)) - z2 * gA - z1 * gE);
                G1[m] = G2[m] = 0.f;
            }
        }
        kt = nk - n0;
        if (v1) {                       /* (osc 1's sine: co_step's co_sinn; near its restart co_sinr's) */
            float u;
            if (!(nk <= krs && P->D > 0.f && SY_FAR(co_sinw)(o1, K1, CO_P1, c, P->D, &u)))
                u = co_sinv(o1, &q1, K1, CO_P1, c, kt, w1);
            else q1.kn = 99;
            y += g1 * u;
        }
        if (v2) y += g2 * co_sinv(o2, &q2, K2, CO_P2, c, kt, w2);
        if (rs) {                       /* (the ring's sine terms: co_tabx's) */
            const float hA = H1[m], hE = H2[m];
            const int i3 = (i + 3) & 7;
            float sb1 = 0.f, ss1 = 0.f, sb2 = 0.f, ss2 = 0.f, x;
            int tp;
            H1[m] = H2[m] = 0.f;
            if (n1) {
                if (w1) lw = i;
                co_slv(o1, &q1, CO_P1, c, K1->lat, w1);
                sb1 = o1->ws * q1.ys; ss1 = o1->ws * 6.28318531f * o1->a * q1.yc;
                va[i3] = o1->ws * (q1.ys * o1->p6c + q1.yc * o1->p6s);   /* (the sines 3 samples on) */
            }
            if (n2) {
                co_slv(o2, &q2, CO_P2, c, K2->lat, 0);
                sb2 = o2->ws * q2.ys; ss2 = o2->ws * 6.28318531f * o2->a * q2.yc;
                ve[i3] = o2->ws * (q2.ys * o2->p6c + q2.yc * o2->p6s);
            }
            tp = i - lw < 6;            /* (osc 1's restart within the filter's reach: its taps) */
            x = 0.f;                    /* (each sine: by the other's steps (G by its value, H by its slope), filtered
                                         * with the other's line and its steps still to come; short cycles: the lines as
                                         * they are) */
            if (n1) {
                if (!rf) x += sb1 * z2 + ss1 * s2 * (cs2 + c * (1.f - c));
                else if (tp) { float f; x += SY_FAR(co_ftap)(va, Z->S.h, i, &f) * (z2 + fs2) + f * rr * s2; }
                else x += Z->S.qa1 * q1.ys * (z2 + fs2) + Z->S.qb1 * q1.yc * s2;
                x -= sb1 * (gE + (rf ? fs2 : 0.f)) + ss1 * hE;
            }
            if (n2) {
                if (!rf) x += sb2 * z1 + ss2 * s1 * (cs2 + c * (1.f - c));
                else if (tp) { float f; x += SY_FAR(co_ftap)(ve, Z->S.h, i, &f) * (z1 + fs1) + f * rr * s1; }
                else x += Z->S.qa2 * q2.ys * (z1 + fs1) + Z->S.qb2 * q2.yc * s1;
                x -= sb2 * (gA + (rf ? fs1 : 0.f)) + ss2 * hA;
            }
            y -= rgr * x;
            if (pr) {                   /* (the sines' product through the filter) */
                float f;
                vt[i3] = -rgr * va[i3] * ve[i3];
                y += tp ? SY_FAR(co_ftap)(vt, Z->S.h, i, &f) : Z->S.qc * q1.yc * q2.yc + Z->S.qd * q1.ys * q2.ys;
            }
        }
        out[i] = eb * y - ea * em;
        em = y;
        z1 += d1; z2 += d2;
    }
#undef CO_P1
#undef CO_P2
    P->p[0] += (uint32_t)(nk - P->ns[0]) * i1; P->p[1] += (uint32_t)(nk - P->ns[1]) * i2;
    L->c = c; L->em = em; L->D = P->D; L->sa = L->sa + (float)SY_BLOCK * rr; L->sg = L->sg + (float)SY_BLOCK * rr;
    L->p1 = P->p[0]; L->p2 = P->p[1];
}
#endif

#if CO_PTAB
/* (CLEANER, cpu-p1b) the table waves of a block run by co_runp (no ring): its output less them, then here their filter
 * (co_tabx's, its past tv) at each sample's phases (the clock again from L0, the block's start), through the emphasis
 * (linear: eb x[n] - ea x[n-1] of the tables' part added), its state em with it */
static SY_HOT __attribute__((noinline)) void co_tabr(co_scr *Z, float *CO_RESTRICT out, const co_lst *L0, co_lst *L,
                                                   const float *tv)
{
    const float r = Z->S.r, ea = Z->S.ea, eb = Z->S.eb;
    const uint32_t i1 = Z->S.i1, i2 = Z->S.i2, T0 = (uint32_t)(CO_T0 * 4294967296.f);
    float c = L0->c, yp = 0.f;
    uint32_t p1 = L0->p1, p2 = L0->p2;
    int i, k;
    for (i = 0; i < 8; i++) Z->S.vt[i] = tv[i];
    for (i = 0; i < SY_BLOCK; i++) {
        float y;
        for (k = 0; k < 2; k++) {       /* (co_rung's clock) */
            c += r;
            if (c >= 1.f) {
                const uint32_t qn = p1 + i1;
                c -= 1.f;
                p1 = qn < p1 ? T0 : qn;
                p2 += i2;
            }
        }
        y = co_tabx(&Z->S, &Z->A, &Z->E, (float)p1 * U2F, (float)p2 * U2F, c, i, 0);
        out[i] += eb * y - ea * yp;
        yp = y;
    }
    L->em += yp;
}
#endif
static SY_HOT __attribute__((noinline)) void cln_osc(sy_voice_t *v, const sy_vblk_t *b, float *CO_RESTRICT out, float *x0, void *scr)
{
    co_scr *const Z = (co_scr *)scr;
    co_ev *const A = &Z->A, *const E = &Z->E;   /* (each oscillator's record: none taken yet) */
    co_lst L;
    SY_FAR(co_setup)(&Z->S, v, b, x0);
    A->n = A->nc = E->n = E->nc = A->ovf = E->ovf = A->zs = E->zs = A->nf = E->nf = 0;
    A->fs = E->fs = A->fa = E->fa = 0.f; A->st = E->st = 1; A->xs = E->xs = 0.f;
    A->kn = A->yn = E->kn = E->yn = 99;   /* (the sines' phasors: taken at their first use) */
    A->b = A->s = A->bc = A->bk = E->b = E->s = E->bc = E->bk = 0.f;
    L.c = v->tk_c; L.em = v->tk_em; L.D = Z->S.D; L.sa = Z->S.sa; L.sg = Z->S.sg; L.p1 = v->ph1; L.p2 = v->ph2;
#if CO_PTAB
    if (Z->S.qp && Z->S.tab) {          /* (with table waves: their filter after, co_tabr) */
        const co_lst L0 = L;
        float tv[8];
        int i;
        for (i = 0; i < 8; i++) tv[i] = Z->S.vt[i];
        co_runp(Z, out, &L);
        co_tabr(Z, out, &L0, &L, tv);
    } else
#endif
    if (Z->S.qp) co_runp(Z, out, &L);   /* (saws and pulses, ORIGINAL: by events) */
#if CO_PSIN
    else if (Z->S.qs2) co_runs(Z, out, &L);   /* (with sines) */
#endif
    else co_rung(Z, out, &L);
    SY_FAR(co_save)(&Z->S.o1, &Z->S.o2, v, b, L.p1, L.p2, L.D);
    v->tk_c = L.c; v->tk_em = L.em; v->tk_D1 = L.D;
    v->ph1 = L.p1; v->ph2 = L.p2; v->tk_pe1 = L.p1; v->tk_pe2 = L.p2;
}
#endif   /* CO_FAST */
