/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 J0KERPACK */
/* bk_test.c: the user banks' store (cleaner/fm1/bankstore.c, 1.18C30) on a simulated NOR flash, against the platform's
 * own store (fm1-base device/storage.c) as 1.18C29 used it for banks C, D, E. Built and run by bk_test.sh.
 *
 *   migrate   1.18C29 images (the banks, their extensions, staged uploads, short objects, A/B copies, writes of the
 *             old store cut off by a power loss, junk in the free sector): after bk_init every slot and extension reads
 *             as the old store read it (storage.c st_load_part, its zeros past an object's end), banks D-F empty, and
 *             bk_init writes nothing
 *   random    saves into all six banks (single slots, slot + extension, staged uploads, reads of the sector open):
 *             a model compared after each, and after a reboot
 *   power     a power loss at every erase / page program / header write of a save (the flash torn as a cut write
 *             leaves it: a random part of the bytes written, of the bits erased), from a migrated store, a full one
 *             (one spare) and at the spare's walk: after the reboot each logical sector is its old or its new
 *             content, never anything else, and the store writes on (a second save and reboot checked)
 *   full      all 768 slots and their extensions written, read back, rebooted, read back
 *   wear      one slot saved 20000 times, then random slots: the erases per physical sector
 *   boot      bk_init's flash reads on a full store (bytes, calls), its host time
 * Exit status 0: all passed. */
#define PLAT_HOST 1
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <time.h>
#include "plat.h"
#include "app.h"

#define FL_SIZE 0x100000u
#define BK_LO 0xA5000u                   /* (bankstore.c's) */
#define BK_HI 0xDC000u
static uint8_t nor[FL_SIZE];
static int fail_at = -1, ops;            /* the op (erase or program) that the power loss cuts, -1 none */
static jmp_buf cut;
static uint32_t rd_bytes, rd_calls, er_n[64];
static uint64_t rs = 88172645463325252ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)rs; }
static uint64_t rt = 1442695040888963407ull;   /* (the torn bytes' own numbers) */
static uint32_t rtn(void) { rt ^= rt << 13; rt ^= rt >> 7; rt ^= rt << 17; return (uint32_t)rt; }
static int nfail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); \
                                       if (++nfail > 20) exit(1); } } while (0)

static void torn_prog(uint32_t off, const uint8_t *s, uint32_t n)   /* a program cut off: some bytes, some bits */
{
    uint32_t i, mode = rtn() % 3u, cutn = rtn() % (n + 1u);
    for (i = 0; i < n; i++) {
        uint8_t v = s[i];
        if (mode == 0u && i >= cutn) break;                       /* a prefix */
        if (mode == 1u && rtn() % 2u) continue;                   /* a random part of the bytes */
        if (mode == 2u) v |= (uint8_t)rtn();                      /* a random part of the bits (0s not all made) */
        nor[off + i] &= v;
    }
}
static void torn_erase(uint32_t off)     /* an erase cut off: unchanged, a part of the bits set, or all */
{
    uint32_t i, mode = rtn() % 3u;
    if (mode == 0u) return;
    for (i = 0; i < 4096u; i++)
        nor[off + i] |= mode == 2u ? 0xFFu : (uint8_t)(rtn() & rtn());
}
static int fl_erase(uint32_t off)
{
    if ((off & 0xFFFu) || off >= FL_SIZE) return -1;
    if (ops++ == fail_at) { torn_erase(off); longjmp(cut, 1); }
    if (off >= BK_LO && off < BK_HI) er_n[(off - BK_LO) >> 12]++;
    memset(nor + off, 0xFF, 4096u);
    return 0;
}
static int fl_prog(uint32_t off, const void *src, uint32_t n)
{
    uint32_t i;
    if (off + n > FL_SIZE) return -1;
    if (ops++ == fail_at) { torn_prog(off, src, n); longjmp(cut, 1); }
    for (i = 0; i < n; i++) nor[off + i] &= ((const uint8_t *)src)[i];
    return 0;
}
/* the platform's data area (plat.h) */
int plat_data_read(uint32_t off, void *dst, uint32_t n)
{
    if (!PLAT_DATA_OK(off, n)) return -1;
    rd_bytes += n; rd_calls++;
    memcpy(dst, nor + off, n);
    return 0;
}
int plat_data_erase(uint32_t off) { return PLAT_DATA_OK(off, 4096u) ? fl_erase(off) : -1; }
int plat_data_write(uint32_t off, const void *src, uint32_t n) { return PLAT_DATA_OK(off, n) ? fl_prog(off, src, n) : -1; }
/* the old store: storage.c over the same flash */
static int st_read(uint32_t off, void *dst, uint32_t n) { memcpy(dst, nor + off, n); return 0; }
static int st_erase(uint32_t off) { return fl_erase(off); }
static int st_prog(uint32_t off, const void *src, uint32_t n) { return fl_prog(off, src, n); }
#include "storage.c"                    /* (fm1-base device/storage.c) */

/* the store under test, as app.c includes it */
#define BK_NLOG 54u
#define BK_NOLD 27u
#define BK_OBJ0 OBJ_UB0
#include "bankstore.c"

/* app.c's places: slot s of bank b, its extension */
#define NB 6u
#define SLOTS 128u
#define XREC 14u
#define XOFF 2048u
static uint32_t lsec(uint32_t b, uint32_t s) { return b * 9u + s / 15u; }
static uint32_t loff(uint32_t s) { return s % 15u * 256u; }
static uint32_t xsec(uint32_t b) { return b * 9u + 8u; }
static uint32_t xoff(uint32_t s) { return XOFF + s * XREC; }

/* a model: each logical sector's 3840 bytes */
static uint8_t model[BK_NLOG][BK_PAY], prev[BK_NLOG][BK_PAY];
static void reboot(void) { bk_init(); }
static int same_sector(uint32_t l, const uint8_t *m)
{
    uint8_t b[BK_PAY];
    if (bk_read(l, 0, b, BK_PAY) != 0) return 0;
    return !memcmp(b, m, BK_PAY);
}
static void check_all(const char *what)
{
    uint32_t l;
    for (l = 0; l < BK_NLOG; l++)
        CHECK(same_sector(l, model[l]), "%s: logical sector %u differs", what, l);
}
static void fill_patch(uint8_t *p, uint32_t tag)
{
    uint32_t k;
    for (k = 0; k < 256u; k++) p[k] = (uint8_t)(rnd() & 0x7Fu);
    p[240] = (uint8_t)('A' + tag % 26u);
}
static void fill_x(uint8_t *x)
{
    uint32_t k;
    x[0] = 0x53; x[1] = 6;
    for (k = 2; k < XREC; k++) x[k] = (uint8_t)rnd();
}

/* ---- the old store (1.18C29): its writes as app.c made them ---- */
static void old_save(uint32_t b, uint32_t s, int ext, int stage)   /* ub_write / ub_stage of 1.18C29 */
{
    uint8_t p[256], x[XREC];
    fill_patch(p, s);
    fill_x(x);
    if (stage) { st_stage(OBJ_UB0 + lsec(b, s), loff(s), p, 256u); if (ext) st_stage(OBJ_UB0 + xsec(b), xoff(s), x, XREC); }
    else if (ext) { st_stage(OBJ_UB0 + lsec(b, s), loff(s), p, 256u); st_stage(OBJ_UB0 + xsec(b), xoff(s), x, XREC); st_flush(); }
    else st_save_part(OBJ_UB0 + lsec(b, s), loff(s), p, 256u);
}
static void old_view(void)               /* the model: what the old store reads (zeros past an object's end) */
{
    uint32_t l;
    memset(model, 0, sizeof model);
    for (l = 0; l < BK_NOLD; l++) {
        const int n = st_size(OBJ_UB0 + l);
        if (n > 0) st_load_part(OBJ_UB0 + l, 0, model[l], (uint32_t)n);
    }
}
static void make_old_image(uint32_t seed, int torn)
{
    uint32_t i, b, n;
    rs = seed * 2654435761u + 1u;
    memset(nor, 0xFF, sizeof nor);
    st_held = -1; st_held_err = 0;
    if (seed % 3u == 1u) for (i = 0; i < 4096u; i++) nor[BK_LO + i] = (uint8_t)rnd();   /* junk in the free sector */
    if (seed % 3u == 2u) {                /* a valid store record of another object there */
        uint8_t z[64] = {0};
        st_save(OBJ_SETTINGS, z, 64);    /* (its own sector 0xFC000) */
        memcpy(nor + BK_LO, nor + 0xFC000u, 4096u);
    }
    n = 50u + rnd() % 600u;
    for (i = 0; i < n; i++) {
        b = rnd() % 3u;
        old_save(b, rnd() % SLOTS, (int)(rnd() % 3u == 0u), (int)(rnd() % 4u == 0u));
    }
    for (b = 0; b < 3u; b++)              /* a bank upload: staged, its extensions at the end */
        if (rnd() % 2u) {
            uint32_t s0 = rnd() % 64u, s;
            for (s = s0; s < s0 + 40u; s++) old_save(b, s, 0, 1);
            for (s = s0; s < s0 + 40u; s += 3u) { uint8_t x[XREC]; fill_x(x); st_stage(OBJ_UB0 + xsec(b), xoff(s), x, XREC); }
            st_flush();
        }
    st_flush();
    if (torn) {                           /* a save of the old store cut off by a power loss */
        int sv = ops;
        fail_at = ops + (int)(rnd() % 20u);
        if (!setjmp(cut)) { for (i = 0; i < 20u; i++) old_save(rnd() % 3u, rnd() % SLOTS, (int)(rnd() % 2u), 0); }
        fail_at = -1; (void)sv;
        st_held = -1; st_held_err = 0;
    }
}
static void t_migrate(void)
{
    static uint8_t snap[BK_HI - BK_LO];
    uint32_t seed, l, filled = 0;
    for (seed = 1; seed <= 300u; seed++) {
        make_old_image(seed, (int)(seed % 2u));
        old_view();
        memcpy(snap, nor + BK_LO, sizeof snap);
        reboot();
        CHECK(!memcmp(snap, nor + BK_LO, sizeof snap), "migrate %u: bk_init wrote the flash", seed);
        check_all("migrate");
        for (l = 0; l < BK_NLOG; l++) filled += bk_ph[l] != 0;
        /* and writes on: a slot of bank F, one of bank A, rebooted */
        {
            uint8_t p[256];
            fill_patch(p, 1); CHECK(bk_stage(lsec(5, 77), loff(77), p, 256) == 0 && bk_flush() == 0, "migrate: save F");
            memcpy(model[lsec(5, 77)] + loff(77), p, 256);
            fill_patch(p, 2); CHECK(bk_stage(lsec(0, 3), loff(3), p, 256) == 0 && bk_flush() == 0, "migrate: save A");
            memcpy(model[lsec(0, 3)] + loff(3), p, 256);
            reboot();
            check_all("migrate+save");
        }
    }
    printf("  ok   migrate: 300 images of 1.18C29 (half with a save of the old store cut off), %u sectors found\n", filled);
}

/* ---- the app's kinds of save on the model ---- */
static int do_save(uint32_t kind, uint32_t b, uint32_t s)   /* 0 = done; the model gets the new bytes */
{
    uint8_t p[256], x[XREC];
    int e = 0;
    uint32_t k;
    switch (kind) {
    case 0:                               /* a slot (ub_write, no extension) */
        fill_patch(p, s);
        e = bk_stage(lsec(b, s), loff(s), p, 256u); if (!e) e = bk_flush();
        if (!e) memcpy(model[lsec(b, s)] + loff(s), p, 256u);
        break;
    case 1:                               /* a slot and its extension (two sectors, or one: the ninth) */
        fill_patch(p, s); fill_x(x);
        e = bk_stage(lsec(b, s), loff(s), p, 256u); if (!e) e = bk_stage(xsec(b), xoff(s), x, XREC);
        if (!e) e = bk_flush();
        if (!e) { memcpy(model[lsec(b, s)] + loff(s), p, 256u); memcpy(model[xsec(b)] + xoff(s), x, XREC); }
        break;
    default:                              /* an upload: 20 slots on from s, then extensions of every 2nd, read between */
        for (k = s; !e && k < s + 20u && k < SLOTS; k++) {
            uint8_t r[256];
            fill_patch(p, k);
            e = bk_stage(lsec(b, k), loff(k), p, 256u);
            if (!e) memcpy(model[lsec(b, k)] + loff(k), p, 256u);
            if (!e && bk_read(lsec(b, k), loff(k), r, 256u) == 0) CHECK(!memcmp(r, p, 256u), "a staged slot read back");
            if (!e && k > s && bk_read(lsec(b, k - 1u), loff(k - 1u), r, 256u) == 0)
                CHECK(!memcmp(r, model[lsec(b, k - 1u)] + loff(k - 1u), 256u), "the slot before read back");
        }
        for (k = s; !e && k < s + 20u && k < SLOTS; k += 2u) {
            fill_x(x);
            e = bk_stage(xsec(b), xoff(k), x, XREC);
            if (!e) memcpy(model[xsec(b)] + xoff(k), x, XREC);
        }
        if (!e) e = bk_flush();
        break;
    }
    return e;
}
static void t_random(void)
{
    uint32_t i;
    make_old_image(7, 0);
    old_view();
    reboot();
    for (i = 0; i < 3000u; i++) {
        const uint32_t b = rnd() % NB;
        CHECK(do_save(rnd() % 3u, b, rnd() % SLOTS) == 0, "random save %u", i);
        if (i % 50u == 0u) check_all("random");
        if (i % 97u == 0u) { reboot(); check_all("random, rebooted"); }
    }
    reboot();
    check_all("random end");
    printf("  ok   random: 3000 saves into six banks (slots, extensions, uploads), checked against a model\n");
}

/* ---- power loss at every flash op of a save ---- */
static int sector_is(uint32_t l, const uint8_t *a, const uint8_t *b) { return same_sector(l, a) || same_sector(l, b); }
static void power_from(const char *name, uint32_t kinds)
{
    static uint8_t snap[BK_HI - BK_LO];
    static uint8_t m0[BK_NLOG][BK_PAY];
    uint32_t kind, cases = 0, rot = 0;
    for (kind = 0; kind < 3u; kind++) {
        int nops, i;
        uint64_t rs0;
        uint32_t b, s, t;
        if (!(kinds >> kind & 1u)) continue;
        for (t = 0; t < 6u; t++) {
            b = rnd() % NB; s = kind == 1u ? rnd() % SLOTS : rnd() % 100u;
            if (t == 5u) s = 125u;        /* (a slot of the ninth sector: slot and extension in one) */
            memcpy(snap, nor + BK_LO, BK_HI - BK_LO);
            memcpy(m0, model, sizeof model);
            rs0 = rs;
            ops = 0;
            { const uint32_t g = bk_gen; uint32_t nf; bk_free(&nf); rot += nf == 1u && g % BK_ROT == BK_ROT - 1u; }
            CHECK(do_save(kind, b, s) == 0, "%s: the save itself", name);
            nops = ops;
            memcpy(prev, model, sizeof model);   /* (prev: the new content) */
            for (i = 0; i < nops; i++) {
                uint32_t l, rep;
                for (rep = 0; rep < 3u; rep++) {
                    memcpy(nor + BK_LO, snap, BK_HI - BK_LO);
                    memcpy(model, m0, sizeof model);
                    reboot();
                    rs = rs0 + rep * 7919u + (uint64_t)i;
                    ops = 0; fail_at = i;
                    if (!setjmp(cut)) { do_save(kind, b, s); CHECK(0, "%s: no power loss at op %d", name, i); }
                    fail_at = -1;
                    /* (the bytes this run made: regenerate the new content with the same random numbers) */
                    {
                        static uint8_t keep[BK_HI - BK_LO];
                        memcpy(keep, nor + BK_LO, sizeof keep);
                        memcpy(nor + BK_LO, snap, sizeof keep);
                        memcpy(model, m0, sizeof model);
                        reboot();
                        rs = rs0 + rep * 7919u + (uint64_t)i;
                        do_save(kind, b, s);
                        memcpy(prev, model, sizeof model);
                        memcpy(nor + BK_LO, keep, sizeof keep);
                    }
                    reboot();
                    for (l = 0; l < BK_NLOG; l++)
                        CHECK(sector_is(l, m0[l], prev[l]), "%s: kind %u op %d/%d: logical sector %u neither old nor new",
                              name, kind, i, nops, l);
                    cases++;
                    /* the store writes on: the model as the flash holds it now, another save, a reboot */
                    for (l = 0; l < BK_NLOG; l++) bk_read(l, 0, model[l], BK_PAY);
                    CHECK(do_save(0, (b + 1u) % NB, (s + 7u) % SLOTS) == 0, "%s: a save after the power loss", name);
                    reboot();
                    check_all("after a power loss and a save");
                }
            }
            memcpy(nor + BK_LO, snap, BK_HI - BK_LO);   /* back, and on with the save done whole */
            memcpy(model, m0, sizeof model);
            reboot();
            rs = rs0;
            do_save(kind, b, s);
            reboot();
            check_all("power: the save done");
        }
    }
    printf("  ok   power %s: %u power losses (each erase, page program, header: torn 3 ways), %u at the spare's walk\n",
           name, cases, rot);
}
static void fill_all(void)
{
    uint32_t b, s;
    for (b = 0; b < NB; b++) {
        for (s = 0; s < SLOTS; s++) {
            uint8_t p[256], x[XREC];
            fill_patch(p, s); fill_x(x);
            CHECK(bk_stage(lsec(b, s), loff(s), p, 256u) == 0, "fill");
            memcpy(model[lsec(b, s)] + loff(s), p, 256u);
            memcpy(model[xsec(b)] + xoff(s), x, XREC);
        }
        for (s = 0; s < SLOTS; s++)
            CHECK(bk_stage(xsec(b), xoff(s), model[xsec(b)] + xoff(s), XREC) == 0, "fill x");
        CHECK(bk_flush() == 0, "fill flush");
    }
}
static void t_power(void)
{
    uint32_t i;
    make_old_image(11, 0); old_view(); reboot();
    power_from("migrated", 7u);
    fill_all(); reboot(); check_all("full");
    power_from("full", 7u);
    for (i = 0; i < 40u; i++) {           /* to the spare's walk: bk_gen % BK_ROT == BK_ROT - 1 */
        while (bk_gen % BK_ROT != BK_ROT - 1u) do_save(0, rnd() % NB, rnd() % SLOTS);
        power_from("walk", 1u);
        if (i > 2u) break;
    }
}
static void t_full(void)
{
    uint32_t b, s, n = 0;
    memset(nor, 0xFF, sizeof nor);
    memset(model, 0, sizeof model);
    reboot();
    fill_all();
    check_all("full");
    reboot();
    check_all("full, rebooted");
    for (b = 0; b < NB; b++)
        for (s = 0; s < SLOTS; s++) { uint8_t c; bk_read(lsec(b, s), loff(s) + 240u, &c, 1); n += c != 0; }
    CHECK(n == NB * SLOTS, "full: %u slots filled", n);
    { uint32_t f; bk_free(&f); CHECK(f == 1u, "full: %u free sectors", f); }
    printf("  ok   full: %u slots and their extensions in 54 sectors, one spare\n", n);
}
static void t_rot(void)                  /* a payload's bit rotten in the newest copy: the one before it (as storage.c) */
{
    uint32_t l = lsec(1, 20), ph, n = 0;
    static uint8_t m0[BK_PAY];
    memset(nor, 0xFF, sizeof nor); memset(model, 0, sizeof model); reboot();
    CHECK(do_save(0, 1, 20) == 0, "rot: save 1");
    memcpy(m0, model[l], BK_PAY);
    CHECK(do_save(0, 1, 21) == 0, "rot: save 2");
    ph = bk_ph[l] - 1u;
    nor[BK_LO + ph * 4096u + 256u + 1000u] ^= 0x04u;
    reboot();
    CHECK(same_sector(l, m0), "rot: not the copy before");
    CHECK(do_save(0, 1, 22) == 0, "rot: a save after");
    memcpy(m0 + loff(22), model[l] + loff(22), 256u); memcpy(model[l], m0, BK_PAY);
    { uint8_t b[256]; bk_read(l, loff(22), b, 256u); n = !memcmp(b, m0 + loff(22), 256u); }
    reboot();
    CHECK(n && same_sector(l, model[l]), "rot: the save after it");
    printf("  ok   rot: a rotten newest copy: the one before it read, the next save on top of that\n");
}
static void t_wear(void)
{
    uint32_t i, mx = 0, mn = ~0u, tot = 0;
    memset(er_n, 0, sizeof er_n);
    for (i = 0; i < 20000u; i++) CHECK(do_save(0, 2, 5) == 0, "wear");
    for (i = 0; i < BK_NPH; i++) { if (er_n[i] > mx) mx = er_n[i]; if (er_n[i] < mn) mn = er_n[i]; tot += er_n[i]; }
    printf("  ok   wear: one slot saved 20000 times on a full store: erases per sector %u .. %u (all %u; the A/B pair "
           "of 1.18C29: 10000 each on two)\n", mn, mx, tot);
    CHECK(mx < 1500u && mn > 0u, "wear: the writes did not go round");
    memset(er_n, 0, sizeof er_n); mx = 0; mn = ~0u;
    for (i = 0; i < 5000u; i++) do_save(rnd() % 2u, rnd() % NB, rnd() % SLOTS);
    for (i = 0; i < BK_NPH; i++) { if (er_n[i] > mx) mx = er_n[i]; if (er_n[i] < mn) mn = er_n[i]; }
    printf("  ok   wear: 5000 random saves: erases per sector %u .. %u\n", mn, mx);
    reboot();
    check_all("wear");
}
static void t_boot(void)
{
    struct timespec a, b;
    uint32_t i;
    rd_bytes = rd_calls = 0;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (i = 0; i < 100u; i++) reboot();
    clock_gettime(CLOCK_MONOTONIC, &b);
    printf("  ok   boot: bk_init on a full store reads %u bytes in %u reads (host %.3f ms)\n", rd_bytes / 100u,
           rd_calls / 100u, ((b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6) / 100.);
}
static int mkimg(const char *in, const char *out)   /* a simulator's store (hostcmd.c stdump of 1.18C29) as the
                                          * device's flash holds it: banks C, D, E's objects written by storage.c (some
                                          * twice: a stale copy beside the current one, as on a used FM-1) */
{
    static uint8_t x[PLAT_STORE_MAX], y[PLAT_STORE_MAX];
    FILE *f = fopen(in, "rb"), *g;
    uint32_t o, n2 = 0;
    int32_t n;
    if (!f) return 1;
    memset(nor, 0xFF, sizeof nor);
    for (o = 0; o < OBJ_NOBJ && fread(&n, 4, 1, f) == 1; o++) {
        if (n < 0 || n > (int32_t)sizeof x || fread(x, 1, (size_t)n, f) != (size_t)n) return 1;
        if (o < OBJ_UB0 || o >= OBJ_UB0 + 27u || n <= 0) continue;
        if (o % 3u) {                    /* (an older version first: the current copy in B, or with a third save in A) */
            memcpy(y, x, (size_t)n);
            y[0] ^= 0x55u; y[n - 1] ^= 0x22u;
            st_save(o, y, (uint32_t)n);
            if (o % 3u == 2u) st_save(o, y, (uint32_t)n / 2u + 1u);
            n2++;
        }
        if (st_save(o, x, (uint32_t)n)) return 1;
    }
    fclose(f);
    if (!(g = fopen(out, "wb")) || fwrite(nor + BK_LO, 1, BK_HI - BK_LO, g) != BK_HI - BK_LO) return 1;
    fclose(g);
    printf("  ok   mkimg: %s -> %s (%u objects with an older copy beside)\n", in, out, n2);
    return 0;
}
int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "mkimg")) return mkimg(argv[2], argv[3]);
    const char *only = argc > 1 ? argv[1] : "";   /* (a test alone: its name) */
    memset(nor, 0xFF, sizeof nor);
    if (!*only || !strcmp(only, "migrate")) t_migrate();
    if (!*only || !strcmp(only, "random")) t_random();
    if (!*only || !strcmp(only, "full")) t_full();
    if (!*only || !strcmp(only, "rot")) t_rot();
    if (!*only || !strcmp(only, "power")) t_power();
    if (!*only || !strcmp(only, "wear")) t_wear();
    if (!*only || !strcmp(only, "boot")) t_boot();
    if (nfail) { printf("FAIL %d\n", nfail); return 1; }
    printf("all passed\n");
    return 0;
}
