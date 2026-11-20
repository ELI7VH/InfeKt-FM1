/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 J0KERPACK */
/* ------------------------------------------------ the user banks' store (1.18C30: six banks) --- */
/* Included by app.c (and by tools/perf/host/bk_test.c, the store's own test). The user banks' patches and their
 * extensions in the flash's data area 0xA5000..0xDBFFF (BK_LO..BK_HI): BK_NPH = 55 sectors of 4 KiB, the app's own
 * (plat_data_*), no longer the platform's store objects (app.h OBJ_UB0..: the numbers stay, see below).
 *
 * Logical sectors: BK_NLOG (54: six banks of nine) of BK_PAY = 3840 bytes each (a bank's 128 slots of 256 bytes and,
 * in its ninth, the slots' extensions: app.c ub_*, shx_*). Each lives in some physical sector, which moves with every
 * write (a log of sectors): a write goes to a free sector (erased, the payload programmed from byte 256, the header at
 * byte 0 last: the commit record), the sector the logical one was in becomes free. With all 54 written one sector is
 * free (the spare); fewer written leave more, and a write takes the next free one after the last written (bk_cur),
 * so the writes go round the area.
 *
 * The header is the platform's store record (fm1-base device/storage.c st_hdr_t, its CRC-32 and its magic "X0X0"):
 *   magic, type (the logical sector: BK_OBJ0 + L for the first 27, the old objects' numbers; BK_TYPE_X + L for the
 *   rest), slot (the physical sector: information only), seq (per logical sector, the newest wins), len (the payload's
 *   bytes; this store writes BK_PAY, the platform's objects may be shorter: past len reads as zeros), crc (of the
 *   payload), gen (rsv[0] of storage.c: a count over all writes, 0xFFFFFFFF in the platform's records), rsv, hcrc.
 * So the copies the platform wrote for the old banks C, D, E (objects OBJ_UB0 .. OBJ_UB0 + 26, an A/B pair of
 * sectors each in 0xA6000..0xDBFFF; 0xA5000 was free) already are sectors of this store: 1.18C29's banks are read
 * as they are, nothing copied at the update.
 *
 * Power-on (bk_init): every header read; for each logical sector the valid header with the highest seq (the lower
 * sector on a tie) whose payload's CRC holds is the current copy, else the next older (as storage.c's A/B pair).
 * Every other sector is free: an older copy, a write cut off (no header, or a header without its payload), erased,
 * or anything else. A power loss anywhere in a write leaves the copy before it current: its sector is not touched
 * until it is free, which it only becomes once the newer copy is written whole.
 *
 * Wear: besides the writes going round the free sectors, every 16th write (bk_gen) with a single free sector first
 * moves the logical sector right after the spare into it (a write of the same bytes), so the spare walks round the
 * area and a slot saved over and over does not wear two sectors alone (bk_test.c "wear").
 *
 * A write is staged a page (256 bytes) at a time, no 3840-byte buffer (the RAM is short): bk_stage opens the logical
 * sector (the free sector erased), copies the pages before the change from the current copy, keeps the page changed
 * in bk_pg and programs it once the next change lies past it; bk_flush (or a stage into another logical sector, or
 * one before the page held) copies the rest, writes the header and reads it all back. So the slots of a bank upload
 * cost one erase per logical sector, as the platform's staging did. Reads of the logical sector open see the staged
 * bytes. Main loop only.
 *
 * Bytes 0xF00..0xF07 of these sectors are written (the payload's 3584..3591): the update loader skips the data area
 * when it clears its records (fm1-base bsp/loader/ldr_core.c LDR_DATA_LO..HI), as 1.18C29's full objects did. */
#ifndef BK_LO
#define BK_LO 0xA5000u
#define BK_HI 0xDC000u
#endif
#define BK_SEC 4096u
#define BK_NPH ((BK_HI - BK_LO) / BK_SEC)
#define BK_POFF 256u
#define BK_PAY 3840u
#define BK_NPG (BK_PAY / 256u)
#define BK_MAGIC 0x30583058u             /* (storage.c ST_MAGIC) */
#define BK_TYPE_X 0x100u                 /* the type of logical sector L >= BK_NOLD: BK_TYPE_X + L (no object's number) */
#define BK_ROT 16u                       /* a spare's walk every BK_ROT writes */
#ifndef BK_ATTR
#define BK_ATTR
#endif
/* the includer gives BK_NLOG (the logical sectors), BK_NOLD (those that were the platform's objects) and BK_OBJ0 (the
 * first one's object number) */
_Static_assert(BK_NOLD <= BK_NLOG && BK_NLOG < BK_NPH && BK_NPH < 255u && BK_OBJ0 + BK_NOLD <= BK_TYPE_X
               && BK_PAY % 256u == 0u && BK_POFF + BK_PAY == BK_SEC, "the bank store's layout");
typedef struct {
    uint32_t magic;
    uint16_t type, slot;
    uint32_t seq, len, crc, gen, rsv;
    uint32_t hcrc;
} bk_hdr_t;
_Static_assert(sizeof(bk_hdr_t) == 32u, "storage.c's st_hdr_t");

static uint8_t bk_ph[BK_NLOG] PLAT_POOL;    /* logical sector L: its physical sector + 1, 0 none written */
static uint16_t bk_len[BK_NLOG] PLAT_POOL;  /* its payload's bytes (past them: zeros) */
static uint32_t bk_gen PLAT_POOL;           /* the highest gen written */
static uint8_t bk_cur PLAT_POOL;            /* the physical sector a write looks from for a free one */
static int16_t bk_ol PLAT_POOL;             /* the logical sector open (staged into), -1 none */
static uint8_t bk_ow PLAT_POOL, bk_opg PLAT_POOL;   /* its new physical sector, the page held in bk_pg */
static uint32_t bk_oseq PLAT_POOL, bk_ocrc PLAT_POOL;
static uint8_t bk_pg[256] PLAT_POOL __attribute__((aligned(4)));   /* the page held; the reads' buffer otherwise */
static uint32_t bk_nw[2] PLAT_POOL;         /* (tests) the sectors committed, erased */

static BK_ATTR uint32_t bk_crc(uint32_t c, const uint8_t *b, uint32_t n)   /* zlib's CRC-32 running (storage.c's
                                          * st_crc32: c 0xFFFFFFFF first, ~c at the end) */
{
    static const uint32_t T[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    while (n--) {
        c ^= *b++;
        c = (c >> 4) ^ T[c & 15u];
        c = (c >> 4) ^ T[c & 15u];
    }
    return c;
}
static uint32_t bk_addr(uint32_t ph) { return BK_LO + ph * BK_SEC; }
static BK_ATTR uint32_t bk_type(uint32_t l) { return l < BK_NOLD ? BK_OBJ0 + l : BK_TYPE_X + l; }
static BK_ATTR int bk_head(uint32_t ph, bk_hdr_t *h)   /* sector ph's header: its logical sector, -1 none valid */
{
    uint32_t l;
    if (plat_data_read(bk_addr(ph), h, sizeof *h) != 0 || h->magic != BK_MAGIC || h->len > BK_PAY
        || ~bk_crc(0xFFFFFFFFu, (const uint8_t *)h, sizeof *h - 4u) != h->hcrc)
        return -1;
    if (h->type >= BK_OBJ0 && h->type < BK_OBJ0 + BK_NOLD) l = h->type - BK_OBJ0;
    else if (h->type >= BK_TYPE_X + BK_NOLD && h->type < BK_TYPE_X + BK_NLOG) l = h->type - BK_TYPE_X;
    else return -1;
    return (int)l;
}
static BK_ATTR int bk_body(uint32_t ph, const bk_hdr_t *h)   /* its payload's CRC holds: 1 */
{
    uint32_t c = 0xFFFFFFFFu, o, k;
    for (o = 0; o < h->len; o += k) {
        k = h->len - o < 256u ? h->len - o : 256u;
        if (plat_data_read(bk_addr(ph) + BK_POFF + o, bk_pg, k) != 0) return 0;
        c = bk_crc(c, bk_pg, k);
    }
    return ~c == h->crc;
}
static BK_ATTR void bk_init(void)        /* power-on: the current copies (bk_ph, bk_len), bk_gen, bk_cur */
{
    struct { uint32_t seq; uint8_t l; } c[BK_NPH];   /* (each sector's header: its logical sector, 0xFF none) */
    bk_hdr_t h;
    uint32_t ph, l, top = 0, tph = BK_NPH - 1u;
    bk_ol = -1;
    for (l = 0; l < BK_NLOG; l++) { bk_ph[l] = 0; bk_len[l] = 0; }
    for (ph = 0; ph < BK_NPH; ph++) {
        const int r = bk_head(ph, &h);
        c[ph].l = r < 0 ? 0xFFu : (uint8_t)r;
        c[ph].seq = h.seq;
        if (r >= 0 && h.gen != 0xFFFFFFFFu && h.gen >= top) { top = h.gen; tph = ph; }
    }
    bk_gen = top;
    bk_cur = (uint8_t)((tph + 1u) % BK_NPH);
    for (l = 0; l < BK_NLOG; l++)
        for (;;) {                       /* (the newest whose payload holds; a broken one dropped, the next older) */
            int b = -1;
            for (ph = 0; ph < BK_NPH; ph++)
                if (c[ph].l == l && (b < 0 || c[ph].seq > c[b].seq)) b = (int)ph;
            if (b < 0) break;
            if (bk_head((uint32_t)b, &h) == (int)l && bk_body((uint32_t)b, &h)) {
                bk_ph[l] = (uint8_t)(b + 1);
                bk_len[l] = (uint16_t)h.len;
                break;
            }
            c[b].l = 0xFFu;
        }
}
static BK_ATTR int bk_used(uint32_t ph)  /* sector ph holds a current copy, or the one being written */
{
    uint32_t l;
    if (bk_ol >= 0 && bk_ow == ph) return 1;
    for (l = 0; l < BK_NLOG; l++)
        if (bk_ph[l] == ph + 1u) return 1;
    return 0;
}
static BK_ATTR int bk_free(uint32_t *n)  /* the first free sector from bk_cur on (n: how many are free), -1 none */
{
    uint32_t k, f = 0;
    int r = -1;
    for (k = 0; k < BK_NPH; k++) {
        const uint32_t ph = (bk_cur + k) % BK_NPH;
        if (!bk_used(ph)) { if (r < 0) r = (int)ph; f++; }
    }
    if (n) *n = f;
    return r;
}
static BK_ATTR void bk_page(uint32_t l, uint32_t pg, uint8_t *b)   /* page pg of logical sector l's current copy
                                          * (zeros past its length, or none) */
{
    const uint32_t o = pg * 256u, len = bk_ph[l] ? bk_len[l] : 0u;
    uint32_t k = 0, i;
    if (o < len) {
        k = len - o < 256u ? len - o : 256u;
        if (plat_data_read(bk_addr(bk_ph[l] - 1u) + BK_POFF + o, b, k) != 0) k = 0;   /* (a read failing: zeros, the
                                          * verify of the write after it fails or holds what it read) */
    }
    for (i = k; i < 256u; i++) b[i] = 0;
}
static BK_ATTR int bk_prog(void)         /* the page held into the sector being written, the next one held: 0 = done */
{
    const uint32_t a = bk_addr(bk_ow) + BK_POFF + bk_opg * 256u;
    bk_ocrc = bk_crc(bk_ocrc, bk_pg, 256u);
    if (plat_data_write(a, bk_pg, 256u) != 0) return -2;
    bk_opg++;
    if (bk_opg < BK_NPG) bk_page((uint32_t)bk_ol, bk_opg, bk_pg);
    return 0;
}
static BK_ATTR int bk_commit(void)       /* the logical sector open written whole, its header last, read back: 0 = done
                                          * (it is the current copy then); else the copy before stays current */
{
    bk_hdr_t h, r;
    const uint32_t l = (uint32_t)bk_ol, ph = bk_ow;
    int e = 0;
    if (bk_ol < 0) return 0;
    while (!e && bk_opg < BK_NPG) e = bk_prog();
    if (!e) {
        h.magic = BK_MAGIC;
        h.type = (uint16_t)bk_type(l);
        h.slot = (uint16_t)ph;
        h.seq = bk_oseq;
        h.len = BK_PAY;
        h.crc = ~bk_ocrc;
        h.gen = bk_gen + 1u;
        h.rsv = 0xFFFFFFFFu;
        h.hcrc = ~bk_crc(0xFFFFFFFFu, (const uint8_t *)&h, sizeof h - 4u);
        if (plat_data_write(bk_addr(ph), &h, sizeof h) != 0) e = -3;
        else if (bk_head(ph, &r) != (int)l || r.seq != h.seq || r.crc != h.crc || !bk_body(ph, &r)) e = -7;
    }
    bk_ol = -1;
    if (e) return e;
    bk_ph[l] = (uint8_t)(ph + 1u);
    bk_len[l] = (uint16_t)BK_PAY;
    bk_gen++;
    bk_cur = (uint8_t)((ph + 1u) % BK_NPH);
    bk_nw[0]++;
    return 0;
}
static BK_ATTR int bk_open1(uint32_t l)  /* logical sector l opened: a free sector erased for it, page 0 held */
{
    bk_hdr_t h;
    uint32_t ph, s = 0;
    const int f = bk_free(0);
    if (f < 0) return -4;
    for (ph = 0; ph < BK_NPH; ph++)      /* (its next seq: past every header of it, a broken newest one too) */
        if (bk_head(ph, &h) == (int)l && h.seq > s) s = h.seq;
    bk_nw[1]++;
    if (plat_data_erase(bk_addr((uint32_t)f)) != 0) return -5;
    bk_ol = (int16_t)l;
    bk_ow = (uint8_t)f;
    bk_oseq = s + 1u;
    bk_ocrc = 0xFFFFFFFFu;
    bk_opg = 0;
    bk_page(l, 0, bk_pg);
    return 0;
}
static BK_ATTR int bk_open(uint32_t l)   /* as bk_open1, the spare walked on first now and then (the head comment) */
{
    uint32_t n;
    const int f = bk_free(&n);
    if (f >= 0 && n == 1u && bk_gen % BK_ROT == BK_ROT - 1u) {
        uint32_t k, v;
        for (k = 1; k < BK_NPH; k++) {   /* (the logical sector in the next sector after the spare) */
            const uint32_t ph = ((uint32_t)f + k) % BK_NPH;
            for (v = 0; v < BK_NLOG && bk_ph[v] != ph + 1u; v++) {}
            if (v < BK_NLOG) {
                if (v != l && bk_open1(v) == 0) bk_commit();   /* (failing: the write goes on as without it) */
                break;
            }
        }
    }
    return bk_open1(l);
}
static BK_ATTR int bk_flush(void) { return bk_commit(); }
static BK_ATTR int bk_ok(void)
{
#ifdef BK_STORE_OK
    return BK_STORE_OK();
#else
    return 1;
#endif
}
/* n bytes at off into logical sector l, staged (bk_flush writes them): 0 = done; a change before the page held
 * writes the sector first */
static BK_ATTR int bk_stage(uint32_t l, uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    int e;
    if (l >= BK_NLOG || off > BK_PAY || n > BK_PAY - off) return -1;
    if (!bk_ok()) return -9;
    if (!n) return 0;
    if (bk_ol >= 0 && (bk_ol != (int)l || off / 256u < bk_opg)) {
        if ((e = bk_commit()) != 0) return e;
    }
    if (bk_ol < 0 && (e = bk_open(l)) != 0) return e;
    while (n) {
        const uint32_t pg = off / 256u, o = off % 256u, k = 256u - o < n ? 256u - o : n;
        uint32_t i;
        while (bk_opg < pg)
            if ((e = bk_prog()) != 0) { bk_ol = -1; return e; }
        for (i = 0; i < k; i++) bk_pg[o + i] = s[i];
        s += k; off += k; n -= k;
    }
    return 0;
}
/* n bytes from off of logical sector l (zeros where none was written): 0 = read */
static BK_ATTR int bk_read(uint32_t l, uint32_t off, void *dst, uint32_t n)
{
    uint8_t *d = dst;
    if (l >= BK_NLOG || off > BK_PAY || n > BK_PAY - off) return -1;
    while (n) {
        const uint32_t pg = off / 256u, o = off % 256u, k = 256u - o < n ? 256u - o : n;
        uint32_t i, len = bk_ph[l] ? bk_len[l] : 0u, m = 0;
        if (bk_ol == (int)l && pg == bk_opg) {
            for (i = 0; i < k; i++) d[i] = bk_pg[o + i];
        } else {
            uint32_t a;
            if (bk_ol == (int)l && pg < bk_opg) { a = bk_addr(bk_ow); len = BK_PAY; }
            else a = bk_ph[l] ? bk_addr(bk_ph[l] - 1u) : 0u;
            if (a && off < len) {
                m = len - off < k ? len - off : k;
                if (plat_data_read(a + BK_POFF + off, d, m) != 0) return -1;
            }
            for (i = m; i < k; i++) d[i] = 0;
        }
        d += k; off += k; n -= k;
    }
    return 0;
}
