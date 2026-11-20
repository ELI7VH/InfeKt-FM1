/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 J0KERPACK */
/* splash.c (included by app.c after waves.c): the boot image, 0.99.
 *
 * A 240 x 240 picture in the flash outside the program (the program's flash is full): PLAT_XDATA1's last five
 * sectors, 0xF7000..0xFBFFF (the wave bank's two regions end below them), read through the flash's driver (below).
 * The update loader looks for its record at 0xF00 of every sector (plat.h): a chunk that would put "AT" at a
 * sector's 0xF06 is refused (and mk_splash.py never makes one). Written only by an upload over USB (the page's
 * Installation tab; SysEx below). At power-on fm1-base calls app_splash (APP_SPLASH) before app_init: a stored image
 * that checks (magic, size, CRC-32) is drawn, else fm1-base draws the name (APP_SPLASH_RGB on APP_SPLASH_BG, app.h).
 *
 * The image (mk_splash.py): "IKSP", version 1, k (the bits of a colour's index), ncol, 0, w and h (240, 240: u16),
 * len (u32: the runs' bytes), the palette (ncol RGB565 values, high byte first: the panel's order, as the canvas holds
 * them), the runs in raster order (a byte r << k | index; r = rmax = 255 >> k: the run rmax + 1 + the next byte, else
 * r + 1), then the CRC-32 (zlib) of all before it. The firmware checks the magic, the size, the length and the CRC
 * (a later layout takes another magic); the runs and indices of an image that checks are the generator's. Decoded a
 * band of BI_ROWS rows at a time into the canvas (no RAM of its own).
 *
 * SysEx: the wave bank's frames (waves.c, F0 7D 56 46 cmd ... F7), commands 20-22, each answered as the wave bank's
 * (F0 7D 56 46 50 cmd k status c3 F7; status UWS_*, or the check's BI_*). USB: a chunk's frame (593 bytes) is larger
 * than Bluetooth MIDI's 400.
 *   20 k d..      chunk k (0..39) at BI_BASE + 512 k, BI_CHUNK bytes at most, 7-bit packed as a wave; 8 chunks a
 *                 sector, its first erases the sector. No checksum of its own: the check's CRC-32 covers the image
 *   21            the check, as at power-on: status 0 a whole image is stored, 6 none (erased), 7 one that does not
 *                 check (header, CRC), 8 the flash not read (after 0.99; 0.99 answered 6 for all three); c3 1: at
 *                 power-on XIP read the image's header otherwise than the flash's driver
 *   22 7F         erase (the image's first sector: the name again at the next start) */
#define BI_BASE 0xF7000u                  /* PLAT_XDATA1: 0xF7000..0xFBFFF */
#define BI_SECS 5u
#define BI_CHUNK 512u                     /* a write's bytes (8 a sector) */
#define BI_ROWS 15u                       /* rows a band (the canvas holds 15 rows of 240) */
#define BI_PX (240u * BI_ROWS)
_Static_assert(CV_MAX >= BI_PX && 240u % BI_ROWS == 0u && CV_MAX * 2u >= BI_CHUNK, "the canvas holds a band, a chunk");
_Static_assert(BI_BASE >= UW_BASE0 + 2u * UW_SECS * 4096u && BI_BASE + BI_SECS * 4096u <= PLAT_XDATA1_HI,
               "the image's sectors: above the wave bank's, inside PLAT_XDATA1");

/* The image is read through the flash's driver (plat_data_read: the way the store reads, proven on the FM-1), not in
 * place through XIP (plat_data) as 0.99 did: the FM-1 showed the name there, the emulator the image (the development notes,
 * "On the FM-1"); the XIP window's data reads are the one part of that path never seen on a device. A reader of BI_RD
 * bytes at a time on the stack (no RAM of its own) */
#define BI_RD 64u
enum { BI_OK, BI_NONE = 6, BI_BAD, BI_RERR };   /* the check's status (cmd 21's answer, GLO > ABOUT): stored whole,
                                                  * none (erased), there but broken (header, CRC), not readable */
typedef struct { uint32_t at, i, err, c; uint8_t b[BI_RD]; } bi_rd_t;
static UI_LATE uint32_t bi_byte(bi_rd_t *r)   /* the next byte (into the CRC-32's register) */
{
    uint32_t v, j;
    if (r->i == BI_RD) {
        r->err |= (uint32_t)plat_data_read(r->at, r->b, BI_RD);   /* (BI_BASE + 20 KiB is a whole number of reads) */
        r->at += BI_RD;
        r->i = 0;
    }
    v = r->b[r->i++];
    r->c ^= v;
    for (j = 0; j < 8u; j++) r->c = r->c >> 1 ^ (0xEDB88320u & (0u - (r->c & 1u)));
    return v;
}
static UI_LATE void bi_rd(bi_rd_t *r) { r->at = BI_BASE; r->i = BI_RD; r->err = 0; r->c = ~0u; }
static uint8_t bi_st = BI_NONE;           /* the check at power-on (GLO > ABOUT); | 0x80: XIP reads it otherwise */
static UI_LATE uint32_t bi_ok(uint32_t *h)   /* the stored image whole (BI_OK; h: its header, 4 words, and palette,
                                                  * 32), or why not: the magic, 240 x 240, k, the colours, the length,
                                                  * the CRC-32 over the whole image (16.8 KB read once) */
{
    bi_rd_t r;
    uint32_t i, n;
    bi_rd(&r);
    for (i = 0; i < 16u; i++) ((uint8_t *)h)[i] = (uint8_t)bi_byte(&r);
    if (r.err) return BI_RERR;
    if (h[0] == ~0u) return BI_NONE;
    n = 2u * (h[1] >> 16 & 0xFFu);
    if (h[0] != 0x50534B49u || h[2] != (240u | 240u << 16) || (h[1] >> 8 & 0xFFu) > 6u || n > 128u ||
        h[3] > BI_SECS * 4096u - 20u - n)
        return BI_BAD;
    for (i = 0; i < n + h[3] + 4u; i++) {   /* the palette (kept), the runs, the CRC */
        const uint32_t v = bi_byte(&r);
        if (i < n) ((uint8_t *)(h + 4))[i] = (uint8_t)v;
    }
    return r.err ? BI_RERR : r.c == 0xDEBB20E3u ? BI_OK : BI_BAD;   /* (CRC-32's residue: the data and its CRC) */
}

UI_LATE int app_splash(void)              /* fm1-base at power-on: the stored image drawn (1), else 0 (the name) */
{
    uint32_t h[4 + 32];
    const uint16_t *pal = (const uint16_t *)(const void *)(h + 4);
    const uint32_t *x = (const uint32_t *)(const void *)plat_data(BI_BASE);
    bi_rd_t r;
    uint32_t k, i, y, run = 0, col = 0;
    bi_st = (uint8_t)bi_ok(h);
    if (bi_st != BI_OK) return 0;
    if (!x || *x != h[0]) bi_st = 0x80u;  /* (a diagnosis: XIP reads the window otherwise; the wave bank reads it so) */
    k = h[1] >> 8 & 0xFFu;
    bi_rd(&r);
    for (i = 16u + 2u * (h[1] >> 16 & 0xFFu); i; i--) bi_byte(&r);   /* (the header and the palette passed) */
    for (y = 0; y < 240u; y += BI_ROWS) {
        cv_begin(240, BI_ROWS, 0);        /* (its last band out first) */
        for (i = 0; i < BI_PX; i++) {
            if (!run) {
                const uint32_t v = bi_byte(&r);
                run = (v >> k) + 1u;
                if (run == (255u >> k) + 1u) run += bi_byte(&r);
                col = pal[v & ((1u << k) - 1u)];
            }
            run--;
            cv_px[i] = (uint16_t)col;
        }
        cv_blit(0, y);
    }
    return 1;
}

static UI_LATE void bi_about(char *v)     /* GLO > ABOUT: SPLASH OK (XIP: in place it reads otherwise, the wave bank's
                                                  * way), NONE, BAD (header or CRC: a cut-off upload), ERR (not read) */
{
    const uint32_t s = bi_st & 0x7Fu;
    text_cat(v, s == BI_OK ? "SPLASH OK" : s == BI_NONE ? "SPLASH NONE" : s == BI_BAD ? "SPLASH BAD" : "SPLASH ERR");
    if (bi_st & 0x80u) text_cat(v, " XIP");
}

static UI_LATE void bi_cmd(const uint8_t *a, uint32_t n)   /* waves.c uw_cmd: a its command 20-22 .., n its bytes */
{
    const uint32_t k = n > 1u ? a[1] : 0u, m = n > 2u ? n - 2u : 0u,   /* (m: packed bytes) */
                   len = m - (m + 7u) / 8u, at = BI_BASE + k * BI_CHUNK;
    uint32_t st = UWS_CMD;
    if (a[0] == 0x20u) {                  /* a chunk */
        st = UWS_LEN;
        if (k < 8u * BI_SECS && len && len <= BI_CHUNK && !uw_unpack(a + 2, m, uw_rec, len) &&
            (k % 8u != 7u || len < 0x108u || (uw_rec[0x106] | uw_rec[0x107] << 8) != 0x5441u))   /* (never "AT" at a
                                                  * sector's 0xF06: plat.h, the update loader's record) */
            st = (k % 8u == 0u && plat_data_erase(at) != 0) || plat_data_write(at, uw_rec, len) != 0 ? UWS_FLASH : UWS_OK;
    } else if (a[0] == 0x21u) {           /* the check (as at power-on; GLO > ABOUT shows it after) */
        uint32_t h[4 + 32];
        st = bi_ok(h);
        bi_st = (uint8_t)(st | (bi_st & 0x80u));
    } else if (a[0] == 0x22u && k == 0x7Fu)   /* erase */
        st = plat_data_erase(BI_BASE) != 0 ? UWS_FLASH : UWS_OK;
    uw_ack(a[0], k, st, bi_st >> 7);      /* (the check's: 1 XIP read the image otherwise at power-on) */
}
