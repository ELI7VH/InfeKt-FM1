/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 J0KERPACK */
/* A microbenchmark of the chip, for the device (the emulator counts one cycle an instruction): what a float
 * multiply-add, a division, a conversion, a load from the RAM and from the flash cost, and the same loop run from
 * the flash and from the RAM. Built with the engine's flags (fm1.toml o2), so its code is the engine's kind.
 * Each test runs short (well under a block) and many times; the least is kept, a run the audio did not interrupt. */
#ifdef HYB_BENCH                         /* (fm1.toml o2_cflags: the benchmark built in; off, nothing of it) */
#include <stdint.h>
#include "bench.h"

#define RAMF __attribute__((section(".ram_text"), noinline))
#define FLF __attribute__((noinline))

static const uint32_t FL_CHASE[16384] = {
#include "bench_tab.inc"
};
static const uint32_t FL_SMALL[1024] = {
#include "bench_tab_s.inc"
};
static uint32_t ram_chase[1024];
volatile float bench_sink_f;
volatile uint32_t bench_sink_u;
static volatile float va = 0.999f, vb = 0.0001f, vk = 3.0f;

static void *far(void *p) { void *volatile q = p; return q; }
#define FAR(fn) ((__typeof__(&fn))far((void *)&fn))

/* a dependent multiply-add chain, 8 an iteration */
#define MAC8(x) x = x * a + b; x = x * a + b; x = x * a + b; x = x * a + b; \
                x = x * a + b; x = x * a + b; x = x * a + b; x = x * a + b
static FLF float t_mac_fl(int n) { float x = 1.f, a = va, b = vb; while (n--) { MAC8(x); } return x; }
static RAMF float t_mac_ram(int n) { float x = 1.f, a = va, b = vb; while (n--) { MAC8(x); } return x; }
/* a dependent add chain, a dependent multiply chain (the compiler orders independent chains by itself: no
 * side-by-side test here, inline assembly being the BSP's) */
static RAMF float t_add(int n)
{
    float x = 1.f, b = vb;
    while (n--) { x = x + b; x = x + b; x = x + b; x = x + b; x = x + b; x = x + b; x = x + b; x = x + b; }
    return x;
}
static RAMF float t_mul(int n)
{
    float x = 1.f, a = va;
    while (n--) { x = x * a; x = x * a; x = x * a; x = x * a; x = x * a; x = x * a; x = x * a; x = x * a; }
    return x;
}
/* a dependent division chain */
static RAMF float t_div(int n)
{
    float x = 1.f, a = va, b = vb + 1.f;
    while (n--) { x = a / (x + b); x = a / (x + b); x = a / (x + b); x = a / (x + b);
                  x = a / (x + b); x = a / (x + b); x = a / (x + b); x = a / (x + b); }
    return x;
}
/* float to int and back, dependent */
static RAMF float t_cvt(int n)
{
    float x = 5.f, k = vk;
    while (n--) { int i;
        i = (int)(x * k); x = (float)(i & 1023) + 1.f; i = (int)(x * k); x = (float)(i & 1023) + 1.f;
        i = (int)(x * k); x = (float)(i & 1023) + 1.f; i = (int)(x * k); x = (float)(i & 1023) + 1.f; }
    return x;
}
/* loads, each one's address from the one before: the RAM, the flash in 4 KB (cached), the flash in 64 KB */
static RAMF uint32_t t_chase(const uint32_t *t, int n)
{
    uint32_t i = 0;
    while (n--) { i = t[i]; i = t[i]; i = t[i]; i = t[i]; i = t[i]; i = t[i]; i = t[i]; i = t[i]; }
    return i;
}

/* independent work: an int chain (1 a cycle?), loads in a row, stores in a row, a float array scaled (load,
 * multiply, store, each element on its own), the same loads from the pool (the engine's buffers) */
static RAMF uint32_t t_int(int n)
{
    uint32_t r = bench_sink_u, k = 0x9E37u;
    while (n--) { r = (r ^ k) + 7u; r = (r ^ k) + 7u; r = (r ^ k) + 7u; r = (r ^ k) + 7u; }
    return r;
}
static RAMF uint32_t t_ldseq(const uint32_t *a, int n)
{
    uint32_t s0 = 0, s1 = 0;
    while (n--) {
        const uint32_t *p = a;
        int j;
        for (j = 0; j < 1024; j += 8) { s0 += p[j]; s1 += p[j + 1]; s0 += p[j + 2]; s1 += p[j + 3];
                                        s0 += p[j + 4]; s1 += p[j + 5]; s0 += p[j + 6]; s1 += p[j + 7]; }
    }
    return s0 + s1;
}
static RAMF void t_stseq(uint32_t *a, int n)
{
    while (n--) {
        int j;
        for (j = 0; j < 1024; j += 8) { a[j] = (uint32_t)j; a[j + 1] = (uint32_t)j; a[j + 2] = (uint32_t)j; a[j + 3] = (uint32_t)j;
                                        a[j + 4] = (uint32_t)j; a[j + 5] = (uint32_t)j; a[j + 6] = (uint32_t)j; a[j + 7] = (uint32_t)j; }
    }
}
static RAMF void t_fscale(float *y, const float *x, int n)
{
    float a = va;
    while (n--) {
        int j;
        for (j = 0; j < 256; j += 4) { y[j] = x[j] * a; y[j + 1] = x[j + 1] * a; y[j + 2] = x[j + 2] * a; y[j + 3] = x[j + 3] * a; }
    }
}
static float fx_in[256], fx_out[256];
/* four independent multiply-add chains (the FPU's throughput: with results no one waits for, does it overlap
 * them, or does each op hold it?) and a data-dependent branch taken half the time */
static RAMF float t_mac_ind(int n)
{
    float x0 = 1.f, x1 = 2.f, x2 = 3.f, x3 = 4.f, a = va, b = vb;
    while (n--) {
        x0 = x0 * a + b; x1 = x1 * a + b; x2 = x2 * a + b; x3 = x3 * a + b;
        x0 = x0 * a + b; x1 = x1 * a + b; x2 = x2 * a + b; x3 = x3 * a + b;
    }
    return x0 + x1 + x2 + x3;
}
static RAMF uint32_t t_branch(int n)
{
    uint32_t r = 74565u, s = 0;         /* (a seed) */
    while (n--) {                        /* 8 branches an iteration on a pseudo-random bit */
        int k;
        for (k = 0; k < 8; k++) {
            r = r * 1664525u + 1013904223u;
            if (r & 0x80000000u) s += 3u; else s ^= r >> 7;
        }
    }
    return s;
}
/* the same chains with denormal operands (a filter's or a reverb's tail decaying under 1.2e-38) */
static volatile float vden = 1e-39f, vone = 0.999f;
static RAMF float t_mul_den(int n)
{
    float x = vden, a = vone;
    while (n--) { x = x * a + vden; x = x * a + vden; x = x * a + vden; x = x * a + vden; }
    return x;
}
static RAMF float t_add_den(int n)
{
    float x = vden, b = vden;
    while (n--) { x = x + b; x = x - b; x = x + b; x = x - b; x = x + b; x = x - b; x = x + b; x = x - b; }
    return x;
}

#include "bench_big.inc"
/* the flash's cache emptied as far as a pass over 480 KB of the image can: every 16th byte below the code's end */
extern const uint8_t _etext[];
static FLF uint32_t evict(void)
{
    const uint8_t *p = _etext - 0x78000;
    uint32_t s = 0, i;
    for (i = 0; i < 0x78000u; i += 16u) s += p[i];
    return s;
}

static uint32_t (*clk)(void);
static uint32_t clk_mul;
static uint32_t best;
static void take(uint32_t t0)
{
    uint32_t d = (clk() - t0) * clk_mul;
    if (d < best) best = d;
}

int hyb_bench(uint32_t (*clock)(void), uint32_t cycles_a_tick, uint32_t *pool, uint32_t res[BENCH_N])
{
    static const uint16_t OPS[BENCH_N] = {2048, 2048, 2048, 2048, 512, 512, 2048, 2048, 512,
                                          2048, 1024, 1024, 256, 1024, 1024, 512, 512, 256, 2048, 2048, 2048, 1024};
    float (*mac_ram)(int) = FAR(t_mac_ram), (*add)(int) = FAR(t_add), (*mul)(int) = FAR(t_mul), (*dv)(int) = FAR(t_div),
          (*cv)(int) = FAR(t_cvt);
    uint32_t (*ch)(const uint32_t *, int) = FAR(t_chase), (*it)(int) = FAR(t_int),
             (*ld)(const uint32_t *, int) = FAR(t_ldseq);
    float (*md)(int) = FAR(t_mul_den), (*ad)(int) = FAR(t_add_den), (*mi)(int) = FAR(t_mac_ind);
    uint32_t (*br)(int) = FAR(t_branch);
    void (*st)(uint32_t *, int) = FAR(t_stseq), (*fs)(float *, const float *, int) = FAR(t_fscale);
    uint32_t k;
    int i, r;
    clk = clock; clk_mul = cycles_a_tick;
    for (i = 0; i < 1024; i++) ram_chase[i] = pool[i] = (uint32_t)(i + 257) & 1023u;
    for (k = 0; k < BENCH_N; k++) {
        int n = k == BENCH_LD_COLD ? OPS[k] / 8 : k == BENCH_CODE_COLD || k == BENCH_CODE_WARM ? 1
              : k == BENCH_CVT || k == BENCH_MAC_DEN ? OPS[k] / 4                 /* (iterations: 8 ops each, the conversions 4; */
              : k == BENCH_LD_SEQ || k == BENCH_ST_SEQ || k == BENCH_FSCALE || k == BENCH_LDSEQ_POOL ? 1
              : OPS[k] / 8;                                  /* the rows: one pass, OPS its elements) */
        best = ~0u;
        for (r = 0; r < 24; r++) {
            uint32_t t0;
            if (k == BENCH_LD_COLD || k == BENCH_CODE_COLD) bench_sink_u = evict();
            if (k == BENCH_CODE_WARM) bench_sink_f = t_bigcode(1.f);
            t0 = clk();
            switch (k) {
            case BENCH_MAC_FL: bench_sink_f = t_mac_fl(n); break;
            case BENCH_MAC_RAM: bench_sink_f = mac_ram(n); break;
            case BENCH_ADD: bench_sink_f = add(n); break;
            case BENCH_MUL: bench_sink_f = mul(n); break;
            case BENCH_DIV: bench_sink_f = dv(n); break;
            case BENCH_CVT: bench_sink_f = cv(n); break;
            case BENCH_LD_RAM: bench_sink_u = ch(ram_chase, n); break;
            case BENCH_LD_FL4K: bench_sink_u = ch(FL_SMALL, n); break;
            case BENCH_LD_FL64K: bench_sink_u = ch(FL_CHASE, n); break;
            case BENCH_INT: bench_sink_u = it(n); break;
            case BENCH_LD_SEQ: bench_sink_u = ld(ram_chase, n); break;
            case BENCH_ST_SEQ: st(ram_chase, n); break;
            case BENCH_FSCALE: fs(fx_out, fx_in, n); break;
            case BENCH_LD_POOL: bench_sink_u = ch(pool, n); break;
            case BENCH_MAC_DEN: bench_sink_f = md(n); break;
            case BENCH_LD_COLD: bench_sink_u = ch(FL_CHASE, n); break;
            case BENCH_MAC_IND: bench_sink_f = mi(n); break;
            case BENCH_BRANCH: bench_sink_u = br(n); break;
            case BENCH_CODE_COLD: case BENCH_CODE_WARM: bench_sink_f = t_bigcode(1.f); break;
            case BENCH_ADD_DEN: bench_sink_f = ad(n); break;
            default: bench_sink_u = ld(pool, n); break;
            }
            take(t0);
        }
        res[k] = (best * 100u + OPS[k] / 2u) / OPS[k];   /* cycles an op, x 100 */
    }
    return BENCH_N;
}
#endif
