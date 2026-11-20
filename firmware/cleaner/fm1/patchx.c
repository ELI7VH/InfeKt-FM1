/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 J0KERPACK */
/* ------------------------------------------------ SysEx: patches out, the extension frame (1.05) --- */
/* the development notes; included by app.c after the bank upload's handshake (bu_*) and before app_sysex
 * (UI_LATE, the main loop alone). Two parts:
 *
 * the dump (the page's Patches tab reads the FM-1's patches; a later patch manager reads whole banks): the bank
 * upload's handshake F0 7D 42 55 gets
 *   page -> FM-1: 05 bank slot      bank 0..5 C..H (as the upload's), 10 A, 11 B (3, 4 until 1.18C29, the hello's
 *                                   version 2), 7F the edit buffer (the part edited; slot ignored)
 *   FM-1 -> page: the single dump of that patch as stored (F0 00 20 33 01 00 10 bb pp <256> cs F7; bb 1 + bank (1..6),
 *                 00 with pp 40 for the edit buffer), then its extension frame (below) when the patch has one that is
 *                 not the defaults, then 53 bank slot status flags (status 0 sent, 1 an empty slot: nothing sent,
 *                 2 no such bank or slot; flags 1: an extension frame came before it). The frames go out at once
 *                 (fm1-base's USB sender waits for room; one request at a time from the page)
 *
 * the extension frame: what a patch carries past the 256 bytes the format has (FX > SHIMMER's
 * values, the shimmer as a target of the matrix and the LFOs), in a frame of the non-commercial id that other
 * software leaves alone ("IX"):
 *   F0 7D 49 58 01 bb pp n x0 .. x(n-1) cs F7      01 its layout; bb pp the single it follows (its bank byte and
 *                                                   program); n the bytes (20: SHX_N values 0..127, then SHX_NR
 *                                                   targets 0..15: 0 none, 1 SHIM AMT, 2 SHIM MOD (SHIM TONE before 1.18C45), as patch_ext;
 *                                                   1.18C47: 21, then W GLIDE 0..15: a frame of 20 reads its default);
 *                                                   cs (01 + bb + pp + n + x..) & 7F
 * In (page -> FM-1, after a single): armed (an upload), it is the extension of slot pp of the armed bank, if this
 * upload wrote that slot (the page sends the uploads' extension frames after all its singles: the ninth object staged
 * once); not armed, of the single just put into the edit buffer (the frame right before, the same pp). Answered
 * 51 pp status where: 0 taken, 1 checksum, 2 length or layout, 3 the flash, 4 no single for it, 5 no extension in this
 * build. A single without one still loads with the defaults (the shimmer off), as before */
#define PX_FR ((uint8_t *)(void *)cv_px) /* a frame out: the canvas, as the wave bank's and the editor's (lcd_sync first) */
#define PX_BANK_A 0x10u                  /* (3, 4 until 1.18C29: banks F, G now) */
#define PX_BANK_B 0x11u
_Static_assert(UB_BANKS < PX_BANK_A, "the dump's bank numbers");
#define PX_EDIT 0x7Fu
#ifdef HYB_SHIMMER
#define PX_XN SHX_XN                     /* the extension frame's bytes: the values, the targets, W GLIDE (1.18C47) */
static UI_LATE void shx_rec_x(const uint8_t *x, uint8_t *r)   /* an extension x (patch_ext's form) as a record */
{
    uint8_t sh[SH_LEN];                  /* (the panel's form: shx_rec_make's; the patch bytes not read but the targets) */
    uint32_t k;
    for (k = 0; k < SHX_NR; k++) sh[SHX_RB[k]] = 0;
    shx_put(x, sh);
    shx_rec_make(sh, r);
}
static UI_LATE uint32_t px_ext_frame(uint8_t *f, const uint8_t *x, uint32_t bb, uint32_t pp)   /* the frame: its bytes */
{
    uint32_t k, s, m = 0;
    f[m++] = 0xF0; f[m++] = 0x7D; f[m++] = 0x49; f[m++] = 0x58; f[m++] = 0x01;
    f[m++] = (uint8_t)(bb & 0x7Fu); f[m++] = (uint8_t)(pp & 0x7Fu); f[m++] = (uint8_t)PX_XN;
    for (k = 0; k < PX_XN; k++) f[m++] = (uint8_t)(x[k] & 0x7Fu);
    for (s = 0, k = 4; k < m; k++) s += f[k];
    f[m++] = (uint8_t)(s & 0x7Fu);
    f[m++] = 0xF7;
    return m;
}
#endif
static UI_LATE void px_dump(uint32_t bank, uint32_t slot)   /* 05: the patch's single (and extension), then 53 */
{
    uint8_t *f = PX_FR;
    const uint8_t *d = 0;
    uint32_t k, s, bb = 1u + bank, pp = slot, st = 0, fl = 0;
#ifdef HYB_SHIMMER
    const uint8_t *x = SHX_DEF0;
    uint8_t xe[PX_XN];
#endif
    lcd_sync();                          /* (the canvas is the frame) */
    if (bank == PX_EDIT) { bb = 0; pp = 0x40u; }
    else if (slot >= UB_SLOTS) st = 2;
    else if (bank < UB_BANKS) {
        const uint32_t u = bank * UB_SLOTS + slot;
        if (!ub_filled(u)) st = 1;
        else {
#ifdef HYB_SHIMMER
            x = patch_ext(PU_0 + u);
#endif
            d = ub_bytes(u);
        }
    } else if (bank == PX_BANK_A) {
        if (slot < BANK_N) d = patch_bytes(slot); else st = 1;
        bb = 1;
    } else if (bank == PX_BANK_B) {
        st = 1; (void)slot;              /* (no factory banks) */
        bb = 2;
    } else st = 2;
    if (!st) {
        f[0] = 0xF0; f[1] = 0x00; f[2] = 0x20; f[3] = 0x33; f[4] = 0x01; f[5] = 0x00; f[6] = 0x10;
        f[7] = (uint8_t)bb; f[8] = (uint8_t)pp;
        if (bank == PX_EDIT) {           /* (the panel's: a destination on the shimmer off, as a save stores it) */
            for (k = 0; k < 256u; k++) f[9u + k] = (uint8_t)(SHX_EB(k, shadow[k]) & 0x7Fu);
#ifdef HYB_SHIMMER
            for (k = 0; k < SHX_N; k++) xe[k] = shadow[256u + k];
            for (k = 0; k < SHX_NR; k++)
                xe[SHX_N + k] = (uint8_t)(shadow[SHX_RB[k]] >= SHX_VD ? shadow[SHX_RB[k]] - SHX_VD + 1u : 0u);
            xe[SHX_GX] = shadow[256u + SHX_GL];
            x = xe;
#endif
        } else
            for (k = 0; k < 256u; k++) f[9u + k] = (uint8_t)(d[k] & 0x7Fu);
        for (s = 0, k = 5; k < 265u; k++) s += f[k];
        f[265] = (uint8_t)(s & 0x7Fu); f[266] = 0xF7;
        plat_sysex_send(f, 267u);
#ifdef HYB_SHIMMER
        for (k = 0; k < PX_XN && x[k] == SHX_DEF0[k]; k++) {}
        if (k < PX_XN) {                 /* (the defaults, no target: no frame, as the store keeps no record) */
            plat_sysex_send(f, px_ext_frame(f, x, bb, pp));
            fl = 1;
        }
#endif
    }
    {
        uint8_t a[10] = {0xF0, 0x7D, 0x42, 0x55, 0x53, 0, 0, 0, 0, 0xF7};
        a[5] = (uint8_t)(bank & 0x7Fu); a[6] = (uint8_t)(slot & 0x7Fu); a[7] = (uint8_t)st; a[8] = (uint8_t)fl;
        plat_sysex_send(a, sizeof a);
    }
}
/* the patch manager (the page's, hello version 4): a bank's names at once, and a slot emptied within an upload
 *   page -> FM-1: 07 bank      bank 0..5 (C..H as the upload's)
 *   FM-1 -> page: 54 bank part st <PX_NPART slots: f n0 .. n9> four times (part 0..3, slots 32 part ..): f 1 a filled
 *                 slot, n its name's 10 bytes (& 7F); f 0 an empty one (its name 0s). st 2: no such bank (one frame,
 *                 part 0, no slots). From the store a name at a time (bk_read: no RAM of its own; the frame the canvas)
 *   page -> FM-1: 06 slot      armed (02): slot of the armed bank emptied (its 256 bytes 0s staged as a single's, its
 *                              extension off at the end as a single's without one); one explicit write of the page's
 *                              (its "write to the device", as SAVE), never by itself
 *   FM-1 -> page: 52 slot st where   st 0 done, 2 no such slot, 3 the flash, 4 not armed; where 1 + the bank (0: not
 *                                    armed) */
#define PX_NPART 32u
_Static_assert(UB_SLOTS == 4u * PX_NPART, "the names' four parts");
static UI_LATE void px_names(uint32_t bank)   /* 07: the bank's names */
{
    uint8_t *f = PX_FR;
    uint32_t c, k, i, m;
    lcd_sync();                          /* (the canvas is the frame) */
    for (c = 0; c < 4u; c++) {
        f[0] = 0xF0; f[1] = 0x7D; f[2] = 0x42; f[3] = 0x55; f[4] = 0x54;
        f[5] = (uint8_t)(bank & 0x7Fu); f[6] = (uint8_t)c; f[7] = (uint8_t)(bank < UB_BANKS ? 0u : 2u);
        m = 8;
        if (bank < UB_BANKS)
            for (k = 0; k < PX_NPART; k++) {
                const uint32_t u = bank * UB_SLOTS + c * PX_NPART + k;
                uint8_t *q = f + m + 1u;
                f[m] = 0;
                for (i = 0; i < 10u; i++) q[i] = 0;
                if (ub_filled(u) && bk_read(ub_obj(u), ub_off(u) + 240u, q, 10u) == 0) {
                    f[m] = 1;
                    for (i = 0; i < 10u; i++) q[i] &= 0x7Fu;
                }
                m += 11u;
            }
        f[m++] = 0xF7;
        plat_sysex_send(f, m);
        if (bank >= UB_BANKS) break;
    }
}
static UI_LATE void px_clear(uint32_t pp)   /* 06: slot pp of the armed bank emptied (staged) */
{
    const uint32_t where = bu.on ? 1u + bu.bank : 0u;
    uint32_t k, st = 0;
    if (!bu.on) st = 4;
    else if (pp >= UB_SLOTS) st = 2;
    else {
        const uint32_t u = bu.bank * UB_SLOTS + pp;
        for (k = 0; k < 256u; k++) ub_cache[k] = 0;
        if (ub_stage(u) != 0) { st = 3; bu.err = 1; }
        else {
            bu.saved++;
            ub_full[u >> 3] &= (uint8_t)~(1u << (u & 7u));   /* (ub_stage marks it filled) */
        }
        ub_cached = 0;
#ifdef HYB_SHIMMER
        bu_x[pp >> 3] |= (uint8_t)(1u << (pp & 7u));   /* (its extension off at the end: bu_ext_off) */
#endif
        bu.got++;
        bu.t = plat_ms();
        bu_show(0, 4000u);
    }
    bu_send(0x52, pp, st, where, 3);
}
static UI_LATE void px_ext(const uint8_t *p, uint32_t n, uint32_t xp)   /* an extension frame in (p: 7D 49 58 ..);
                                          * xp: the edit buffer's single's pp + 1 if it came right before */
{
    const uint32_t pp = n > 5u ? p[5] & 0x7Fu : 0u, where = bu.on ? 1u + bu.bank : 0u;
    uint32_t st = 0;
#ifdef HYB_SHIMMER
    uint32_t k, s = 0, m = n > 6u ? p[6] : 0u;
    uint8_t x[PX_XN];
    if (n < 8u || p[3] != 0x01u || n != 8u + m || m > 64u) st = 2;
    else {
        for (k = 3; k < n - 1u; k++) s += p[k];
        if ((s & 0x7Fu) != p[n - 1u]) st = 1;
    }
    if (!st) {                           /* (a longer frame of a later layout: its first bytes; a shorter: the defaults
                                          * for the rest; a target this firmware does not know: none) */
        for (k = 0; k < PX_XN; k++) {
            x[k] = k < m ? p[7u + k] : SHX_DEF0[k];
            if (k >= SHX_N && k < SHX_GX && x[k] > SHX_NT) x[k] = 0;
            if (k == SHX_GX && x[k] > 15u) x[k] = 15u;
        }
        if (where) {
            const uint32_t u = bu.bank * UB_SLOTS + pp;
            uint8_t r[SHX_REC];
            if (!(bu_x[pp >> 3] >> (pp & 7u) & 1u)) st = 4;
            else {
                shx_rec_x(x, r);
                if (ub_put(shx_obj(u), shx_off(u), r, SHX_REC) != 0) { st = 3; bu.err = 1; }
                else bu_x[pp >> 3] &= (uint8_t)~(1u << (pp & 7u));   /* (its record set: not cleared at the end) */
                shx_cu = 0;
            }
            bu.t = plat_ms();
        } else if (xp != pp + 1u || !sx_edit) st = 4;
        else {
            for (k = 0; k < SHX_NR; k++) shadow[SHX_RB[k]] = (uint8_t)SHX_EB(SHX_RB[k], shadow[SHX_RB[k]]);
            shx_put(x, shadow);
            SHX_SYNC();                  /* (part A's to the reverb, the targets to the engine) */
            ui.d_lcd = ui.d_rows = 1;
        }
    }
#else
    (void)n; (void)xp;
    st = 5;
#endif
    bu_send(0x51, pp, st, where, 3);
}
