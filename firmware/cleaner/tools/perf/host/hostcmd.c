/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 J0KERPACK */
#undef plat_midi_send
static uint32_t host_wr0;                  /* (after 1.00) the store's writes at wrmark (wris) */
static int host_jv(const char *a)          /* (jrn, drpokes) "h-10": JR_N / 2 - 10, "f-20": JR_N - 20 */
{
    return a[0] == 'h' ? (int)JR_N / 2 + atoi(a + 1) : a[0] == 'f' ? (int)JR_N + atoi(a + 1) : atoi(a);
}
static uint16_t host_non[16][128];        /* the notes out (mk.sh's hook): on less off a channel's note (0 at least) */
static uint32_t host_mon[16];              /* (1.19.2) the note-ons out on each channel since "moclr" */
static uint16_t host_mnn[16][128];         /* (1.19.3) each note's note-ons out since "moclr" ("monote") */
static uint8_t host_mnv[16][128];          /* (1.19.3) its last velocity */
static uint32_t host_mot[64], host_motn, host_mt0;   /* (1.19.4) the note-ons' times out since "momark" (ms after it) */
static uint32_t host_mos[4][64], host_mosn[4];       /* ("mosave": kept to compare, "modelta") */
#ifdef HOST_XM
static double host_xnow;                   /* (1.19.5) the block's time, s (app_render's frames counted: "xm") */
static uint32_t host_xnn;                  /* the note-ons' times since "xmmark" (host_xnt) */
static double host_xnt[8192];
static int host_xmk;
#endif
static void host_midi_tap(uint32_t pkt, uint32_t to)
{
    const uint32_t st = pkt >> 8 & 0xFFu, n = pkt >> 16 & 0x7Fu, v = pkt >> 24 & 0x7Fu;
#ifdef HOST_XM
    if (host_xmk && (st & 0xF0u) == 0x90u && v && host_xnn < 8192u) host_xnt[host_xnn++] = host_xnow;
#endif
    if ((st & 0xF0u) == 0x90u && v && host_motn < 64u) host_mot[host_motn++] = plat_ms() - host_mt0;
    if ((st & 0xF0u) == 0x90u && v) { host_mon[st & 15u]++; host_mnn[st & 15u][n]++; host_mnv[st & 15u][n] = (uint8_t)v; }
    if ((st & 0xF0u) == 0x90u && v) host_non[st & 15u][n]++;
    else if (((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u) && host_non[st & 15u][n]) host_non[st & 15u][n]--;
    else if ((st & 0xF0u) == 0xB0u && (n == 123u || n == 120u)) {   /* (all notes off: the channel's) */
        uint32_t k;
        for (k = 0; k < 128u; k++) host_non[st & 15u][k] = 0;
    }
    plat_midi_send(pkt, to);
}
#undef plat_leds
#undef plat_lights
static uint32_t host_lb;                   /* (1.05) the buttons as input() lit them last (mk.sh's hooks; "btnis") */
#ifdef HYB_MULTI
static int ln_tr;
static void host_ln_tap(void)            /* (1.18C15) "lntr 1": a line whenever the row playing or a part's origin changes */
{
    static uint32_t p, o0, o1, o2;
    if (!ln_tr) return;
    if (ln.play != p || sq_line_mt[0] != o0 || sq_line_mt[1] != o1 || dr_org != o2) {
        printf("TR mt %u play %d next %d org %u %u %u len %u sw %u\n", (unsigned)ck.mt, ln.play, ln.next,
               (unsigned)sq_line_mt[0], (unsigned)sq_line_mt[1], (unsigned)dr_org, (unsigned)ln.len, (unsigned)ln.sw);
        p = ln.play; o0 = sq_line_mt[0]; o1 = sq_line_mt[1]; o2 = dr_org;
    }
}
#else
static void host_ln_tap(void) {}
#endif
static uint32_t host_lt[8];               /* (host_lt_str: [6] the keys at the fade level, [7] that level, 1.18C46) */
static void host_leds_tap(uint32_t b, uint32_t k)   /* (plat_leds: the unlit buttons glow, PLAY green unless red) */
{
    host_lb = b;
    host_lt[0] = b; host_lt[1] = k; host_lt[2] = ((1u << NB) - 1u) & ~b; host_lt[3] = 0;
    plat_leds(b, k);
}
#define HOST_NMC 32u
static int32_t host_mc[HOST_NMC];          /* (1.18C40) the metronome's clicks since "mcclr": the clock's tick (signed: a
                                          * PRECOUNT's before 0) */
static uint8_t host_mca[HOST_NMC];         /* (each accented: the bar's first beat) */
static uint32_t host_nmc;
static void host_click(uint32_t m, uint32_t acc)
{
    if (host_nmc < HOST_NMC) { host_mc[host_nmc] = (int32_t)m; host_mca[host_nmc] = (uint8_t)acc; }
    host_nmc++;
}
/* (1.18C43) the lights as they went out, a level per light: the keys F3..G5, the buttons FX .. OCT+ (plat.h's B_*; PLAY
 * either colour): '#' lit, 'm' the middle level (plat_lights_mid; the glow under it or not), 'd' the glow, '.' dark.
 * Each change kept with its time (HOST_LTN of them from the boot: "lttrace" prints them, "ltkmax", "ltorder", "ltis",
 * "ltlast" check them) */
#define HOST_LTN 1024u
                                           /* host_lt: lit buttons, keys; glow buttons, keys; middle buttons, keys */
static struct { uint32_t t; char k[NKEYS + 1], b[NB + 1]; uint8_t lv; } host_lte[HOST_LTN];   /* (lv: the fade's, 1.18C46) */
static uint32_t host_ltn;
static char host_fade_ch(uint32_t lv)    /* (1.18C46) a key at the fade level lv (plat_lights_fade, /256 of lit): '1'..'9',
                                          * then 'A' 10..19 .. 'X' 240..249, 'Y' */
{
    return (char)(lv < 10u ? '0' + lv : 'A' + ((lv - 10u) / 10u > 24u ? 24u : (lv - 10u) / 10u));
}
static void host_lt_str(char *k, char *b)
{
    uint32_t i;
    const uint32_t pg = LED_PLAY_GREEN, pl = 1u << B_PLAY;
    const uint32_t bl = (host_lt[0] & ~pg) | (host_lt[0] & pg ? pl : 0u), bd = (host_lt[2] & ~pg) | (host_lt[2] & pg ? pl : 0u);
    for (i = 0; i < NKEYS; i++) {
        const uint32_t m = 1u << i;
        k[i] = host_lt[1] & m ? '#' : host_lt[5] & m ? 'm' : host_lt[3] & m ? 'd' : host_lt[6] & m ? host_fade_ch(host_lt[7]) : '.';
    }
    for (i = 0; i < NB; i++) {
        const uint32_t m = 1u << i;
        b[i] = bl & m ? '#' : host_lt[4] & m ? 'm' : bd & m ? 'd' : '.';
    }
    k[NKEYS] = b[NB] = 0;
}
static void host_lt_rec(void)
{
    char k[NKEYS + 1], b[NB + 1];
    host_lt_str(k, b);
    if (host_ltn && !strcmp(host_lte[(host_ltn - 1u) % HOST_LTN].k, k) && !strcmp(host_lte[(host_ltn - 1u) % HOST_LTN].b, b)
        && host_lte[(host_ltn - 1u) % HOST_LTN].lv == (host_lt[6] ? host_lt[7] : 0u))
        return;
    host_lte[host_ltn % HOST_LTN].t = plat_ms();
    host_lte[host_ltn % HOST_LTN].lv = (uint8_t)(host_lt[6] ? host_lt[7] : 0u);
    memcpy(host_lte[host_ltn % HOST_LTN].k, k, sizeof k);
    memcpy(host_lte[host_ltn % HOST_LTN].b, b, sizeof b);
    host_ltn++;
}
static void host_lights_tap(uint32_t b, uint32_t k, uint32_t d, uint32_t dk, int g)
{
    host_lb = b;
    host_lt[0] = b; host_lt[1] = k; host_lt[2] = g ? d & ~b : 0u; host_lt[3] = g ? dk & ~k : 0u;
    plat_lights(b, k, d, dk, g);
}
#undef plat_lights_mid
static void host_mid_tap(uint32_t b, uint32_t k)   /* (always after plat_lights or plat_leds: the picture whole) */
{
    host_lt[4] = b & ~host_lt[0];
    host_lt[5] = k & ~host_lt[1];
    plat_lights_mid(b, k);
    host_lt_rec();
}
#undef plat_lights_fade
static uint32_t host_fade_n;               /* (1.18C46) plat_lights_fade's calls ("ltfade": one a change) */
static void host_fade_tap(uint32_t k, uint32_t lv)   /* (before plat_lights: lt_out; host_mid_tap records the picture) */
{
    host_lt[6] = lv ? k : 0u;
    host_lt[7] = lv;
    host_fade_n++;
    plat_lights_fade(k, lv);
}
static uint32_t host_spin;                 /* (1.18C46) "passes N": input() N more times every millisecond, the time not
                                          * moving (the device's main loop passes many times a millisecond) */
void app_host_ms(void)
{
    uint32_t i;
    for (i = 0; i < host_spin; i++) app_input_only();
}
static int host_batt_v = 3;                 /* "batt": plat_battery's level (-1 .. 4); the host's default full */
#undef plat_battery
static int host_batt(void) { return host_batt_v; }
static int32_t host_late_n[NE];             /* (1.18C21) "turnlate": the detents waiting, and the reads to let pass first */
static uint8_t host_late_s[NE];
#undef plat_enc
static int32_t host_enc(int role)
{
    int32_t v = plat_enc(role);
    if (role >= 0 && role < NE && host_late_n[role]) {
        if (host_late_s[role]) host_late_s[role]--;
        else { v += host_late_n[role]; host_late_n[role] = 0; }
    }
    return v;
}
#undef plat_sysex_send
void sy_waves_build(sy_wtab_t *w, int id);   /* (sy_int.h's: "wtabis") */
/* (0.90) the SysEx the app sends (mk.sh's hook): kept for "wsx" / "wsxf", the last frame's bytes */
static uint8_t host_sx_last[1024];
static uint32_t host_sx_n;
static FILE *host_sx_out;
/* (0.98) the remote editor's frames out (remote.c, F0 7D 52 45 cmd): counted by cmd (41..4F), the buffer as an editor
 * would hold it from the dumps (42) and chunks (49), the last state (STAT), the last set's answer (43), the last
 * controllers' frame (4A) */
static uint32_t host_re_n[16];
static uint8_t host_re_mir[SH_LEN], host_re_st[8], host_re_ack[2] = {0xFF, 0xFF}, host_re_cc[256];
static uint32_t host_re_ccn;
static uint8_t host_re_lv[9];             /* (the last 4B: the matrix's slots live) */
static void host_re_unpack(const uint8_t *s, uint8_t *d, uint32_t n)
{
    uint32_t k;
    for (k = 0; k < n; k++) d[k] = (uint8_t)(s[k] | (s[n + k / 7u] >> (k % 7u) & 1u) << 7);
}
static void host_re_tap(const uint8_t *f, uint32_t n)
{
    const uint32_t c = f[4];
    uint32_t k;
    if (c >= 0x40u && c < 0x50u) host_re_n[c - 0x40u]++;
    if (c == 0x41u && n >= 18u) for (k = 0; k < 8u; k++) host_re_st[k] = f[9u + k];
    if ((c == 0x42u || c == 0x45u || c == 0x46u || c == 0x48u) && n >= 14u) for (k = 0; k < 8u; k++) host_re_st[k] = f[5u + k];
    if (c == 0x42u && n >= 16u) {
        const uint32_t m = (uint32_t)f[13] << 7 | f[14];
        if (m == SH_LEN && n == 16u + m + (m + 6u) / 7u) host_re_unpack(f + 15, host_re_mir, m);
        else printf("FAIL re dump: %u bytes for %u\n", n, m);
    }
    if (c == 0x49u && n >= 8u) {
        const uint32_t i = f[6], m = i * 32u + 32u <= SH_LEN ? 32u : SH_LEN - i * 32u;
        if (i * 32u < SH_LEN && n == 8u + m + (m + 6u) / 7u) host_re_unpack(f + 7, host_re_mir + i * 32u, m);
        else printf("FAIL re chunk %u: %u bytes\n", i, n);
    }
    if (c == 0x4Bu && n == 16u) for (k = 0; k < 9u; k++) host_re_lv[k] = f[6u + k];
    if (c == 0x43u && n >= 8u) { host_re_ack[0] = f[5]; host_re_ack[1] = f[6]; }
    if (c == 0x4Au) { host_re_ccn = 0; for (k = 6; k + 2u < n && host_re_ccn < 128u; k += 2u) { host_re_cc[2u * host_re_ccn] = f[k]; host_re_cc[2u * host_re_ccn + 1u] = f[k + 1u]; host_re_ccn++; } }
}
/* (1.05) patches out (patchx.c): the last single dump, extension frame and dump answer (53) the app sent */
static uint8_t host_px_s[267], host_px_x[80], host_px_r[10];
static uint32_t host_px_sn, host_px_xn, host_px_rn;
static uint8_t host_pn[8][400];          /* (the names' frames, 54, since "pxnames" asked) */
static uint32_t host_pnl[8], host_pnn;
static void host_px_tap(const uint8_t *f, uint32_t n)
{
    uint32_t k;
    if (n == 267u && f[1] == 0x00 && f[2] == 0x20 && f[3] == 0x33 && f[6] == 0x10) { for (k = 0; k < n; k++) host_px_s[k] = f[k]; host_px_sn = n; }
    if (n >= 9u && n <= sizeof host_px_x && f[1] == 0x7D && f[2] == 0x49 && f[3] == 0x58) { for (k = 0; k < n; k++) host_px_x[k] = f[k]; host_px_xn = n; }
    if (n == 10u && f[1] == 0x7D && f[2] == 0x42 && f[3] == 0x55 && f[4] == 0x53) { for (k = 0; k < n; k++) host_px_r[k] = f[k]; host_px_rn = n; }
    if (n >= 9u && n <= sizeof host_pn[0] && f[1] == 0x7D && f[2] == 0x42 && f[3] == 0x55 && f[4] == 0x54 && host_pnn < 8u) {
        for (k = 0; k < n; k++) host_pn[host_pnn][k] = f[k];   /* (the patch manager's names, 54: "pxnames") */
        host_pnl[host_pnn++] = n;
    }
}
static void host_sx_tap(const uint8_t *f, uint32_t n)
{
    uint32_t k;
    host_px_tap(f, n);
    if (n >= 6u && f[0] == 0xF0 && f[1] == 0x7D && f[2] == 0x52 && f[3] == 0x45) host_re_tap(f, n);
    if (n <= sizeof host_sx_last) { for (k = 0; k < n; k++) host_sx_last[k] = f[k]; host_sx_n = n; }
    if (host_sx_out) fwrite(f, 1, n, host_sx_out);
    plat_sysex_send(f, n);
}
static void host_sx_in(const uint8_t *f, uint32_t n)   /* a frame F0 .. F7 to the app as the platform hands it */
{
    if (n >= 2u && f[0] == 0xF0 && f[n - 1u] == 0xF7) app_sysex(f + 1, n - 2u);
}
#undef app_render
#ifndef HAT_ON                           /* (an app before 0.98: no sampled hat) */
#define HAT_ON 0u
#define HAT_HAVE 0
#endif
#include <math.h>                        /* (sqrt, log10: "clkis") */
static uint32_t q_mark;                  /* (after 0.98: "qmark", "qsync") */
static uint32_t host_oh = 2166136261u, host_on;   /* the output's hash (FNV-1a of its words) and blocks since "ohreset" */
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
#ifdef HYB_SHIMMER
int hyb_rv_shim_on(void);
int hyb_rv_shim_get(int k);
static int host_sm[2][2] = {{999, -999}, {999, -999}};   /* (0.89) the loop's AMOUNT and MODULATION (TONE before 1.18C45) seen since "shmreset": min, max */
#endif
/* (0.91) clicks: the output's fourth difference (x[i] - 4 x[i-1] + 6 x[i-2] - 4 x[i-3] + x[i-4]) at its largest against
 * the output's rms since "clkreset", per channel ("clkis"). A low tone gives a tiny fourth difference (~(2 pi f / fs)^4
 * of its level: -110 dB at 220 Hz), a jump in the waveform (a delay time stepped) about six times the jump */
static double host_ck_h[2][4], host_ck_max[2], host_ck_e[2];
static uint32_t host_ck_n;
/* (0.92) the L/R balance: 20 log10(rms R / rms L) per window of 2048 frames, its smallest and largest since
 * "balreset" (windows quieter than -80 dBFS on both sides skipped); "balis" */
static double host_bl_e[2], host_bl_min = 999., host_bl_max = -999.;
static double host_rm_e, host_rm_mark = -999.;   /* (0.92) the output's power since "rmsreset" (both sides), a mark */
static double host_sd_e;                 /* (after 0.99) the side's power since "rmsreset": (L - R) / 2 ("sideis") */
static uint32_t host_rm_n;
static uint32_t host_bl_n;
#ifdef HOST_XM
/* (1.19.5) a simulated MIDI clock master ("xm"): ticks sent at its tempo (24 a beat; "xmtempo": a jump or a linear
 * ramp), each arriving at its send time plus a uniform jitter (+- jit, never before the one before) or at the next
 * multiple of a burst period (Bluetooth's connection events), some dropped ("xmdrop"); the ticks due at a block's start
 * go in (USB) before it renders, as the platform's would. The beats' send times are kept: "xmstat" measures the
 * note-ons out (SEQ A's pattern 1, a note a beat) against them */
static struct {
    int on;
    double ts, bpm0, bpm1, rt0, rt1;     /* the next tick's send time; the tempo from bpm0 to bpm1 over rt0 .. rt1 (s) */
    double jit, burst, alast, late;      /* the jitter, the burst period (s); the last arrival; the arrivals' lateness summed */
    uint32_t k, nb, drop, seed, nl;      /* ticks sent, beats kept, ticks to drop, the noise's state, lateness count */
    double t0, beat[16384];              /* xm's start; each beat's send time */
    struct { double t; uint8_t st; } q[256];
    uint32_t qw, qr, iw, ir;             /* the queue in flight; arrived, to plat_midi_in */
    uint8_t in[256];
} hx;
static double host_xf;                   /* the frames rendered */
static double hx_rnd(void) { hx.seed = hx.seed * 1664525u + 1013904223u; return (double)(hx.seed >> 8) / 16777216.; }
static void hx_push(double a, uint8_t st)
{
    if (a < hx.alast) a = hx.alast;
    hx.alast = a;
    if (hx.qw - hx.qr < 256u) { hx.q[hx.qw % 256u].t = a; hx.q[hx.qw++ % 256u].st = st; }
}
static double hx_bpm(double t)
{
    if (t <= hx.rt0) return hx.bpm0;
    if (t >= hx.rt1) return hx.bpm1;
    return hx.bpm0 + (hx.bpm1 - hx.bpm0) * (t - hx.rt0) / (hx.rt1 - hx.rt0);
}
static void hx_block(void)
{
    while (hx.on && hx.ts <= host_xnow + 0.05) {   /* (sent up to 50 ms ahead: the arrivals ordered) */
        double a = hx.ts;
        if (hx.burst > 0.) a = ceil(a / hx.burst - 1e-9) * hx.burst;
        else if (hx.jit > 0.) a += hx.jit * (2. * hx_rnd() - 1.);
        if (hx.k % 24u == 0u && hx.nb < 16384u) hx.beat[hx.nb++] = hx.ts;
        if (hx.drop) hx.drop--;
        else { hx_push(a, 0xF8u); hx.late += hx.alast - hx.ts; hx.nl++; }
        hx.k++;
        hx.ts += 60. / (24. * hx_bpm(hx.ts));
    }
    while (hx.qr != hx.qw && hx.q[hx.qr % 256u].t <= host_xnow + 1e-9) {
        if (hx.iw - hx.ir < 256u) hx.in[hx.iw++ % 256u] = hx.q[hx.qr % 256u].st;
        hx.qr++;
    }
}
#undef plat_midi_in
static int host_midi_in(uint32_t *pkt, int *port)
{
    if (hx.ir != hx.iw) {
        *pkt = 0x0Fu | (uint32_t)hx.in[hx.ir++ % 256u] << 8;
        if (port) *port = PORT_USB;
        return 1;
    }
    return plat_midi_in(pkt, port);
}
#endif
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)   /* (mk.sh: the app's, its output hashed) */
{
    uint32_t k;
#ifdef HOST_XM
    host_xnow = host_xf / (double)S.fs;
    hx_block();
    host_xf += (double)n;
#endif
    app_render_dut(lr, n, master_q12);
    for (k = 0; k < 2u * n; k++) host_oh = (host_oh ^ (uint32_t)lr[k]) * 16777619u;
    host_on++;
    for (k = 0; k < 2u * n; k++) {
        const int c = (int)(k & 1u);
        const double x = (double)lr[k], *h = host_ck_h[c];
        const double d4 = x - 4. * h[0] + 6. * h[1] - 4. * h[2] + h[3];
        if (host_ck_n >= 8u && (d4 < 0. ? -d4 : d4) > host_ck_max[c]) host_ck_max[c] = d4 < 0. ? -d4 : d4;
        host_ck_e[c] += x * x;
        host_ck_h[c][3] = h[2]; host_ck_h[c][2] = h[1]; host_ck_h[c][1] = h[0]; host_ck_h[c][0] = x;
        if (c) host_ck_n++;
        host_bl_e[c] += x * x;
        host_rm_e += x * x;
        host_rm_n++;
        if (c) { const double d = 0.5 * ((double)lr[k - 1u] - x); host_sd_e += 2. * d * d; }   /* (per frame: as rmsis
                                         * counts both sides) */
        if (c && ++host_bl_n == 2048u) {
            const double fs = 8388608. * 8388608. * 2048.;
            if (host_bl_e[0] > fs * 1e-8 || host_bl_e[1] > fs * 1e-8) {
                const double db = 10. * log10((host_bl_e[1] + 1e-9) / (host_bl_e[0] + 1e-9));
                if (db < host_bl_min) host_bl_min = db;
                if (db > host_bl_max) host_bl_max = db;
            }
            host_bl_e[0] = host_bl_e[1] = 0.;
            host_bl_n = 0u;
        }
    }
#ifdef HYB_SHIMMER
    if (hyb_rv_shim_on())
        for (k = 0; k < 2u; k++) {
            const int v = hyb_rv_shim_get((int)k);
            if (v < host_sm[k][0]) host_sm[k][0] = v;
            if (v > host_sm[k][1]) host_sm[k][1] = v;
        }
#endif
}
/* (1.05) patches out (patchx.c). A bank: A, B, C, D, E or "edit" (the edit buffer); a slot
 * 1..128 (as the panel names it, C001):
 *   pxdump BANK SLOT ST [x|-]   the dump asked (F0 7D 42 55 05); a check: answered 53 with status ST; with ST 0 the
 *                               single's checksum and bytes as stored (the edit buffer: as a save would store it), an
 *                               extension frame (x) or none (-) as given, its checksum and its bytes as the patch's
 *   pxcopy BANK SLOT TO [noext|late]   the round trip: the patch dumped, uploaded into user slot TO ("D005": C, D, E
 *                               and the slot) as the page does (armed for one, the single, its extension frame unless noext, the
 *                               end; late: the extension frame after a second single into the next slot); a check:
 *                               the slot's bytes and extension as the source's (noext: the defaults); FAIL else
 *   pxedit BANK SLOT [pp]       the patch dumped, its single and extension frame sent unarmed (the edit buffer); a
 *                               check: the panel's buffer (shadow, SH_LEN) as the source's in the panel's form; pp
 *                               given: the extension frame carries that program instead, answered 4 and not taken */
static uint32_t host_px_bank(const char *a)
{
    return !strcmp(a, "edit") ? 0x7Fu : a[0] == 'A' ? PX_BANK_A : a[0] == 'B' ? PX_BANK_B : (uint32_t)(a[0] - 'C');
}
static int host_px_sum_ok(const uint8_t *f, uint32_t from, uint32_t n)   /* f[n - 2] = (f[from] .. f[n - 3]) & 7F */
{
    uint32_t k, s = 0;
    for (k = from; k + 2u < n; k++) s += f[k];
    return (s & 0x7Fu) == f[n - 2u];
}
static void host_px_resum(uint8_t *f, uint32_t from, uint32_t n)
{
    uint32_t k, s = 0;
    for (k = from; k + 2u < n; k++) s += f[k];
    f[n - 2u] = (uint8_t)(s & 0x7Fu);
}
static void host_px_ask(uint32_t bank, uint32_t slot)
{
    uint8_t q[8] = {0xF0, 0x7D, 0x42, 0x55, 0x05, 0, 0, 0xF7};
    q[5] = (uint8_t)bank; q[6] = (uint8_t)slot;
    host_px_sn = host_px_xn = host_px_rn = 0;
    host_sx_in(q, sizeof q);
}
static void host_px_want(uint32_t bank, uint32_t slot, uint8_t *d, uint8_t *x)   /* the patch's stored bytes and its
                                          * extension (patch_ext's form; the defaults: none) */
{
    uint32_t k;
    const uint8_t *p;
#ifdef HYB_SHIMMER
    const uint8_t *e = SHX_DEF0;
#endif
    if (bank == 0x7Fu) {
        for (k = 0; k < 256u; k++) d[k] = (uint8_t)SHX_EB(k, shadow[k]);
#ifdef HYB_SHIMMER
        for (k = 0; k < SHX_N; k++) x[k] = shadow[256u + k];
        for (k = 0; k < SHX_NR; k++) x[SHX_N + k] = (uint8_t)(shadow[SHX_RB[k]] >= SHX_VD ? shadow[SHX_RB[k]] - SHX_VD + 1u : 0u);
        x[SHX_GX] = shadow[256u + SHX_GL];   /* (1.18C47: W GLIDE last) */
#endif
        return;
    }
    p = bank < UB_BANKS ? ub_bytes(bank * UB_SLOTS + slot) : patch_bytes(bank == PX_BANK_A ? slot : PB_0 + slot);
    for (k = 0; k < 256u; k++) d[k] = p[k];
#ifdef HYB_SHIMMER
    if (bank < UB_BANKS) e = patch_ext(PU_0 + bank * UB_SLOTS + slot);
    for (k = 0; k < SHX_XN; k++) x[k] = e[k];
#endif
}
/* (the patch manager, hello version 4) "pxnames BANK [N]": a bank's names (07): four frames of 32 slots, each slot's
 * flag and name as the store holds them (ub_filled, ub_name); N given: that many filled; a bank not there (X): one
 * frame of status 2. "pxclear BANK SLOT [noarm]": the slot emptied as the page's write does (02 bank 1, 06 slot, 03);
 * its dump then empty (53 status 1), the slots beside it as they were; noarm: 06 alone, answered 52 status 4 and the
 * slot untouched */
static int host_pm_cmd(const char *cmd, const char *a, const char *b, const char *c)
{
    const uint32_t bank = host_px_bank(a);
    uint32_t k, j, bad = 0;
    if (!strcmp(cmd, "pxnames")) {
        uint8_t q[7] = {0xF0, 0x7D, 0x42, 0x55, 0x07, 0, 0xF7};
        uint32_t nf = 0;
        q[5] = (uint8_t)bank;
        host_pnn = 0;
        host_sx_in(q, sizeof q);
        if (bank >= UB_BANKS) {
            if (host_pnn != 1u || host_pnl[0] != 9u || host_pn[0][7] != 2u) bad |= 1;
        } else if (host_pnn != 4u) bad |= 1;
        else
            for (k = 0; k < 4u; k++) {
                const uint8_t *f = host_pn[k];
                if (host_pnl[k] != 9u + 32u * 11u || f[5] != bank || f[6] != k || f[7] != 0u || f[host_pnl[k] - 1u] != 0xF7) bad |= 2;
                for (j = 0; j < 32u && !(bad & 2u); j++) {
                    const uint32_t u = bank * UB_SLOTS + k * 32u + j;
                    const uint8_t *e = f + 8u + j * 11u;
                    char nm[10];
                    if (e[0] != (uint8_t)ub_filled(u)) bad |= 4;
                    if (e[0]) { nf++; ub_name(u, nm); if (memcmp(nm, e + 1, 10u)) bad |= 8; }
                    else for (uint32_t i = 0; i < 10u; i++) if (e[1u + i]) bad |= 16;
                }
            }
        if (b[0] && nf != (uint32_t)atoi(b)) bad |= 32;
        printf("%s pxnames %s: %u frames, %u filled (bad %u)\n", bad ? "FAIL" : "  ok", a, host_pnn, nf, bad);
        return 1;
    }
    {   /* pxclear */
        const uint32_t slot = (uint32_t)atoi(b) - 1u, u = bank * UB_SLOTS + slot, noarm = !strcmp(c, "noarm");
        uint8_t arm[9] = {0xF0, 0x7D, 0x42, 0x55, 0x02, 0, 0, 1, 0xF7}, cl[7] = {0xF0, 0x7D, 0x42, 0x55, 0x06, 0, 0xF7};
        uint8_t end[6] = {0xF0, 0x7D, 0x42, 0x55, 0x03, 0xF7}, nb[2][256];
        const int was = ub_filled(u);
        for (j = 0; j < 2u; j++) {       /* (the slots beside it, as they were) */
            const uint32_t v = j ? u + 1u : u - 1u;
            const uint8_t *p = (v / UB_SLOTS == bank) ? ub_bytes(v) : INIT_PATCH;
            for (k = 0; k < 256u; k++) nb[j][k] = p[k];
        }
        arm[5] = (uint8_t)bank; cl[5] = (uint8_t)slot;
        if (!noarm) host_sx_in(arm, sizeof arm);
        host_sx_n = 0;
        host_sx_in(cl, sizeof cl);
        if (host_sx_n != 9u || host_sx_last[4] != 0x52 || host_sx_last[5] != slot
            || host_sx_last[6] != (noarm ? 4u : 0u) || host_sx_last[7] != (noarm ? 0u : bank + 1u)) bad |= 1;
        if (!noarm) {
            host_sx_n = 0;
            host_sx_in(end, sizeof end);
            if (host_sx_n != 10u || host_sx_last[4] != 0x43 || host_sx_last[8] != 0u) bad |= 2;
        }
        if (ub_filled(u) != (noarm ? was : 0)) bad |= 4;
        host_px_ask(bank, slot);
        if (host_px_rn != 10u || host_px_r[7] != (ub_filled(u) ? 0u : 1u)) bad |= 8;
        for (j = 0; j < 2u; j++) {
            const uint32_t v = j ? u + 1u : u - 1u;
            const uint8_t *p;
            ub_cached = 0;
            p = (v / UB_SLOTS == bank) ? ub_bytes(v) : INIT_PATCH;
            for (k = 0; k < 256u; k++) if (p[k] != nb[j][k]) { bad |= 16; break; }
        }
#ifdef HYB_SHIMMER
        if (!noarm) {
            uint8_t r[SHX_REC];
            shx_rec_read(u, r);
            for (k = 0; k < SHX_REC; k++) if (r[k]) bad |= 32;
        }
#endif
        printf("%s pxclear %s %s%s: was %d, filled %d (bad %u)\n", bad ? "FAIL" : "  ok", a, b, noarm ? " noarm" : "", was,
               ub_filled(u), bad);
        return 1;
    }
}
static int host_px_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d)
{
    const uint32_t bank = host_px_bank(a), slot = (uint32_t)atoi(b) - 1u;
    uint8_t want[256], wx[64], fs[267], fx[80];
    uint32_t k, xn = 0, nx = 0;
    int bad = 0;
#ifdef HYB_SHIMMER
    xn = SHX_XN;                         /* (1.18C47: W GLIDE after the targets) */
#endif
    for (k = 0; k < sizeof wx; k++) wx[k] = 0;
    host_px_ask(bank, slot);
    if (!strcmp(cmd, "pxdump")) {
        const uint32_t st = (uint32_t)atoi(c);
        int has = 0;
        if (host_px_rn != 10u || host_px_r[5] != (bank & 0x7Fu) || host_px_r[6] != (slot & 0x7Fu) || host_px_r[7] != st) bad = 1;
        if (!st) {
            host_px_want(bank, slot, want, wx);
            if (host_px_sn != 267u || !host_px_sum_ok(host_px_s, 5u, 267u)) bad |= 2;
            for (k = 0; k < 256u && host_px_sn == 267u; k++) if (host_px_s[9u + k] != want[k]) { bad |= 4; break; }
            for (k = 0; k < xn; k++) has |= wx[k] != SHX_DEF0[k];
            if (has != (host_px_xn != 0u) || (d[0] == 'x' && !has) || (d[0] == '-' && has)) bad |= 8;
            if (host_px_xn && (host_px_xn != 10u + xn || !host_px_sum_ok(host_px_x, 4u, host_px_xn) || host_px_x[7] != xn
                               || host_px_x[5] != host_px_s[7] || host_px_x[6] != host_px_s[8] || (host_px_r[8] & 1u) == 0u)) bad |= 16;
            for (k = 0; k < xn && host_px_xn; k++) if (host_px_x[8u + k] != wx[k]) bad |= 32;
        } else if (host_px_sn || host_px_xn) bad |= 64;
        printf("%s pxdump %s %s status %d single %u B ext %u B flags %d (bad %d)\n", bad ? "FAIL" : "  ok", a, b,
               host_px_rn ? host_px_r[7] : -1, host_px_sn, host_px_xn, host_px_rn ? host_px_r[8] : -1, bad);
        return 1;
    }
    if (host_px_rn != 10u || host_px_r[7] != 0u || host_px_sn != 267u) {
        printf("FAIL %s %s %s: no dump (status %d)\n", cmd, a, b, host_px_rn ? host_px_r[7] : -1);
        return 1;
    }
    host_px_want(bank, slot, want, wx);
    for (k = 0; k < 267u; k++) fs[k] = host_px_s[k];
    for (k = 0; k < host_px_xn; k++) fx[k] = host_px_x[k];
    nx = host_px_xn;
    if (!strcmp(cmd, "pxcopy")) {
        const uint32_t tb = host_px_bank(c), ts = (uint32_t)atoi(c + 1) - 1u, u = tb * UB_SLOTS + ts;
        const int noext = !strcmp(d, "noext"), late = !strcmp(d, "late");
        uint8_t arm[9] = {0xF0, 0x7D, 0x42, 0x55, 0x02, 0, 0, 1, 0xF7}, end[6] = {0xF0, 0x7D, 0x42, 0x55, 0x03, 0xF7};
        const uint8_t *got, *gx;
        arm[5] = (uint8_t)tb;
        if (late) arm[7] = 2;
        host_sx_in(arm, sizeof arm);
        fs[8] = (uint8_t)ts;
        host_px_resum(fs, 5u, 267u);
        host_sx_n = 0;
        host_sx_in(fs, 267u);
        if (host_sx_n != 9u || host_sx_last[4] != 0x50 || host_sx_last[5] != ts || host_sx_last[6]) bad |= 1;
        if (late) {                      /* (a second single, the init patch, into the next slot first) */
            uint8_t f2[267];
            for (k = 0; k < 267u; k++) f2[k] = fs[k];
            for (k = 0; k < 256u; k++) f2[9u + k] = INIT_PATCH[k];
            f2[8] = (uint8_t)(ts + 1u);
            host_px_resum(f2, 5u, 267u);
            host_sx_in(f2, 267u);
        }
        if (nx && !noext) {
            fx[6] = (uint8_t)ts;
            host_px_resum(fx, 4u, nx);
            host_sx_n = 0;
            host_sx_in(fx, nx);
            if (host_sx_n != 9u || host_sx_last[4] != 0x51 || host_sx_last[5] != ts || host_sx_last[6]) bad |= 2;
        }
        host_sx_in(end, sizeof end);
        if (noext) for (k = 0; k < xn; k++) wx[k] = SHX_DEF0[k];
        ub_cached = 0;
        got = ub_bytes(u);
        for (k = 0; k < 256u; k++) if (got[k] != want[k]) { bad |= 4; break; }
#ifdef HYB_SHIMMER
        shx_cu = 0;
        gx = patch_ext(PU_0 + u);
        for (k = 0; k < xn; k++) if (gx[k] != wx[k]) { bad |= 8; break; }
#else
        (void)gx;
#endif
        printf("%s pxcopy %s %s -> %s %s (ext frame %u B, bad %d)\n", bad ? "FAIL" : "  ok", a, b, c, d, nx, bad);
        return 1;
    }
    /* pxedit */
    host_sx_n = 0;
    host_sx_in(fs, 267u);
    if (host_sx_n != 9u || host_sx_last[4] != 0x50 || host_sx_last[6]) bad |= 1;
    if (nx) {
        if (c[0]) { fx[6] = (uint8_t)atoi(c); host_px_resum(fx, 4u, nx); }
        host_sx_n = 0;
        host_sx_in(fx, nx);
        if (host_sx_n != 9u || host_sx_last[4] != 0x51 || host_sx_last[6] != (c[0] ? 4 : 0)) bad |= 2;
        if (c[0]) for (k = 0; k < xn; k++) wx[k] = SHX_DEF0[k];
    }
    for (k = 0; k < 256u; k++) {
        uint32_t v = want[k];
#ifdef HYB_SHIMMER
        const int r = shx_ri(k);
        if (r >= 0 && wx[SHX_N + (uint32_t)r] >= 1u && wx[SHX_N + (uint32_t)r] <= SHX_NT) v = SHX_VD + wx[SHX_N + (uint32_t)r] - 1u;
#endif
        if (shadow[k] != v) { bad |= 4; printf("  pxedit byte %u: %u, want %u\n", k, shadow[k], v); break; }
    }
#ifdef HYB_SHIMMER
    for (k = 0; k < SHX_N; k++) if (shadow[256u + k] != wx[k]) { bad |= 8; break; }
    if (shadow[256u + SHX_GL] != wx[SHX_GX]) bad |= 8;
    for (k = 0; k < SHX_N + SHX_NR; k++)    /* (the engine's: TAIL's byte with W GLIDE, 1.18C47) */
        if (shx_eng[k] != (k == SHX_TAIL ? shx_tl(shadow + 256u, wx[SHX_GX]) : k < SHX_N ? shadow[256u + k] : wx[k])) { bad |= 16; break; }
#endif
    printf("%s pxedit %s %s %s (ext frame %u B, bad %d)\n", bad ? "FAIL" : "  ok", a, b, c, nx, bad);
    return 1;
}
/* (after 1.08) the parts for the voice checks (vstat, vfree, keyson) */
#ifdef HYB_MULTI
#define HOST_NP 2
#define HOST_PART(p) ((p) ? &SB : &S)
#define HOST_NV(s) ((s)->nv)
#else
#define HOST_NP 1
#define HOST_PART(p) (&S)
#define HOST_NV(s) SY_MAX_VOICES
#endif
#ifdef BK_NLOG                           /* (1.18C30: the banks in the bank store, bankstore.c) */
#define HOST_UB_SIZE(l) (bk_ph[l] ? (int)bk_len[l] : -1)
#define HOST_UB_WRITE(l, off, src, n) (bk_stage((l), (off), (src), (n)) | bk_flush())
#else
#define HOST_UB_SIZE(o) plat_store_size(o)
#define HOST_UB_WRITE(o, off, src, n) plat_store_write((o), (off), (src), (n))
#endif
/* (1.18C30) the update from 1.18C29 (bk_migrate.sh): commands built into both releases' simulators */
static uint32_t host_tag(const char *t)  /* "C005" (A, B the factory banks, C .. the user banks), "INIT": its index */
{
    if (!strcmp(t, "INIT")) return PATCH_INIT;
    return (uint32_t)(t[0] - 'A') * 128u + (uint32_t)atoi(t + 1) - 1u;
}
static uint64_t host_rs = 0x9E3779B97F4A7C15ull;
static uint32_t host_rnd(void) { host_rs ^= host_rs << 13; host_rs ^= host_rs >> 7; host_rs ^= host_rs << 17; return (uint32_t)host_rs; }
static int host_mig_cmd(const char *cmd, const char *a, const char *b, const char *c)
{
    if (!strcmp(cmd, "ubfill")) {        /* ubfill SEED N BANKS: N saves (ub_write, as SAVE) of made-up patches into random
                                          * slots of the first BANKS user banks, about a third with a shimmer extension */
        uint32_t i, k;
        const uint32_t nb = (uint32_t)atoi(c) < UB_BANKS ? (uint32_t)atoi(c) : UB_BANKS;
        int bad = 0;
        host_rs = 0x9E3779B97F4A7C15ull ^ (uint64_t)atoi(a) * 2654435761u;
        for (i = 0; i < (uint32_t)atoi(b); i++) {
            uint8_t sh[SH_LEN];
            const uint32_t u = host_rnd() % (nb * UB_SLOTS);
            for (k = 0; k < 256u; k++) sh[k] = (uint8_t)(host_rnd() & 127u);
            for (k = 0; k < 10u; k++) sh[240u + k] = (uint8_t)('A' + host_rnd() % 26u);
#ifdef HYB_SHIMMER
            for (k = 0; k < SHX_NR; k++) if (sh[SHX_RB[k]] >= SHX_VD) sh[SHX_RB[k]] = 0;
            for (k = 0; k < SHX_N; k++) sh[256u + k] = SHX_DEF[k];
            sh[256u + SHX_GL] = SHX_GDEF;
            if (host_rnd() % 3u == 0u) {
                for (k = 0; k < SHX_N; k++) sh[256u + k] = (uint8_t)(host_rnd() & 127u);
                sh[256u + SHX_TAIL] &= 1u;
                if (host_rnd() % 2u) sh[SHX_RB[host_rnd() % SHX_NR]] = (uint8_t)(SHX_VD + host_rnd() % SHX_NT);
            }
#endif
            bad |= ub_write(u, sh) != 0;
        }
        printf("%s ubfill %s %s %s\n", bad ? "FAIL" : "  ok", a, b, c);
        return 1;
    }
    if (!strcmp(cmd, "ubsum")) {         /* ubsum [BANKS | same]: each user bank's filled slots and a hash of their bytes
                                          * and extensions as the app reads them (the update's check: the same before
                                          * and after); "same": a check, all banks as at the last ubsum (FAIL else) */
        static uint32_t last[UB_BANKS][2];
        uint32_t bk, s, k, bad = 0;
        const int same = !strcmp(a, "same");
        const uint32_t nb = a[0] && !same && (uint32_t)atoi(a) < UB_BANKS ? (uint32_t)atoi(a) : UB_BANKS;
        for (bk = 0; bk < nb; bk++) {
            uint32_t n = 0, h = 2166136261u;
            for (s = 0; s < UB_SLOTS; s++) {
                const uint32_t u = bk * UB_SLOTS + s;
                const uint8_t *p;
                if (!ub_filled(u)) continue;
                n++;
                p = ub_bytes(u);
                for (k = 0; k < 256u; k++) h = (h ^ p[k]) * 16777619u;
#ifdef HYB_SHIMMER
                p = patch_ext(PU_0 + u);
                for (k = 0; k < SHX_N + SHX_NR; k++) h = (h ^ p[k]) * 16777619u;
#endif
                h = (h ^ s) * 16777619u;
            }
            if (same) bad += last[bk][0] != n || last[bk][1] != h;
            else { last[bk][0] = n; last[bk][1] = h; }
            printf("  ubsum bank %u: %u filled, %08x\n", (unsigned)bk, (unsigned)n, (unsigned)h);
        }
        if (same) printf("%s ubsum same (%u banks differ)\n", bad ? "FAIL" : "  ok", (unsigned)bad);
        return 1;
    }
#ifdef HYB_MULTI
    if (!strcmp(cmd, "setsave")) {       /* setsave A B: part A's patch A, part B's B ("C005", "INIT"), saved as SAVE does */
        ed_part = 0;
        set.patch = (uint16_t)host_tag(a);
        patch_o = (uint16_t)host_tag(b);
        printf("%s setsave %s %s\n", mx_save_set() ? "FAIL" : "  ok", a, b);
        return 1;
    }
    if (!strcmp(cmd, "setsum")) {        /* the patches the settings hold, as tags (after a reboot: those loaded) */
        char t1[8], t2[8];
        patch_tag(t1, set.patch); patch_tag(t2, set.mx_patch);
        printf("  setsum patch %s mx_patch %s (part B plays %u, pt_ok %u)\n", t1, t2, (unsigned)patch_o, (unsigned)set.pt_ok);
        return 1;
    }
    if (!strcmp(cmd, "lsput")) {         /* lsput K A B: launcher set K made now (ls_make), its patches A and B ("C005",
                                          * "INIT") with their names, written (ls_put) */
        uint32_t q, k;
        ls_make(&ls_buf);
        for (q = 0; q < 2u; q++) {
            const uint32_t i = host_tag(q ? c : b);
            const uint8_t *p = patch_bytes(i);
            ls_buf.pt[q][0] = (uint8_t)i; ls_buf.pt[q][1] = (uint8_t)(i >> 8);
            for (k = 0; k < 10u; k++) ls_buf.nm[q][k] = (char)p[240u + k];
        }
        printf("%s lsput %s %s %s\n", ls_put((uint32_t)atoi(a)) ? "FAIL" : "  ok", a, b, c);
        return 1;
    }
    if (!strcmp(cmd, "lssum")) {         /* each launcher set: its patches as tags, whether they are as saved (ls_pnum) */
        uint32_t k, q;
        for (k = 0; k < LS_SLOTS; k++) {
            ls_rec_t r;
            if (!ls_read(k, &r)) continue;
            printf("  lssum %u:", (unsigned)k);
            for (q = 0; q < 2u; q++) {
                uint32_t chg = 0, i;
                char t[8];
                i = ls_pnum(&r, q, &chg);
                patch_tag(t, i);
                printf(" %s%s", t, chg ? "(changed)" : "");
            }
            printf("\n");
        }
        return 1;
    }
#endif
    return 0;
}
static unsigned host_hold_n(void) { unsigned n = 0, k; for (k = 0; k < 4u; k++) n += (unsigned)__builtin_popcount(hold_bits[k]); return n; }
#ifdef HYB_MULTI
static uint16_t host_fs_t[2][4];         /* (glmark: the parts' last glides, sy_synth fs_t) */
#endif
int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d)
{
    if (!strcmp(cmd, "sxbyte")) {        /* (P8) the edit buffer's single sent back over SysEx (unarmed, as a USB frame) with
                                          * byte a set to b: it replaces the edit buffer (the take-over the EURO benchmark
                                          * sends at 2.5 s); the check is byteis */
        uint8_t fs[267];
        uint32_t k;
        host_px_ask(0x7Fu, 0u);
        if (host_px_sn != 267u) { printf("FAIL sxbyte: no dump\n"); return 1; }
        for (k = 0; k < 267u; k++) fs[k] = host_px_s[k];
        fs[9u + ((uint32_t)atoi(a) & 255u)] = (uint8_t)atoi(b);
        host_px_resum(fs, 5u, 267u);
        host_sx_in(fs, 267u);
        return 1;
    }
    if (!strcmp(cmd, "pxdump") || !strcmp(cmd, "pxcopy") || !strcmp(cmd, "pxedit")) return host_px_cmd(cmd, a, b, c, d);
    if (!strcmp(cmd, "pxnames") || !strcmp(cmd, "pxclear")) return host_pm_cmd(cmd, a, b, c);
    if (host_mig_cmd(cmd, a, b, c)) return 1;
#ifdef BK_NLOG
    if (!strcmp(cmd, "bkstat")) {        /* (1.18C30) the bank store: the sectors written, free, the writes and erases */
        uint32_t l, n = 0, f;
        for (l = 0; l < BK_NLOG; l++) n += bk_ph[l] != 0;
        bk_free(&f);
        printf("  bkstat sectors %u free %u gen %u commits %u erases %u open %d\n", (unsigned)n, (unsigned)f,
               (unsigned)bk_gen, (unsigned)bk_nw[0], (unsigned)bk_nw[1], (int)bk_ol);
        return 1;
    }
    if (!strcmp(cmd, "bkdump")) {        /* bkdump FILE: the bank store's flash (0xA5000..0xDBFFF) to a file */
        FILE *f = fopen(a, "wb");
        if (!f) { printf("FAIL bkdump %s\n", a); return 1; }
        fwrite(plat_data(BK_LO), 1, BK_HI - BK_LO, f);
        fclose(f);
        printf("  bkdump %s\n", a);
        return 1;
    }
#endif
    if (!strcmp(cmd, "splash")) {        /* (0.99) a check: the boot image drawn (a 1: app_splash, splash.c) or the
                                          * name instead (0); "fbdump" then has the screen */
        const int r = app_splash();
        printf("%s splash %d (want %s)\n", r == atoi(a) ? "  ok" : "FAIL", r, a);
        return 1;
    }
    if (!strcmp(cmd, "bist")) {          /* (after 0.99) a check: GLO > ABOUT's boot image line is "SPLASH " a (b: more) */
        char v[40] = "", w[40] = "SPLASH ";
        bi_about(v);
        strcat(w, a);
        if (b[0]) { strcat(w, " "); strcat(w, b); }
        printf("%s bist \"%s\" (want \"%s\")\n", strcmp(v, w) ? "FAIL" : "  ok", v, w);
        return 1;
    }
    if (!strcmp(cmd, "biatis")) {        /* (0.99) a check: no "AT" at 0xF06 of the image's sectors (plat.h: the update
                                          * loader's record) */
        uint32_t k, bad = 0;
        for (k = 0; k < BI_SECS; k++) bad += plat_data(BI_BASE + k * 0x1000u + 0xF06u)[0] == 'A' &&
                                             plat_data(BI_BASE + k * 0x1000u + 0xF07u)[0] == 'T';
        printf("%s biatis (%u sectors with one)\n", bad ? "FAIL" : "  ok", bad);
        return 1;
    }
    if (!strcmp(cmd, "re")) {            /* (0.98) a remote editor's frame in: a the command (hello, dump, bye, undo,
                                          * part b), its answer kept (host_re_tap) */
        static const char *const N[6] = {"hello", "dump", "set", "bye", "part", "undo"};
        uint8_t f[8] = {0xF0, 0x7D, 0x52, 0x45, 0, 0xF7, 0xF7, 0};
        uint32_t k, n = 6;
        for (k = 0; k < 6u && strcmp(a, N[k]); k++)
            ;
        f[4] = (uint8_t)(k + 1u);
        if (k == 4u) { f[5] = (uint8_t)atoi(b); n = 7; }
        host_sx_in(f, n);
        return 1;
    }
    if (!strcmp(cmd, "reset")) {         /* (0.98) the remote editor sets byte a to b (c: the part, default the one
                                          * edited; d: the sequence number, default 1) */
#ifdef HYB_MULTI
        const uint32_t pt = c[0] ? (uint32_t)atoi(c) : ed_part;
#else
        const uint32_t pt = c[0] ? (uint32_t)atoi(c) : 0u;
#endif
        const uint32_t by = (uint32_t)atoi(a), v = (uint32_t)atoi(b);
        const uint8_t f[12] = {0xF0, 0x7D, 0x52, 0x45, 0x03, (uint8_t)(d[0] ? atoi(d) & 0x7F : 1), (uint8_t)pt,
                               (uint8_t)(by >> 7), (uint8_t)(by & 0x7Fu), (uint8_t)(v >> 7 & 0x7Fu), (uint8_t)(v & 0x7Fu), 0xF7};
        host_sx_in(f, sizeof f);
        if (host_re_ack[1] == 0u && by < SH_LEN) host_re_mir[by] = (uint8_t)v;   /* (the editor's own copy, as the page) */
        return 1;
    }
    if (!strcmp(cmd, "reclr")) { uint32_t k; for (k = 0; k < 16u; k++) host_re_n[k] = 0; host_re_ccn = 0; return 1; }
    if (!strcmp(cmd, "ren")) {           /* a check (0.98): frames of command a (hex, 41..4F) out since reclr: b of them
                                          * ("+": one at least) */
        const uint32_t c0 = (uint32_t)strtol(a, 0, 16) & 15u, n = host_re_n[c0];
        const int bad = b[0] == '+' ? n < 1u : n != (uint32_t)atoi(b);
        printf("%s ren %s %s (%u)\n", bad ? "FAIL" : "  ok", a, b, n);
        return 1;
    }
    if (!strcmp(cmd, "renle")) {         /* a check (0.98): at most b frames of command a (hex) out since reclr, one at least */
        const uint32_t n = host_re_n[(uint32_t)strtol(a, 0, 16) & 15u];
        printf("%s renle %s %s (%u)\n", n < 1u || n > (uint32_t)atoi(b) ? "FAIL" : "  ok", a, b, n);
        return 1;
    }
    if (!strcmp(cmd, "relv")) {          /* a check (0.98): the last 4B's slot a (0..8) at b .. c */
        const uint32_t v = host_re_lv[(uint32_t)atoi(a) % 9u];
        uint32_t i;
        printf("%s relv %s %s..%s (%u; all", v < (uint32_t)atoi(b) || v > (uint32_t)atoi(c) ? "FAIL" : "  ok", a, b, c, v);
        for (i = 0; i < 9u; i++) printf(" %u", host_re_lv[i]);
        printf(")\n");
        return 1;
    }
    if (!strcmp(cmd, "remirror")) {      /* a check (0.98): the editor's copy (the dumps and chunks out) is the panel's
                                          * edit buffer, every byte; FAIL else */
        uint32_t k, bad = 0;
        for (k = 0; k < SH_LEN; k++)
            if (host_re_mir[k] != shadow[k]) { if (!bad) printf("  remirror: byte %u editor %u panel %u\n", k, host_re_mir[k], shadow[k]); bad++; }
        printf("%s remirror (%u bytes differ)\n", bad ? "FAIL" : "  ok", bad);
        return 1;
    }
    if (!strcmp(cmd, "restat")) {        /* a check (0.98): the last state told: part a, its flags b (1 edited, 2 a SysEx
                                          * single, 4 part B on), the tag c ("A001") */
        char tg[5];
        int bad;
        memcpy(tg, host_re_st + 4, 4); tg[4] = 0;
        bad = host_re_st[0] != atoi(a) || host_re_st[1] != atoi(b) || (c[0] && strcmp(tg, c));
        printf("%s restat %s %s %s (part %u flags %u %s)\n", bad ? "FAIL" : "  ok", a, b, c, host_re_st[0], host_re_st[1], tg);
        return 1;
    }
    if (!strcmp(cmd, "reack")) {         /* a check (0.98): the last set answered with status a (0 taken, 1 part, 2 refused) */
        const int bad = host_re_ack[1] != atoi(a);
        printf("%s reack %s (seq %u status %u)\n", bad ? "FAIL" : "  ok", a, host_re_ack[0], host_re_ack[1]);
        host_re_ack[0] = host_re_ack[1] = 0xFF;
        return 1;
    }
    if (!strcmp(cmd, "recc")) {          /* a check (0.98): the last controllers' frame holds byte a at b */
        uint32_t k;
        int bad = 1;
        for (k = 0; k < host_re_ccn; k++)
            if (host_re_cc[2u * k] == atoi(a)) bad = host_re_cc[2u * k + 1u] != atoi(b);
        printf("%s recc %s %s (%u pairs:", bad ? "FAIL" : "  ok", a, b, host_re_ccn);
        for (k = 0; k < host_re_ccn; k++) printf(" %u=%u", host_re_cc[2u * k], host_re_cc[2u * k + 1u]);
        printf(")\n");
        return 1;
    }
    if (!strcmp(cmd, "undois")) {        /* a check (0.98): a undo steps kept */
        printf("%s undois %s (%u)\n", undo_n != (uint32_t)atoi(a) ? "FAIL" : "  ok", a, undo_n);
        return 1;
    }
    if (!strcmp(cmd, "rmsreset")) { host_rm_e = host_sd_e = 0.; host_rm_n = 0u; return 1; }
    if (!strcmp(cmd, "sideis")) {        /* (after 0.99) a check: the side's level since "rmsreset" (dBFS) within a .. b */
        const double db = 10. * log10(host_sd_e / (8388608. * 8388608. * (host_rm_n ? host_rm_n : 1u)) + 1e-30);
        printf("%s sideis %s %s (%.1f dB)\n", db < atof(a) || db > atof(b) ? "FAIL" : "  ok", a, b, db);
        return 1;
    }
    if (!strcmp(cmd, "rmsis") || !strcmp(cmd, "rmsmark")) {   /* (0.92) the output's level since "rmsreset" (dBFS):
                                          * rmsis a b: within a .. b (FAIL else); rmsmark: kept for rmsrel */
        const double db = 10. * log10(host_rm_e / (8388608. * 8388608. * (host_rm_n ? host_rm_n : 1u)) + 1e-30);
        if (!strcmp(cmd, "rmsmark")) { host_rm_mark = db; printf("  rmsmark %.1f dB\n", db); return 1; }
        printf("%s rmsis %s %s (%.1f dB)\n", db < atof(a) || db > atof(b) ? "FAIL" : "  ok", a, b, db);
        return 1;
    }
    if (!strcmp(cmd, "rmsrel")) {        /* (0.92) a check: the level since "rmsreset" less the mark within a .. b dB */
        const double db = 10. * log10(host_rm_e / (8388608. * 8388608. * (host_rm_n ? host_rm_n : 1u)) + 1e-30),
                     d = db - host_rm_mark;
        printf("%s rmsrel %s %s (%.1f dB, mark %.1f, %+.1f)\n", d < atof(a) || d > atof(b) ? "FAIL" : "  ok", a, b, db, host_rm_mark, d);
        return 1;
    }
    if (!strcmp(cmd, "mxlis")) {         /* (0.92) a check: the matrix page's lights (slots 1, 2A .. 6: "1" lit, "0" dark)
                                          * are a, and (b given) slot 1's name b ("_" a space); FAIL else */
        char w[MX_N + 1u], nm[12], n1[12];
        uint32_t i;
        for (i = 0; i < MX_N; i++) w[i] = (char)('0' + mx_slot(i, nm));
        w[MX_N] = 0;
        mx_slot(0, n1);
        for (i = 0; n1[i]; i++) if (n1[i] == ' ') n1[i] = '_';
        printf("%s mxlis %s %s (%s, %s)\n", strcmp(w, a) || (b[0] && strcmp(n1, b)) ? "FAIL" : "  ok", a, b, w, n1);
        return 1;
    }
    if (!strcmp(cmd, "balreset")) { host_bl_min = 999.; host_bl_max = -999.; host_bl_n = 0u; host_bl_e[0] = host_bl_e[1] = 0.; return 1; }
    if (!strcmp(cmd, "balis")) {         /* (0.92) a check: the balance's swing since "balreset" (largest - smallest,
                                          * dB) at least a (b "max": at most a); FAIL else */
        const double sw = host_bl_max - host_bl_min, w = atof(a);
        const int bad = !strcmp(b, "max") ? sw > w : sw < w;
        printf("%s balis %s %s (%.1f .. %.1f dB, swing %.1f)\n", bad ? "FAIL" : "  ok", a, b, host_bl_min, host_bl_max, sw);
        return 1;
    }
    if (!strcmp(cmd, "clkreset")) {      /* (0.91) restart the click measure (above) */
        int ch;
        for (ch = 0; ch < 2; ch++) { host_ck_max[ch] = host_ck_e[ch] = 0.; host_ck_h[ch][0] = host_ck_h[ch][1] = host_ck_h[ch][2] = host_ck_h[ch][3] = 0.; }
        host_ck_n = 0;
        return 1;
    }
    if (!strcmp(cmd, "clkis")) {         /* (0.91) the click measure since "clkreset": the largest fourth difference re
                                          * the rms, dB, per channel, at most a (FAIL above; FAIL on silence) */
        int ch, bad = host_ck_n < 1000u;
        double v[2];
        for (ch = 0; ch < 2; ch++) {
            const double rms = host_ck_n ? sqrt(host_ck_e[ch] / host_ck_n) : 0.;
            v[ch] = rms > 0. ? 20. * log10(host_ck_max[ch] / rms + 1e-12) : 999.;
            if (rms < 1e-6 * 8388608. || v[ch] > atof(a)) bad = 1;
        }
        printf("%s clkis %.1f %.1f dB (at most %s, %u frames)\n", bad ? "FAIL" : "  ok", v[0], v[1], a, host_ck_n);
        return 1;
    }
    if (!strcmp(cmd, "wsx")) {           /* (0.90) one SysEx frame in (hex, F0 .. F7; "_" ends a part: a, b, c, d
                                          * joined); the answer printed */
        static uint8_t f[1024];
        const char *w[4] = {a, b, c, d};
        uint32_t n = 0, k, j;
        for (j = 0; j < 4u; j++)
            for (const char *h = w[j]; h[0] && h[1] && n < sizeof f; h += 2) { unsigned v; sscanf(h, "%2x", &v); f[n++] = (uint8_t)v; }
        host_sx_n = 0;
        host_sx_in(f, n);
        printf("  wsx out");
        for (k = 0; k < host_sx_n && k < 12u; k++) printf(" %02X", host_sx_last[k]);
        printf("%s (%u B)\n", host_sx_n > 12u ? " .." : "", host_sx_n);
        return 1;
    }
    if (!strcmp(cmd, "wsxp")) {          /* (0.95) a single dump in, as the bank upload page's preview sends it (not
                                          * armed: the edit buffer): the init patch named "SX TEST", byte b set to c,
                                          * program a; a check: answered 50 a 0 0 (in, the edit buffer); FAIL else */
        static uint8_t f[267];
        uint32_t k, s = 0;
        const uint8_t h[9] = {0xF0, 0x00, 0x20, 0x33, 0x01, 0x00, 0x10, 0x00, (uint8_t)(atoi(a) & 0x7F)};
        for (k = 0; k < 9u; k++) f[k] = h[k];
        for (k = 0; k < 256u; k++) f[9u + k] = INIT_PATCH[k];
        for (k = 0; k < 10u; k++) f[9u + 240u + k] = (uint8_t)"SX TEST   "[k];
        f[9u + ((uint32_t)atoi(b) & 255u)] = (uint8_t)(atoi(c) & 0x7F);
        for (k = 5; k < 265u; k++) s += f[k];
        f[265] = (uint8_t)(s & 0x7Fu); f[266] = 0xF7;
        host_sx_n = 0;
        host_sx_in(f, sizeof f);
        printf("%s wsxp out", host_sx_n == 9u && host_sx_last[4] == 0x50 && host_sx_last[5] == h[8] && !host_sx_last[6]
                              && !host_sx_last[7] ? "  ok" : "FAIL");
        for (k = 0; k < host_sx_n && k < 12u; k++) printf(" %02X", host_sx_last[k]);
        printf(" (%u B)\n", host_sx_n);
        return 1;
    }
    if (!strcmp(cmd, "wsxf")) {          /* (0.90) the frames of SysEx file a in, one after another, a millisecond of
                                          * the device between (the main loop's, as frames come); the answers into
                                          * file b (OUTDIR's, if not absolute) */
        FILE *in = fopen(a, "rb");
        static uint8_t f[4096];
        uint32_t n = 0, frames = 0;
        int ch;
        char out[600];
        if (!in) { printf("FAIL wsxf: no %s\n", a); return 1; }
        snprintf(out, sizeof out, "%s%s%s", b[0] == '/' ? "" : plat_host_dir, b[0] == '/' ? "" : "/", b);
        host_sx_out = b[0] ? fopen(out, "wb") : 0;
        while ((ch = fgetc(in)) != EOF) {
            if (ch == 0xF0) n = 0;
            if (n < sizeof f) f[n++] = (uint8_t)ch;
            if (ch == 0xF7 && f[0] == 0xF0) { host_sx_in(f, n); frames++; n = 0; }
        }
        fclose(in);
        if (host_sx_out) { fclose(host_sx_out); host_sx_out = 0; }
        printf("  wsxf %u frames in\n", frames);
        return 1;
    }
    if (!strcmp(cmd, "wackis")) {        /* (0.90) a check: the last answer was 50 cmd a, slot b, status c; FAIL else */
        const int ok = host_sx_n == 12u && host_sx_last[4] == 0x50 && host_sx_last[5] == (uint8_t)strtol(a, 0, 16) &&
                       host_sx_last[6] == atoi(b) && host_sx_last[7] == atoi(c);
        printf("%s wackis %s %s %s (%u B, cmd %02X slot %d status %d)\n", ok ? "  ok" : "FAIL", a, b, c, host_sx_n,
               host_sx_n > 7u ? host_sx_last[5] : 0, host_sx_n > 7u ? host_sx_last[6] : -1, host_sx_n > 7u ? host_sx_last[7] : -1);
        return 1;
    }
    if (!strcmp(cmd, "wuseris")) {       /* (0.90) a check: slot a holds a user wave (b 1) or plays the factory one (0) */
        const int u = uw_get(atoi(a)) != 0;
        printf("%s wuseris %s %s (%d; region %d gen %u next %d)\n", u == atoi(b) ? "  ok" : "FAIL", a, b, u, uw.reg, uw.gen, uw.next);
        return 1;
    }
    if (!strcmp(cmd, "wdone")) {         /* (0.90) a check: the audio interrupt built every table a write marked */
        uint32_t k, left = 0;
        for (k = 0; k < UW_SLOTS; k++) left += ((uw_new[k >> 5] | uw_mark[k >> 5]) >> (k & 31u) & 1u) != 0;
        printf("%s wdone (seq %u seen %u, slots left %u)\n", uw_seq == uw_seen && !left ? "  ok" : "FAIL", uw_seq, uw_seen, left);
        return 1;
    }
    if (!strcmp(cmd, "wwave")) {         /* (0.90) oscillator 1's wave of the panel's patch to a (0..63), as an edit */
        const uint32_t v = (uint32_t)atoi(a) & 127u;
        shadow[19] = (uint8_t)v;
        em_set(set.patch, 19u, v);
        ev_put(EV_PARAM, 19u, v);
        return 1;
    }
    if (!strcmp(cmd, "wtabis")) {        /* (0.90) a check: oscillator a's table (0..2) holds wave b as the bank has it
                                          * now (c "user": a user wave; "factory": the factory one, the bank aside); d
                                          * "B": part B's (two parts) */
        static sy_wtab_t t;
        const int o = atoi(a) % 3, id = atoi(b), fac = !strcmp(c, "factory");
        const uint8_t *(*keep)(int) = sy_user_wave;
        int same, isuser = uw_get(id) != 0;
        sy_synth *e = &S;
        if (d[0] == 'B') {               /* (d "B": part B's engine; a build with one part: not checked) */
#ifdef HYB_MULTI
            e = &SB;
#else
            printf("  ok wtabis %s %s %s B (one part: not checked)\n", a, b, c);
            return 1;
#endif
        }
        if (fac) sy_user_wave = 0;
        sy_waves_build(&t, id);
        sy_user_wave = keep;
        same = e->wt[o].id == id && !memcmp(t.tab, e->wt[o].tab, sizeof t.tab);
        printf("%s wtabis %s %s %s %s (id %d, same %d, the slot's user wave %d, tab[0][0..3] %.4f %.4f %.4f %.4f)\n",
               same && (fac ? !isuser : isuser) ? "  ok" : "FAIL", a, b, c, d, e->wt[o].id, same, isuser, e->wt[o].tab[0][0],
               e->wt[o].tab[0][64], e->wt[o].tab[0][128], e->wt[o].tab[0][192]);
        return 1;
    }
    if (!strcmp(cmd, "ohreset")) { host_oh = 2166136261u; host_on = 0; return 1; }
#ifdef HYB_RV_LEND
    if (!strcmp(cmd, "bleoff")) {        /* (1.04) GLO > BLUETOOTH to a (1 OFF, 0 ON) as its line sets it, GLO's settings
                                          * saved: the next reboot lends Bluetooth's memory (OFF) or not */
        if ((uint32_t)(atoi(a) != 0) != set.ble_off) ble_toggle();
        printf("  ok bleoff %u (saved %d)\n", set.ble_off, glo_save());
        return 1;
    }
    if (!strcmp(cmd, "lentis")) {        /* a check (1.04): Bluetooth's memory lent (a 1) or not (0); FAIL else */
        printf("%s lentis %s (%u)\n", (uint32_t)atoi(a) != bt_lent ? "FAIL" : "  ok", a, bt_lent);
        return 1;
    }
#endif
    if (!strcmp(cmd, "shimhas")) {       /* (0.89) this build has the shimmer (run.sh skips "# requires: shimmer" else;
                                          * 0.98: "hat" / "nohat", the sampled hat's table) */
#ifdef HYB_SHIMMER
        printf("SHIMMER %s\n", SHX_LIVE ? "yes" : "no");   /* (1.04: the Bluetooth two-part build's with its memory lent) */
#else
        printf("SHIMMER no\n");
#endif
#ifdef HYB_SHIM_LEND
        if (!bt_lent) printf("SHIMBT yes\n");   /* (1.04: that build with Bluetooth on: the shimmer locked) */
#endif
        printf("NOFACT yes\n");          /* (the cleaner without factory banks: tests/run.sh runs the "nofactory" scripts) */
#ifdef HYB_MULTI
        printf("MULTI yes\n");
#endif
        printf("HAT %s\n", HAT_HAVE ? "yes" : "no");   /* (0.98: the sampled hat's table built in) */
#ifdef HYB_RAND
        printf("RAND yes\n");           /* (after 0.98: PRESET's RANDOMIZE) */
#endif
        return 1;
    }
#ifdef HYB_SHIMMER
    if (!strcmp(cmd, "shxis")) {         /* a check (0.89): the edited part's extension (shadow 256..): AMOUNT a,
                                          * LAYERS b, GRAIN c, TONE d ("-": not checked); FAIL else (the line shows
                                          * MODULATION, 1.18C45, and DIRECT after the ";") */
        const char *w[4] = {a, b, c, d};
        int k, bad = 0;
        for (k = 0; k < 4; k++) if (w[k][0] && w[k][0] != '-' && shadow[256 + k] != atoi(w[k])) bad = 1;
        printf("%s shxis %s %s %s %s (%d %d %d %d; %d %d)\n", bad ? "FAIL" : "  ok", a, b, c, d, shadow[256], shadow[257],
               shadow[258], shadow[259], shadow[260], shadow[261]);
        return 1;
    }
    if (!strcmp(cmd, "shxset")) {        /* the edited part's extension value a (0..5) to b, as an edit (em, the
                                          * engine) */
        const uint32_t by = 256u + ((uint32_t)atoi(a) % SHX_N);
        shadow[by] = (uint8_t)atoi(b);
        em_set(set.patch, by, shadow[by]);
        shx_sync();
        return 1;
    }
    if (!strcmp(cmd, "shimis")) {        /* a check (0.89): the engine's shimmer running (a 1) or not (0); b (given, not
                                          * "-"): the loop's AMOUNT now; c (given): the AMOUNT the app gave it (part
                                          * A's); FAIL else */
        const int on = hyb_rv_shim_on(), ma = hyb_rv_shim_get(0);
        const int bad = on != atoi(a) || (b[0] && b[0] != '-' && ma != atoi(b)) || (c[0] && hyb_rv_shim_get(2) != atoi(c));
        printf("%s shimis %s %s (on %d, amount %d mod %d, controls %d %d, gain %d)\n", bad ? "FAIL" : "  ok", a, b, on, ma,
               hyb_rv_shim_get(1), hyb_rv_shim_get(2), hyb_rv_shim_get(3), hyb_rv_shim_get(4));
        return 1;
    }
    if (!strcmp(cmd, "rtset")) {         /* destination byte a to the shimmer's target b (0: the patch's value c), as
                                          * the list does (an edit; the engine's byte off, part A's routes) */
        const uint32_t by = (uint32_t)atoi(a) & 255u, v = atoi(b) ? SHX_VD + (uint32_t)atoi(b) - 1u : (uint32_t)atoi(c);
        shadow[by] = (uint8_t)v;
        em_set(set.patch, by, v);
        ev_put(EV_PARAM, by, shx_eb(by, v));
        shx_sync();
        return 1;
    }
    if (!strcmp(cmd, "rvlineis")) {      /* a check (0.89): GLO > AUDIO > REVERB's line reads a ("_" a space; 1.18C42:
                                          * "FULL" and "HALF" not followed by a digit stand for the build's names now,
                                          * RVQ_FNAME, RVQ_HNAME: FULL16, FULL32, HALF32); FAIL else */
        char v[24] = "", w[48] = "";
        uint32_t k, j = 0;
        for (k = 0; a[k] && j < 40u; k++) {
            const char *nm = !strncmp(a + k, "FULL", 4) && !(a[k + 4] >= '0' && a[k + 4] <= '9') ? RVQ_FNAME
                           : !strncmp(a + k, "HALF", 4) && !(a[k + 4] >= '0' && a[k + 4] <= '9') ? RVQ_HNAME : 0;
            if (nm) { strcpy(w + j, nm); j += (uint32_t)strlen(nm); k += 3u; }
            else w[j++] = a[k] == '_' ? ' ' : a[k];
        }
        w[j] = 0;
        glo_val(G_RVQ, v);
        printf("%s rvlineis %s (%s)\n", strcmp(v, w) ? "FAIL" : "  ok", a, v);
        return 1;
    }
    if (!strcmp(cmd, "shmreset")) { host_sm[0][0] = host_sm[1][0] = 999; host_sm[0][1] = host_sm[1][1] = -999; return 1; }
    if (!strcmp(cmd, "shmrange")) {      /* a check (0.89): the loop's AMOUNT (a 0) or MODULATION (1; 1.18C45, TONE before) since shmreset went at
                                          * least as low as b and as high as c; FAIL else */
        const int k = atoi(a) & 1, bad = host_sm[k][0] > atoi(b) || host_sm[k][1] < atoi(c);
        printf("%s shmrange %s %s %s (seen %d .. %d)\n", bad ? "FAIL" : "  ok", a, b, c, host_sm[k][0], host_sm[k][1]);
        return 1;
    }
    if (!strcmp(cmd, "shmodis")) {       /* a check (1.18C45): the loop's MODULATION a (0..127, "-" not checked), its depth
                                          * now (cents) within b .. c, LFO 3 the modulator (d 1) or the reverb's LFO (0;
                                          * given); FAIL else */
        extern uint8_t hyb_rv_shl3;
        const int mo = hyb_rv_shim_get(1), dc = hyb_rv_shim_get(7);
        const int bad = (a[0] && a[0] != '-' && mo != atoi(a)) || dc < (int)(atof(b) * 100.) || dc > (int)(atof(c) * 100.)
                        || (d[0] && hyb_rv_shl3 != atoi(d));
        printf("%s shmodis %s %s %s %s (MODULATION %d, depth %d.%02d c, LFO 3 %d)\n", bad ? "FAIL" : "  ok", a, b, c, d, mo,
               dc / 100, dc % 100, hyb_rv_shl3);
        return 1;
    }
    if (!strcmp(cmd, "rtis")) {          /* a check (0.89): patch byte a (a destination) shows the shimmer's target b
                                          * (0 none, 1 SHIM AMT, 2 SHIM MOD), its engine byte is c, part A's engine
                                          * route d (given); FAIL else */
        const uint32_t by = (uint32_t)atoi(a) & 255u, t = shadow[by] >= SHX_VD ? shadow[by] - SHX_VD + 1u : 0u;
        const int ri = shx_ri(by);
        const int bad = t != (uint32_t)atoi(b) || (c[0] && S.patch[by] != atoi(c)) || ri < 0
                        || (d[0] && S.shx_rt[ri] != atoi(d));
        printf("%s rtis %s %s %s %s (panel %d, engine %d, route %d)\n", bad ? "FAIL" : "  ok", a, b, c, d, shadow[by],
               S.patch[by], ri >= 0 ? S.shx_rt[ri] : -1);
        return 1;
    }
    if (!strcmp(cmd, "shxrecis")) {      /* a check (0.89): user slot a's extension record in the store: none (b 0) or
                                          * one (b 1); FAIL else */
        uint8_t r[SHX_REC];
        uint32_t k, n = 0;
        shx_rec_read((uint32_t)atoi(a), r);
        for (k = 0; k < SHX_REC; k++) n |= r[k];
        printf("%s shxrecis %s %s (tag %02x n %02x, %02x %02x, %u B of object %u: %d)\n", (n != 0u) != (atoi(b) != 0) ? "FAIL" : "  ok",
               a, b, r[0], r[1], r[6], r[7], (unsigned)SHX_REC, (unsigned)shx_obj((uint32_t)atoi(a)), HOST_UB_SIZE(shx_obj((uint32_t)atoi(a))));
        return 1;
    }
    if (!strcmp(cmd, "shx2is")) {        /* a check (0.91): the edited part's TAIL a (0 LIMITED, 1 FREE) and SWELL b on
                                          * the panel; c, d (given): the engine's as its reverb took them; FAIL else */
        const int t = shadow[256u + SHX_TAIL], w = shadow[256u + SHX_SWELL], et = hyb_rv_shim_get(5), ew = hyb_rv_shim_get(6);
        const int bad = t != atoi(a) || w != atoi(b) || (c[0] && et != atoi(c)) || (d[0] && ew != atoi(d));
        printf("%s shx2is %s %s %s %s (panel %d %d, engine %d %d)\n", bad ? "FAIL" : "  ok", a, b, c, d, t, w, et, ew);
        return 1;
    }
    if (!strcmp(cmd, "shwis")) {         /* a check (1.18C47): the edited part's WIDTH a (the extension's value 5) and W
                                          * GLIDE b (0..15) on the panel; c given: part A's to the engine as well (WIDTH
                                          * in its byte, W GLIDE ^ 5 in bits 1-4 of TAIL's); FAIL else */
        const int w = shadow[256u + 5u], g = shadow[256u + SHX_GL], ew = shx_eng[5], eg = (shx_eng[SHX_TAIL] >> 1 & 15) ^ (int)SHX_GDEF;
        const int bad = w != atoi(a) || g != atoi(b) || (c[0] && (ew != atoi(a) || eg != atoi(b) || (shx_eng[SHX_TAIL] & 1) != shadow[256u + SHX_TAIL]));
        printf("%s shwis %s %s %s (panel %d %d, engine %d %d)\n", bad ? "FAIL" : "  ok", a, b, c, w, g, ew, eg);
        return 1;
    }
    if (!strcmp(cmd, "rec46")) {         /* (1.18C47) user slot a's record as 1.18C46 wrote it (no W GLIDE: bit 7 of bytes
                                          * 2-5 clear) */
        uint8_t r[SHX_REC];
        const uint32_t u = (uint32_t)atoi(a);
        uint32_t k;
        shx_rec_read(u, r);
        if (r[0] == SHX_TAG) {
            for (k = 2; k < 6u; k++) r[k] &= 127u;
            HOST_UB_WRITE(shx_obj(u), shx_off(u), r, SHX_REC);
        }
        shx_cu = 0;
        return 1;
    }
    if (!strcmp(cmd, "pxshort")) {       /* (1.18C47) the edit buffer dumped (its single, its extension frame of 21
                                          * bytes), then sent back as a page of before sends it: the frame cut to 20 (no W
                                          * GLIDE); a check: the values and targets as sent, W GLIDE its default; FAIL else */
        uint8_t fs[267], fx[80], v[SHX_XN];
        uint32_t k, n;
        int bad = 0;
        host_px_ask(0x7Fu, 0);
        if (host_px_sn != 267u || host_px_xn != 10u + SHX_XN) { printf("FAIL pxshort: dump %u B, frame %u B\n", host_px_sn, host_px_xn); return 1; }
        for (k = 0; k < 267u; k++) fs[k] = host_px_s[k];
        for (k = 0; k < host_px_xn; k++) fx[k] = host_px_x[k];
        for (k = 0; k < SHX_XN; k++) v[k] = fx[8u + k];
        n = SHX_XN - 1u;
        fx[7] = (uint8_t)n; fx[9u + n] = 0xF7;
        host_px_resum(fx, 4u, 10u + n);
        host_sx_n = 0;
        host_sx_in(fs, 267u);
        host_sx_in(fx, 10u + n);
        if (host_sx_last[4] != 0x51 || host_sx_last[6]) bad |= 1;
        for (k = 0; k < SHX_N; k++) if (shadow[256u + k] != v[k]) bad |= 2;
        if (shadow[256u + SHX_GL] != SHX_GDEF) bad |= 4;
        printf("%s pxshort (sent W GLIDE %u in 21 B; back from 20 B: %u, bad %d)\n", bad ? "FAIL" : "  ok", v[SHX_GX], shadow[256u + SHX_GL], bad);
        return 1;
    }
    if (!strcmp(cmd, "rec90")) {         /* (0.91) user slot a's record as 0.89 / 0.90 wrote it (the same values, no
                                          * TAIL or SWELL: byte 1 the count 6, bit 7 of bytes 6, 7 clear) */
        uint8_t r[SHX_REC];
        const uint32_t u = (uint32_t)atoi(a);
        shx_rec_read(u, r);
        if (r[0] == SHX_TAG) {
            r[1] = 6u; r[6] &= 127u; r[7] &= 127u;
            HOST_UB_WRITE(shx_obj(u), shx_off(u), r, SHX_REC);
        }
        shx_cu = 0;
        return 1;
    }
    if (!strcmp(cmd, "oldslot")) {       /* (0.89) user slot a written as 0.88 wrote it: the panel's patch (its own
                                          * bytes, every target off), no extension; the slot's record cleared first */
        uint8_t q[256], z[SHX_REC];
        const uint32_t u = (uint32_t)atoi(a);
        uint32_t k;
        for (k = 0; k < 256u; k++) q[k] = (uint8_t)shx_eb(k, shadow[k]);
        memset(z, 0, sizeof z);
        if (HOST_UB_SIZE(shx_obj(u)) >= (int)(shx_off(u) + SHX_REC)) HOST_UB_WRITE(shx_obj(u), shx_off(u), z, SHX_REC);
        HOST_UB_WRITE(ub_obj(u), ub_off(u), q, 256u);
        ub_scan(); shx_cu = 0;
        return 1;
    }
#endif
    if (!strcmp(cmd, "ohis") || !strcmp(cmd, "ohiso")) {   /* a check (0.88): the output's hash since ohreset is a
                                          * (the single build), b (the USB build, two parts), c (the Bluetooth build
                                          * with two parts); "-": not checked; FAIL else. The hashes: a release
                                          * before's (drum_send.txt). "ohis" (an earlier reverb's hashes) is
                                          * skipped; "ohiso" checks the own reverb's */
        if (cmd[4] != 'o') { printf("  ok ohis skipped (the own reverb: ohiso)\n"); return 1; }
#ifdef HYB_MULTI
        const char *w = FM1B_BLE ? c : b;
#ifdef HYB_RV_LEND
        extern int hyb_rv_is_full(void);
        if (bt_lent && hyb_rv_is_full()) w = b;   /* (1.04: the Bluetooth two-part build with its memory lent, REVERB
                                          * FULL: the USB build's float ring, its output to the bit; mk.sh mble-lent) */
#endif
#else
        const char *w = a;
#endif
        char h[12];
        snprintf(h, sizeof h, "%08x", host_oh);
        printf("%s ohis %s (%s, %u blocks)\n", w[0] != '-' && strcmp(w, h) ? "FAIL" : "  ok", w, h, host_on);
        return 1;
    }
    if (!strcmp(cmd, "kitsnd")) {        /* (0.88) instrument a's SEND to b (as stored in the kit, then dr_sends) */
        drk.k[atoi(a)].snd = (uint8_t)atoi(b);
        dr_sends();
        return 1;
    }
    if (!strcmp(cmd, "kitis")) {         /* a check (0.88): instrument a's (0 BD ..) SEND b and MODEL c in the kit; the
                                          * engine's drum send (S.dr_w) d ("0": none, "1": some); FAIL else */
        const dr_kp_t *q = &drk.k[atoi(a)];
        const int bad = q->snd != atoi(b) || q->m909 != atoi(c) || (d[0] && (S.dr_w > 0.f) != (atoi(d) != 0));
        printf("%s kitis %s send %d model %d dr_w %.3f\n", bad ? "FAIL" : "  ok", a, q->snd, q->m909, (double)S.dr_w);
        return 1;
    }
    if (!strcmp(cmd, "oldkit")) {        /* (0.88) a kit as 0.87 wrote it into the store: SD the 909 voice (MODEL's byte
                                          * 1), its level 90; BD's PAN L20; the bus WARM / OFF */
        uint8_t q[80];
        uint32_t i;
        memset(q, 0, sizeof q);
        q[0] = DR_KMARK; q[1] = DR_KV1;
        for (i = 0; i < DR_NI; i++) {
            uint8_t *k = q + 2u + 7u * i;
            k[0] = DR_KLEV; k[1] = DR_K0; k[2] = DR_K0; k[3] = 0; k[4] = DR_K0; k[5] = 0; k[6] = 0;
        }
        q[2 + 7 * 1 + 5] = 1; q[2 + 7 * 1 + 0] = 90;
        q[2 + 7 * 0 + 6] = (uint8_t)-20;
        printf("  oldkit %d\n", store_write(OBJ_DRUM, DR_KIT_OFF, q, sizeof q));
        return 1;
    }
    if (!strcmp(cmd, "dsis")) {          /* a check (0.88): the drums' bus into the synth's effects: sending this block
                                          * (S.dr_on) a; part A's effects at rest b ("-": not checked); FAIL else */
        const int bad = S.dr_on != atoi(a) || (b[0] && b[0] != '-' && S.fx_rest != atoi(b));
        printf("%s dsis %s %s (on %d rest %d w %.3f mode %d)\n", bad ? "FAIL" : "  ok", a, b, S.dr_on, S.fx_rest,
               (double)S.dr_w, S.patch[112]);
        return 1;
    }
    (void)c; (void)d;
    if (!strcmp(cmd, "sqd")) {           /* the shown arp pattern's steps on: step ev off len vel; RECORD's state */
        uint32_t k;
        printf("  sqd arec %d metro %d mute %d/%d run %d lc %d div %d dirty %d base %d undo n %d |", sqv.arec, sq_metro,
               sq_mute[0], sq_mute[1], sq.run, sqp.lc, sqp.div, un.n != 0, SQ_BASEV, un.n);
        for (k = 0; k < 64u; k++)
            if (sqp.st[k].ev & SQ_ON)
                printf(" %u:%s%+d/L%u/A%d/V%u", (unsigned)k + 1u, (sqp.st[k].ev & 15u) == SQ_NOTE ? "N" : "o", sqp.st[k].off,
                       sqp.st[k].la & 31u, (int)SQ_AL(&sqp.st[k]), sqp.st[k].vel);
        printf("\n");
        return 1;
    }
    if (!strcmp(cmd, "sqis")) {          /* a check: the shown arp pattern's steps 1-16 on are a ("1...1..."); FAIL else */
        char on[17];
        uint32_t k;
        for (k = 0; k < 16u; k++) on[k] = sqp.st[k].ev & SQ_ON ? '1' : '.';
        on[16] = 0;
        printf("%s sqis %s%s%s\n", strcmp(on, a) ? "FAIL" : "  ok", a, strcmp(on, a) ? ", got " : "", strcmp(on, a) ? on : "");
        return 1;
    }
    if (!strcmp(cmd, "keylvis")) {       /* a check (0.83): the sequencers' key lights as input sets them: the white keys
                                          * (steps 1-16) a, the black keys (the instruments) b; "#" lit, "m" the middle
                                          * level, "d" the glow, "." off ("-" for b: not checked); FAIL else */
        char w[17], bk[12];
        uint32_t lb = 0, dk, mk, lk, i;
        lk = sq_lights(KEYS_DARK ? 0u : ~0u, &lb, &dk, &mk);
        if (ui.view == V_KIT) { lk = kit_lights(&mk); dk = 0; }
        for (i = 0; i < 16u; i++) {
            const uint32_t m = 1u << SQ_KEY[i];
            w[i] = lk & m ? '#' : mk & m ? 'm' : dk & m ? 'd' : '.';
        }
        for (i = 0; i < 11u; i++) {
            const uint32_t m = 1u << DR_KEY[i];
            bk[i] = lk & m ? '#' : mk & m ? 'm' : dk & m ? 'd' : '.';
        }
        w[16] = bk[11] = 0;
        {
            const int bad = strcmp(w, a) || (b[0] && b[0] != '-' && strcmp(bk, b));
            printf("%s keylvis %s %s (%s %s)\n", bad ? "FAIL" : "  ok", a, b, w, bk);
        }
        return 1;
    }
    if (!strcmp(cmd, "partedit")) {      /* the part edited to a (0 A, 1 B), as SHIFT + A#4 / F#5 (two parts; else none) */
#ifdef HYB_MULTI
        part_edit((uint32_t)atoi(a));
#endif
        return 1;
    }
    if (!strcmp(cmd, "thmis")) {         /* a check (0.83): the screen's colours are theme a (0 part A red, 1 part B
                                          * green, 2 the drums' amber) and the part edited b (the builds with two
                                          * parts); c (if given): the theme the Bluetooth build has instead; FAIL else */
        const int th = (int)(u_th - U_THEMES);
#ifdef HYB_MULTI
        const int bad = th != atoi(a) || (b[0] && ed_part != atoi(b)), ep = ed_part;
#else
        const int bad = th != atoi(c[0] ? c : a), ep = 0;
#endif
        printf("%s thmis %s %s %s (theme %d part %d view %d)\n", bad ? "FAIL" : "  ok", a, b, c, th, ep, ui.view);
        return 1;
    }
    if (!strcmp(cmd, "noteout")) {       /* a check (0.83): note a (MIDI number) on out over MIDI now (b 1) or not (b 0),
                                          * any channel; FAIL else */
        uint32_t ch, on = 0;
        for (ch = 0; ch < 16u; ch++) on |= host_non[ch][atoi(a) & 127];
        printf("%s noteout %s %u\n", (on != 0) != (atoi(b) != 0) ? "FAIL" : "  ok", a, (unsigned)on);
        return 1;
    }
    if (!strcmp(cmd, "svis")) {          /* a check (1.00): SAVE's slot list (V_SAVE) with its cursor on user slot a (0 ..
                                          * UB_N - 1: C001 0, D001 128), the octave b (if given); FAIL else */
        const int bad = ui.view != V_SAVE || (int)ui.scur != atoi(a) || (b[0] && set.oct != atoi(b));
        printf("%s svis %s %s (view %d slot %d octave %d)\n", bad ? "FAIL" : "  ok", a, b, ui.view, ui.scur, set.oct);
        return 1;
    }
    if (!strcmp(cmd, "holdis")) {        /* a check (1.00): HOLD on (a 1) or off (0) for the part edited, b (if given)
                                          * the other part's (the builds with two parts; else ignored); FAIL else */
#ifdef HYB_MULTI
        const int o = hold_o;
#else
        const int o = b[0] ? atoi(b) : 0;
#endif
        const int bad = hold_on != atoi(a) || (b[0] && o != atoi(b));
        printf("%s holdis %s %s (%d %d)\n", bad ? "FAIL" : "  ok", a, b, hold_on, o);
        return 1;
    }
    if (!strcmp(cmd, "viewis")) {        /* a check: the view is a (V_SEQ 10); FAIL else */
        printf("%s viewis %d (%d)\n", (int)ui.view != atoi(a) ? "FAIL" : "  ok", atoi(a), ui.view);
        return 1;
    }
    if (!strcmp(cmd, "pageis")) {        /* a check (0.77): the view a, the page's block of 16 steps shown b, the arp
                                          * sequencer shown c (0 A, 1 B; the USB build, else ignored); d (if given):
                                          * the view the Bluetooth build has instead (its block not checked); FAIL else */
        const int bl = ui.view == V_DRUM ? sqv.blk[1] : sqv.blk[0];
#ifdef HYB_MULTI
        const int bad = (int)ui.view != atoi(a) || bl != atoi(b) || sq_mi != atoi(c), mi = sq_mi;
#else
        const int bad = d && d[0] ? (int)ui.view != atoi(d) : (int)ui.view != atoi(a) || bl != atoi(b), mi = 0;
#endif
        printf("%s pageis %s %s %s %s (%d %d %d, last %d)\n", bad ? "FAIL" : "  ok", a, b, c, d ? d : "", ui.view, bl,
               mi, sqv.last);
        return 1;
    }
    if (!strcmp(cmd, "octis")) { printf("%s octis %s (octave %d)\n", set.oct != atoi(a) ? "FAIL" : "  ok", a, set.oct); return 1; }   /* (1.18C17) a check: the octave is a */
    if (!strcmp(cmd, "keydark")) {       /* GLO > KEY LIGHTS off (1) / on (0) / NIGHT (2, 1.18C43); b "save": GLO's settings
                                          * saved (as SAVE, or AUTO SAVE, does) */
        set.key_dark = (uint8_t)atoi(a);
        if (!strcmp(b, "save")) printf("  ok keydark %u (saved %d)\n", set.key_dark, glo_save());
        return 1;
    }
    if (!strcmp(cmd, "dract")) { printf("  dract %04x rs %d\n", dr.act, (dr.act >> 5) & 1); return 1; }
    if (!strcmp(cmd, "erases")) { printf("  store writes %d\n", 0); return 1; }
    if (!strcmp(cmd, "sqs")) {           /* the arp pattern: its steps on (a string), lc, div, blk, step */
        char on[65];
        uint32_t k;
        for (k = 0; k < 64u; k++) on[k] = sqp.st[k].ev & SQ_ON ? (char)('0' + (sqp.st[k].ev & 15u)) : '.';
        on[64] = 0;
        printf("  sqs pat %d lc %d div %d blk %d run %d step %d jr %d %s\n", set.sq_pat, sqp.lc, sqp.div, sqv.blk[0], sq.run, sq.step, un.n, on);
        return 1;
    }
    if (!strcmp(cmd, "drs")) {           /* the drum pattern: instrument a's steps */
        char on[65];
        uint32_t k, i = (uint32_t)atoi(a);
        for (k = 0; k < 64u; k++) on[k] = DR_ON(i, k) ? (char)('0' + (DR_ST(i, k) & 3u)) : '.';
        on[64] = 0;
        printf("  drs pat %d lc %d div %d blk %d run %d step %d ins %d %s\n", set.dr_pat, drp.lc, drp.div, sqv.blk[1], dr.run, dr.step, i, on);
        return 1;
    }
    if (!strcmp(cmd, "drsis")) {         /* (1.18C30) a check: the drum pattern's instrument a's steps 1-16 on (any hit) are b ("1.11..."); FAIL else */
        char on[17];
        uint32_t k, i = (uint32_t)atoi(a);
        for (k = 0; k < 16u; k++) on[k] = DR_ON(i, k) ? '1' : '.';
        on[16] = 0;
        printf("%s drsis %u %s%s%s\n", strcmp(on, b) ? "FAIL" : "  ok", (unsigned)i, on, strcmp(on, b) ? " wanted " : "", strcmp(on, b) ? b : "");
        return 1;
    }
    if (!strcmp(cmd, "phash")) {         /* a hash of the pattern's bytes: d the drums', s the arp shown's; the undo */
        const uint8_t *q = a[0] == 'd' ? (const uint8_t *)&drp : (const uint8_t *)&sqp;
        const uint32_t n = a[0] == 'd' ? (uint32_t)sizeof drp : (uint32_t)sizeof sqp;
        uint32_t h = 2166136261u, k;
        for (k = 0; k < n; k++) h = (h ^ q[k]) * 16777619u;
        printf("  phash %s %08x lc %d journal n %d\n", a, h, a[0] == 'd' ? drp.lc : sqp.lc, un.n);
        return 1;
    }
    if (!strcmp(cmd, "objsum")) {        /* the patterns' objects in the store: each one's size and a hash (COPY TO) */
        uint32_t o;
        printf("  objsum");
        for (o = OBJ_SEQ; o < OBJ_NOBJ; o++) {
            uint8_t x[PLAT_STORE_MAX];
            const int n = plat_store_size(o);
            uint32_t h = 2166136261u, k;
            if (n > 0 && plat_store_read(o, 0, x, (uint32_t)n) == n)
                for (k = 0; k < (uint32_t)n; k++) h = (h ^ x[k]) * 16777619u;
            printf(" %u:%d/%08x", (unsigned)o, n, n > 0 ? h : 0u);
        }
        printf(" writes %u skipped %u\n", store_n[0], store_n[1]);
        return 1;
    }
    if (!strcmp(cmd, "spst")) {          /* the sequencers' SAVE (0.67; the journal after 1.00): the patterns with edits,
                                          * the entries, asked, pending at the stop */
        printf("  spst pats %llx n %u marked %llx ask %d on %d wait %x light %d kit %d glo %d view %d menu %d msg %s / %s\n",
               (unsigned long long)jr_pats(), un.n, (unsigned long long)pk.m, sqv.sp_ask, sp_on(), sqv.sp_wait, sv_lit(), kv.dirty_t != 0,
               ui.glo_t != 0, ui.view, sqv.menu, ui.msg[0], ui.msg[1]);
        return 1;
    }
    if (!strcmp(cmd, "jrhas")) {         /* a check (after 1.00): pattern a ("d3": drum pattern 3, "a5": arp pattern 5)
                                          * has edits waiting in the journal (b 1) or none (0); FAIL else */
        const uint32_t k = (a[0] == 'd' ? 0u : DR_PATS) + (uint32_t)atoi(a + 1) - 1u, h = (uint32_t)(jr_pats() >> k & 1u);
        printf("%s jrhas %s %u (n %u)\n", h != (uint32_t)atoi(b) ? "FAIL" : "  ok", a, h, un.n);
        return 1;
    }
    if (!strcmp(cmd, "jrn")) {           /* a check (after 1.00): the journal's entries a (as host_jv: the builds' rooms
                                          * differ); 1.18C33: b given, pattern b's undo levels ("d3", "a5") c */
        uint32_t lv = 0, i, k = 0;
        int bad = (int)un.n != host_jv(a);
        if (b[0]) {
            k = (b[0] == 'd' ? 0u : DR_PATS) + (uint32_t)atoi(b + 1) - 1u;
            for (i = 0; i < un.n; i++) lv += (un.e[i] >> 31) && jr_pat(un.e[i]) == k;
            bad |= (int)lv != atoi(c);
        }
        printf("%s jrn %s %s %s (n %u of %u; levels %u)\n", bad ? "FAIL" : "  ok", a, b, c, un.n, JR_N, lv);
        return 1;
    }
    if (!strcmp(cmd, "jrlv")) {          /* (1.18C33) a check: pattern a's ("d3", "a5") undo levels b, marked c (0 / 1; "-"
                                          * or none: not checked); FAIL else */
        const uint32_t k = (a[0] == 'd' ? 0u : DR_PATS) + (uint32_t)atoi(a + 1) - 1u;
        uint32_t lv = 0, i;
        int bad;
        for (i = 0; i < un.n; i++) lv += (un.e[i] >> 31) && jr_pat(un.e[i]) == k;
        bad = (int)lv != atoi(b) || (c[0] && c[0] != '-' && pk_on(k) != atoi(c));
        printf("%s jrlv %s %s %s (levels %u, marked %d; n %u of %u, spare copies %u %u)\n", bad ? "FAIL" : "  ok", a, b, c,
               lv, pk_on(k), un.n, JR_N, pk.ck[0], PK_NC > 1u ? pk.ck[PK_NC - 1u] : 0u);
        return 1;
    }
    if (!strcmp(cmd, "psnap") || !strcmp(cmd, "pback")) {   /* (1.18C33) the drums' (a "d") or the arp sequencer
                                          * shown's pattern kept (psnap), put back as a small edit (pback): the tests'
                                          * stand-in for the UNDO of a small edit, which is gone */
        static union { dr_pat_t d; sq_pat_t s; } sn;
        const uint32_t q = a[0] == 'd' ? 0u : 1u + UN_SQI;
        if (cmd[1] == 's') pk_copy(&sn, un_cur(q), q ? DR_PATS : 0u);
        else { pk_copy(un_cur(q), &sn, q ? DR_PATS : 0u); (void)un_note(q); seq_blk_fix(); }
        printf("  %s %s\n", cmd, a);
        return 1;
    }
    if (!strcmp(cmd, "pkis")) {          /* (1.18C33) a check: the question up for pattern a ("0" none, "a3", "d2"), the
                                          * arp sequencer shown's pattern b and the drums' c (1 ..; "-": not checked) */
        const uint32_t k = a[0] == '0' ? 0u : (a[0] == 'd' ? 0u : DR_PATS) + (uint32_t)atoi(a + 1);
        const int bad = pk.ask != k || (b[0] && b[0] != '-' && (int)SQ_PATV + 1 != atoi(b))
                        || (c[0] && c[0] != '-' && (int)set.dr_pat + 1 != atoi(c));
        printf("%s pkis %s %s %s (ask %u, pattern %u, drums %u; marked %llx, spare copies", bad ? "FAIL" : "  ok", a, b, c,
               pk.ask, SQ_PATV + 1u, set.dr_pat + 1u, (unsigned long long)pk.m);
        {
            uint32_t i;
            for (i = 0; i < PK_NC; i++) printf(" %u", pk.ck[i]);
        }
        printf(")\n");
        return 1;
    }
    if (!strcmp(cmd, "jrfill")) {        /* (1.18C33) the journal filled up to a entries (as host_jv) with edits of the
                                          * drum patterns from 16 down not loaded (each byte of their steps' bits
                                          * and values flipped, unsaved): a full journal of edits that are not undone */
        static dr_pat_t p;
        uint32_t k = DR_PATS, i;
        while (un.n < (uint32_t)host_jv(a) && k--) {
            if (k == set.dr_pat || (dr_sp && k == dr_spn) || pk_on(k)) continue;
            dr_load_to(&p, k);
            for (i = 0; i < DR_NI * 2u * SQ_NB && un.n < (uint32_t)host_jv(a); i++)
                if (jr_find(k, jr_a(k) + i) < 0)
                    un.e[un.n++] = (jr_a(k) + i) << 16 | (uint32_t)((uint8_t *)&p)[i] << 8 | (((uint8_t *)&p)[i] ^ 0x5Au);
        }
        printf("  jrfill %s (n %u of %u)\n", a, un.n, JR_N);
        return 1;
    }
    if (!strcmp(cmd, "draccis")) {       /* (1.18C31) a check: the drum pattern's instrument a's steps 1-16 as b: '.' off,
                                          * else the accent (0 none, 1 ACC, 2 ACC+); FAIL else */
        char on[17];
        uint32_t k, i = (uint32_t)atoi(a);
        for (k = 0; k < 16u; k++) on[k] = DR_ON(i, k) ? (char)('0' + (DR_ST(i, k) & 3u)) : '.';
        on[16] = 0;
        printf("%s draccis %u %s%s%s\n", strcmp(on, b) ? "FAIL" : "  ok", (unsigned)i, on, strcmp(on, b) ? " wanted " : "", strcmp(on, b) ? b : "");
        return 1;
    }
    if (!strcmp(cmd, "drmiclr")) { dr_mhn = 0; return 1; }   /* (1.18C31) the drum channel's hits counted afresh */
    if (!strcmp(cmd, "drmiis")) {        /* a check (1.18C31): the drum channel's hits since drmiclr: a of them, the last
                                          * one's instrument b (0 BD .. 10 CB) and accent c ("-": not checked); FAIL else */
        const int bad = (int)dr_mhn != atoi(a) || (b[0] && b[0] != '-' && (int)(dr_mhv & 15u) != atoi(b))
                        || (c[0] && c[0] != '-' && (int)(dr_mhv >> 8) != atoi(c));
        printf("%s drmiis n %u ins %u acc %u\n", bad ? "FAIL" : "  ok", (unsigned)dr_mhn, (unsigned)(dr_mhv & 15u),
               (unsigned)(dr_mhv >> 8));
        return 1;
    }
    if (!strcmp(cmd, "drbis")) {         /* a check (after 1.00): the drum pattern's step byte a (from BD's step 1 on) is
                                          * b; FAIL else */
        const uint8_t v = ((const uint8_t *)drp.st)[atoi(a)];
        printf("%s drbis %s %s (%u)\n", v != atoi(b) ? "FAIL" : "  ok", a, b, v);
        return 1;
    }
    if (!strcmp(cmd, "jrok")) {          /* a check (after 1.00): each pattern in RAM (the drums', the arp sequencers')
                                          * is the store's bytes with its journal entries, and so is its undo copy
                                          * (the drums' unless it is the spare); FAIL else */
        static dr_pat_t d;
        static sq_pat_t q;
        uint32_t i, bad = 0;
        dr_load_to(&d, set.dr_pat);
        bad |= memcmp(&d, &drp, sizeof d) != 0 || (!dr_sp && memcmp(&d, &un_shd, sizeof d) != 0);
        for (i = 0; i < UN_NS; i++) {
#ifdef HYB_MULTI
            if ((i ? set.sqb_pad : set.sq_pad) & SQ_DISARM) continue;   /* (NONE: may keep the other's as it was) */
#endif
            sq_load_to(&q, jr_key(1u + i) - DR_PATS);
            bad |= (memcmp(&q, un_cur(1u + i), sizeof q) != 0) << (1u + i) | (memcmp(&q, &un_shs[i], sizeof q) != 0) << (3u + i);
        }
        printf("%s jrok (%x; n %u marked %llx)\n", bad ? "FAIL" : "  ok", bad, un.n, (unsigned long long)pk.m);
        return 1;
    }
    if (!strcmp(cmd, "wrmark")) { host_wr0 = store_n[0]; return 1; }
    if (!strcmp(cmd, "wris")) {          /* a check (after 1.00): the store's writes since wrmark a; FAIL else */
        printf("%s wris %s (%u)\n", store_n[0] - host_wr0 != (uint32_t)atoi(a) ? "FAIL" : "  ok", a, store_n[0] - host_wr0);
        return 1;
    }
    if (!strcmp(cmd, "drpokes")) {       /* (after 1.00) a of the drum pattern's step bytes (from instrument BD's step 1
                                          * on, c more if given; a as jrn's) set to b, as one edit (one undo group) */
        uint8_t *q = (uint8_t *)drp.st + atoi(c);
        int k;
        for (k = 0; k < host_jv(a); k++) q[k] = (uint8_t)atoi(b);
        dr_dirty(0);
        return 1;
    }
    if (!strcmp(cmd, "objall")) {        /* every object in the store: a hash (and the kit's bytes in OBJ_DRUM) */
        uint32_t o, hk = 2166136261u, k;
        static uint8_t x[PLAT_STORE_MAX];
        printf("  objall");
        for (o = 0; o < OBJ_NOBJ; o++) {
            const int n = plat_store_size(o);
            uint32_t h = 2166136261u;
            if (n > 0 && plat_store_read(o, 0, x, (uint32_t)n) == n)
                for (k = 0; k < (uint32_t)n; k++) h = (h ^ x[k]) * 16777619u;
            printf(" %u:%08x", (unsigned)o, n > 0 ? h : 0u);
        }
        if (plat_store_read(OBJ_DRUM, DR_KIT_OFF, x, sizeof drk) == (int)sizeof drk)
            for (k = 0; k < sizeof drk; k++) hk = (hk ^ x[k]) * 16777619u;
        printf(" kit:%08x writes %u\n", hk, store_n[0]);
        return 1;
    }
    if (!strcmp(cmd, "mkdirty")) {       /* the kit and GLO's settings changed (unsaved), as an edit would */
        drk.k[0].lev = (uint8_t)(drk.k[0].lev ^ 1u);
        kv.dirty_t = plat_ms() | 1u;
        glo_touch();
        return 1;
    }
    if (!strcmp(cmd, "cpst")) {          /* EDIT > COPY TO's state */
        printf("  cpst menu %d cp %d ask %d used %08x view %d msg %s / %s\n", sqv.menu, sqv.cp, sqv.cp_ask, sqv.cp_used,
               ui.view, ui.msg[0], ui.msg[1]);
        return 1;
    }
    if (!strcmp(cmd, "tempo")) {
        printf("  tempo shadow %d engA %d tp_run %d bpm_now %.2f", shadow[TP_BYTE], S.patch[TP_BYTE], tp_run, (double)S.bpm_now);
#ifdef HYB_MULTI
        printf(" engB %d ed %d", SB.patch[TP_BYTE], ed_part);
#endif
        printf(" em %d\n", em_n);
        return 1;
    }
    if (!strcmp(cmd, "tpis")) {          /* (1.17; 1.18 the global tempo) a check: the tempo running is BPM a (tp_run,
                                          * part A's engine, the panel's byte; two parts: part B's engine and the other
                                          * part's panel byte too); b (if given, two parts): part B's arpeggiator playing
                                          * (1) or not (0); FAIL else */
        const int v = atoi(a) - 63;
        int bad = tp_run != v + 1 || S.patch[TP_BYTE] != v || shadow[TP_BYTE] != v;
#ifdef HYB_MULTI
        if (SB.patch[TP_BYTE] != v || shadow_o[TP_BYTE] != v) bad = 1;
        if (b[0] && (SB.patch[129] && SB.arp_on) != (atoi(b) != 0)) bad = 1;
        printf("%s tpis %s %s (tp_run %d engA %d engB %d shadow %d other %d ed %d arpB %d/%d)\n", bad ? "FAIL" : "  ok", a,
               b, tp_run - 1 + 63, S.patch[TP_BYTE] + 63, SB.patch[TP_BYTE] + 63, shadow[TP_BYTE] + 63,
               shadow_o[TP_BYTE] + 63, ed_part, SB.patch[129], SB.arp_on);
#else
        printf("%s tpis %s (tp_run %d engA %d shadow %d)\n", bad ? "FAIL" : "  ok", a, tp_run - 1 + 63,
               S.patch[TP_BYTE] + 63, shadow[TP_BYTE] + 63);
#endif
        return 1;
    }
    if (!strcmp(cmd, "xbpmis")) {        /* (1.18) a check: the MIDI clock's tempo rules (S.xclk_on), S.bpm_now within
                                          * a .. b BPM; FAIL else */
        printf("%s xbpmis %s %s (on %d bpm_now %.2f)\n", S.xclk_on && S.bpm_now >= (float)atof(a)
               && S.bpm_now <= (float)atof(b) ? "  ok" : "FAIL", a, b, S.xclk_on, (double)S.bpm_now);
        return 1;
    }
    if (!strcmp(cmd, "stbyteis")) {      /* (1.18) a check: the patch up's (the part edited's) stored byte a is b; FAIL
                                          * else */
        const uint32_t v = patch_bytes(set.patch)[atoi(a) & 255];
        printf("%s stbyteis %s %s (%u)\n", v != (uint32_t)atoi(b) ? "FAIL" : "  ok", a, b, v);
        return 1;
    }
    if (!strcmp(cmd, "tpsaved")) {       /* (1.18) a check: the settings object's global tempo is BPM a (0: none saved,
                                          * set.tempo 0); FAIL else */
        settings_t s;
        settings_load(&s);
        printf("%s tpsaved %s (%d)\n", (atoi(a) ? s.tempo != atoi(a) - 62 : s.tempo != 0) ? "FAIL" : "  ok", a,
               s.tempo ? s.tempo + 62 : 0);
        return 1;
    }
    if (!strcmp(cmd, "oldobj")) {        /* the first releases' objects: arp pattern 3 (index 2) steps 2,3 on (FING 1),
                                          * div 3; drum pattern 2 (index 1): BD on 1-8, accent 2, div 1; the kit: BD level 50 */
        static uint8_t o[3840];
        uint32_t n, k;
        memset(o, 0, sizeof o);
        for (n = 0; n < 32u; n++) {
            uint8_t *p = o + n * 68u;
            for (k = 0; k < 16u; k++) { p[k * 4] = SQ_NOTE; p[k * 4 + 2] = 2; p[k * 4 + 3] = 100; }
            if (n == 2u) { p[1 * 4] = SQ_ON | SQ_FING; p[2 * 4] = SQ_ON | SQ_FING; }
            p[64] = SQ_MARK; p[65] = n == 2u ? 3 : 2; p[66] = 0; p[67] = 0;
        }
        printf("  oldobj seq %d\n", plat_store_save(OBJ_SEQ, o, 32u * 68u));
        memset(o, 0, sizeof o);
        for (n = 0; n < 16u; n++) {
            uint8_t *p = o + n * 200u;
            for (k = 0; k < 176u; k++) p[22 + k] = DR_SB(0u, DR_LEN1, 0u);
            if (n == 1u) { p[0] = 0xFF; for (k = 0; k < 8u; k++) p[22 + k] = DR_SB(2u, DR_LEN1, 0u); }
            p[198] = DR_MARK; p[199] = n == 1u ? 1 : 2;
        }
        {   /* the kit after them */
            dr_kit_t q;
            memset(&q, 0, sizeof q); q.mark = DR_KMARK; q.ver = DR_KV1;
            for (k = 0; k < DR_NI; k++) q.k[k] = DR_KP0;
            q.k[0].lev = 50;
            memcpy(o + 3200, &q, sizeof q);
        }
        printf("  oldobj drum %d\n", plat_store_save(OBJ_DRUM, o, 3200u + (uint32_t)sizeof(dr_kit_t)));
        printf("  sizes seqx %d drumx %d\n", plat_store_size(OBJ_SEQX), plat_store_size(OBJ_DRUMX));
        return 1;
    }
    if (!strcmp(cmd, "stdump") || !strcmp(cmd, "stload")) {   /* the store's objects to file a / from it (another
                                          * build's store: its patterns as that release saved them) */
        static uint8_t x[PLAT_STORE_MAX];
        FILE *f = fopen(a, cmd[2] == 'd' ? "wb" : "rb");
        uint32_t o;
        if (!f) { printf("FAIL %s %s\n", cmd, a); return 1; }
        for (o = 0; o < OBJ_NOBJ; o++) {
            int32_t n = cmd[2] == 'd' ? plat_store_size(o) : 0;
            if (cmd[2] == 'd') {
                if (n < 0 || plat_store_read(o, 0, x, (uint32_t)n) != n) n = 0;
                fwrite(&n, 4, 1, f);
                fwrite(x, 1, (size_t)n, f);
            } else if (fread(&n, 4, 1, f) == 1 && n > 0 && n <= (int32_t)sizeof x
                       && fread(x, 1, (size_t)n, f) == (size_t)n)
                plat_store_save(o, x, (uint32_t)n);
        }
        fclose(f);
        printf("  %s %s\n", cmd, a);
        return 1;
    }
    if (!strcmp(cmd, "kitset")) {        /* (0.98) instrument a's (0 BD ..) kit field b (lev tune dec att tone model pan
                                          * snd) to c, as stored (then dr_sends) */
        dr_kp_t *q = &drk.k[atoi(a)];
        const int v = atoi(c);
        if (!strcmp(b, "lev")) q->lev = (uint8_t)v;
        else if (!strcmp(b, "tune")) q->tune = (uint8_t)(v + DR_K0);
        else if (!strcmp(b, "dec")) q->dec = (uint8_t)(v + DR_K0);
        else if (!strcmp(b, "att")) q->att = (uint8_t)v;
        else if (!strcmp(b, "tone")) q->tone = (uint8_t)(v + DR_K0);
        else if (!strcmp(b, "model")) q->m909 = (uint8_t)v;
        else if (!strcmp(b, "pan")) q->pan = (int8_t)v;
        else if (!strcmp(b, "snd")) q->snd = (uint8_t)v;
        else { printf("FAIL kitset: no field %s\n", b); return 1; }
        dr_sends();
        return 1;
    }
    if (!strcmp(cmd, "hatis")) {         /* (0.98) a check: the hats' voice sounding (a 1) or not (0), the sample
                                          * (b 1: MODEL SMP) or the synthesized hat (0), "-" not checked; the build
                                          * with the sample's table: c 1 (0 without), "-" not checked; FAIL else */
        const int snd = dr.act >> DR_HH & 1u, smp = HAT_ON != 0u;
        const int bad = (a[0] && a[0] != '-' && snd != atoi(a)) || (b[0] && b[0] != '-' && smp != atoi(b))
                        || (c[0] && c[0] != '-' && HAT_HAVE != atoi(c));
        printf("%s hatis sounding %d sample %d table %d\n", bad ? "FAIL" : "  ok", snd, smp, HAT_HAVE);
        return 1;
    }
    if (!strcmp(cmd, "km")) { drk.k[atoi(a)].m909 = (uint8_t)atoi(b); return 1; }
    if (!strcmp(cmd, "hit")) { dr.prev = (uint8_t)((atoi(a) + 1) | atoi(b) << 4); return 1; }
    if (!strcmp(cmd, "drwav")) {         /* (1.18C42) the drums alone into a WAV: a the file, b the
                                          * hits (lines "block instrument stepbyte", in order), c the blocks, d
                                          * "LEVEL,DRIVEBYTE" (set.dr_lev, drk.drv; full scale 1.0 at 24 bit, the synth
                                          * silent); stereo, 24 bit, at S.fs. The drums' state cleared before and after */
        FILE *ev = fopen(b, "r"), *f = fopen(a, "wb");
        static int32_t lr[2u * SY_BLOCK];
        const uint32_t nb = (uint32_t)atoi(c);
        const uint8_t keepl = set.dr_lev, keepd = drk.drv;
        uint32_t blk, k, eb = 0, ei = 0, es = 0;
        int lv = DR_LEV1, dv0 = 0, have;
        if (!ev || !f) { printf("FAIL drwav: files\n"); if (ev) fclose(ev); if (f) fclose(f); return 1; }
        if (d[0]) sscanf(d, "%d,%d", &lv, &dv0);
        set.dr_lev = (uint8_t)lv;
        drk.drv = (uint8_t)dv0;
        dr_level();
        dr.act = dr.ck = 0;
        dr.prev = 0;
        dv.cidle = 1;
        {
            const uint32_t fs = (uint32_t)(S.fs + 0.5f), by = nb * SY_BLOCK * 6u;
            const uint8_t h[44] = {'R', 'I', 'F', 'F', (uint8_t)(by + 36u), (uint8_t)((by + 36u) >> 8),
                (uint8_t)((by + 36u) >> 16), (uint8_t)((by + 36u) >> 24), 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0,
                1, 0, 2, 0, (uint8_t)fs, (uint8_t)(fs >> 8), (uint8_t)(fs >> 16), 0, (uint8_t)(fs * 6u),
                (uint8_t)(fs * 6u >> 8), (uint8_t)(fs * 6u >> 16), (uint8_t)(fs * 6u >> 24), 6, 0, 24, 0, 'd', 'a', 't', 'a',
                (uint8_t)by, (uint8_t)(by >> 8), (uint8_t)(by >> 16), (uint8_t)(by >> 24)};
            fwrite(h, 1, 44, f);
        }
        have = fscanf(ev, "%u %u %u", &eb, &ei, &es) == 3;
        for (blk = 0; blk < nb; blk++) {
            while (have && eb <= blk) {
                dr_hit(ei, es);
                have = fscanf(ev, "%u %u %u", &eb, &ei, &es) == 3;
            }
            for (k = 0; k < 2u * SY_BLOCK; k++) lr[k] = 0;
            dr_render(lr, SY_BLOCK, 8388607.f);
            for (k = 0; k < 2u * SY_BLOCK; k++) {
                const uint8_t o[3] = {(uint8_t)lr[k], (uint8_t)(lr[k] >> 8), (uint8_t)(lr[k] >> 16)};
                fwrite(o, 1, 3, f);
            }
        }
        fclose(ev);
        fclose(f);
        dr.act = dr.ck = 0;
        set.dr_lev = keepl;
        drk.drv = keepd;
        dr_level();
        printf("  drwav %s %u blocks\n", a, (unsigned)nb);
        return 1;
    }
    if (!strcmp(cmd, "vact")) {
#ifdef HYB_MULTI
        printf("  vact nvA %d nvB %d mode %d A %d B %d oct %d tr %d split %d\n", S.nv, SB.nv, M.mode_now, sy_voices_active(&S), sy_voices_active(&SB), set.oct, set.tr, set.mx_split);
#endif
        return 1;
    }
    if (!strcmp(cmd, "vstat")) {         /* (after 1.08) each part's voices: its slice, limit, on, gated, fading out, the
                                          * engine's keys held, the notes waiting (uni_pend); the panel's keys down,
                                          * HOLD's notes, its restarts waiting, the panel's queue ("vstat v": each voice) */
        int p, i, k;
        for (p = 0; p < HOST_NP; p++) {
            const sy_synth *s = HOST_PART(p);
            int on = 0, g = 0, f = 0, h = 0, up = 0;
            for (i = 0; i < HOST_NV(s); i++) { on += s->v[i].on; g += s->v[i].on && s->v[i].gate; }
#ifdef SY_UNI_CAP
            for (i = 0; i < HOST_NV(s); i++) f += s->v[i].on && s->v[i].ufade;
            up = s->upn;
#endif
            for (k = 0; k < 128; k++) h += s->held[k] != 0;
            if (a[0] == 'v')
                for (i = 0; i < HOST_NV(s); i++)
                    if (s->v[i].on)
                        printf("    %c%02d gate %d key %d uni %d/%d st %d lvl %.2e age %u\n", 'A' + p, i, s->v[i].gate, s->v[i].key,
                               s->v[i].uni_i, s->v[i].uni_n, s->v[i].aenv.stage, (double)s->v[i].aenv.lvl, (unsigned)s->v[i].age);
            printf("  vstat %c nv %d max %d on %d gated %d fading %d held %d upn %d\n", 'A' + p, HOST_NV(s), s->max_voices, on, g, f, h, up);
        }
        printf("  vstat keys %07x hold %u restart %07x evq %u", (unsigned)ui.keys, host_hold_n(), (unsigned)key_rs, (unsigned)(ev_w - ev_r));
#ifdef HYB_MULTI
        { int kp = 0; for (k = 0; k < 128; k++) kp += M.kp[k] != 0;
          printf(" cap %d mode %d dyn %d kp %d", M.cap_now, M.mode_now, M.dyn_now, kp); }
#endif
        printf("\n");
        return 1;
    }
    if (!strcmp(cmd, "vfree")) {         /* (after 1.08) a check: nothing sounds or is held anywhere (every voice free, no key
                                          * held in the engine, none waiting; no key down, no HOLD note, no restart
                                          * waiting; two parts: the setup's key table empty); FAIL else */
        int p, i, k, bad = 0;
        for (p = 0; p < HOST_NP; p++) {
            const sy_synth *s = HOST_PART(p);
            for (i = 0; i < HOST_NV(s); i++) bad += s->v[i].on;
            for (k = 0; k < 128; k++) bad += s->held[k] != 0;
#ifdef SY_UNI_CAP
            bad += s->upn;
#endif
        }
        bad += ui.keys != 0u || host_hold_n() || key_rs;
#ifdef HYB_MULTI
        for (k = 0; k < 128; k++) bad += M.kp[k] != 0;
#endif
        printf("%s vfree\n", bad ? "FAIL" : "  ok");
        if (bad) app_host_cmd("vstat", "", "", "", "");
        return 1;
    }
    if (!strcmp(cmd, "keyson") || !strcmp(cmd, "kmiss")) {   /* (after 1.08) "keyson KEY" a check: key KEY (w0..w15,
                                          * b0..b10) down, every part its note goes to has a voice gated on that note;
                                          * FAIL else. "kmiss": the same for every key down, counted, no check; "kmiss
                                          * show": the counts since the last */
        static uint32_t kn, km[2];
        const int one = cmd[1] == 'e';
        const int kk = !one ? -1 : a[0] == 'w' ? HOST_WHITE_K[atoi(a + 1) & 15] : HOST_BLACK_K[atoi(a + 1) % 11];
        uint32_t k;
        int p, i;
        if (!one && !strcmp(a, "show")) { printf("  kmiss keys %u missA %u missB %u\n", kn, km[0], km[1]); kn = km[0] = km[1] = 0; return 1; }
        if (one && (!(ui.keys >> kk & 1u) || ui.held[kk] == KEY_NONE)) { printf("FAIL keyson %s: not down\n", a); return 1; }
        for (k = 0; k < NKEYS; k++) {
            if (!(ui.keys >> k & 1u) || ui.held[k] == KEY_NONE || (one && (int)k != kk)) continue;
            kn++;
            for (p = 0; p < HOST_NP; p++) {
                const sy_synth *s = HOST_PART(p);
                int f = 0;
#ifdef HYB_MULTI
                if (!(sq_parts_key(ui.held[k]) >> p & 1u)) continue;
#endif
                for (i = 0; i < HOST_NV(s); i++) f |= s->v[i].on && s->v[i].gate && s->v[i].key == ui.held[k];
                if (!f) km[p]++;
                if (one) printf("%s keyson %s part %c note %u\n", f ? "  ok" : "FAIL", a, 'A' + p, ui.held[k]);
            }
        }
        return 1;
    }
    if (!strcmp(cmd, "nvis")) {          /* (after 1.18C34) a check: the note of key a (w0..w15, b0..b10, down) sounds
                                          * with b voices (gated, not fading; whatever their unison count) on part c
                                          * (default A); FAIL else (tests/grd_keep.txt: the guard's shed) */
        const int kk = a[0] == 'w' ? HOST_WHITE_K[atoi(a + 1) & 15] : HOST_BLACK_K[atoi(a + 1) % 11];
        const int p = c[0] ? atoi(c) : 0, want = atoi(b);
        int i, n = 0;
        const sy_synth *s = HOST_PART(p < HOST_NP ? p : 0);
        if (!(ui.keys >> kk & 1u) || ui.held[kk] == KEY_NONE) { printf("FAIL nvis %s: not down\n", a); return 1; }
        for (i = 0; i < HOST_NV(s); i++)
            if (s->v[i].on && s->v[i].gate && s->v[i].key == ui.held[kk]
#ifdef SY_UNI_CAP
                && !s->v[i].ufade
#endif
               ) n++;
        printf("%s nvis %s %d part %c (%d voices)\n", n != want ? "FAIL" : "  ok", a, want, 'A' + p, n);
        return 1;
    }
    if (!strcmp(cmd, "unis")) {          /* (after 1.09) a check: the note of key a (w0..w15, b0..b10, down) plays b unison
                                          * voices (gated, not fading) on part c (default A), each of count b; FAIL else */
        const int kk = a[0] == 'w' ? HOST_WHITE_K[atoi(a + 1) & 15] : HOST_BLACK_K[atoi(a + 1) % 11];
        const int p = c[0] ? atoi(c) : 0, want = atoi(b);
        int i, n = 0, bad = 0;
        const sy_synth *s = HOST_PART(p < HOST_NP ? p : 0);
        if (!(ui.keys >> kk & 1u) || ui.held[kk] == KEY_NONE) { printf("FAIL unis %s: not down\n", a); return 1; }
        for (i = 0; i < HOST_NV(s); i++)
            if (s->v[i].on && s->v[i].gate && s->v[i].key == ui.held[kk]
#ifdef SY_UNI_CAP
                && !s->v[i].ufade
#endif
               ) { n++; bad += s->v[i].uni_n != want; }
        printf("%s unis %s %d part %c (%d voices)\n", n != want || bad ? "FAIL" : "  ok", a, want, 'A' + p, n);
        return 1;
    }
    if (!strcmp(cmd, "gov")) { GOV_MAX = atoi(a); return 1; }   /* (after 1.08) the governor's limit set, as a load would */
    if (!strcmp(cmd, "mxset")) {         /* (after 1.08) GLO > MULTI's a (uni, unim, uauto, steal, dyn, maxv, nva) to b */
#ifdef HYB_MULTI
        const uint8_t v = (uint8_t)atoi(b);
        if (!strcmp(a, "uni") || !strcmp(a, "unis") || !strcmp(a, "unil")) {   /* (1.18C48: "uni" both poly caps, as the one
                                          * UNISON MAX before; "unis" UNI MAX SINGLE, "unil" UNI MAX MULTI; 0 .. 4) */
            if (a[3] != 'l') { set.mx_uni = (uint8_t)(v > 3u ? 3u : v); set.mx_unx = (uint8_t)((set.mx_unx & ~MX_UNX_S4) | (v > 3u ? MX_UNX_S4 : 0u)); }
            if (a[3] != 's') set.mx_unx = (uint8_t)((set.mx_unx & ~MX_UNX_L) | (v + 1u));
        } else if (!strcmp(a, "unim")) {
            set.mx_unim = (uint8_t)(v > 5u ? 5u : v);
            set.mx_unx = (uint8_t)((set.mx_unx & ~MX_UNX_M6) | (v > 5u ? MX_UNX_M6 : 0u));
        }
        else if (!strcmp(a, "uauto")) set.mx_uauto = v; else if (!strcmp(a, "steal")) set.mx_steal = v;
        else if (!strcmp(a, "dyn")) set.mx_dyn = v; else if (!strcmp(a, "maxv")) set.mx_maxv = v;
        else if (!strcmp(a, "nva")) set.mx_nva = v;
        else { printf("FAIL mxset: no %s\n", a); return 1; }
        mx_apply();
#endif
        return 1;
    }
    if (!strcmp(cmd, "ccfree")) {
        uint32_t c; char t[40];
        printf("  ccfree");
        for (c = 0; c < 128u; c++) { t[0] = 0; if (!cc_name(c, t)) printf(" %u", c); }
        printf("\n");
        return 1;
    }
    if (!strcmp(cmd, "pvol")) {
#ifdef HYB_MULTI
        printf("  pvol A %d B %d rtA %.4f rtB %.4f partgA %.4f partgB %.4f ed %d oct %d tempo %d vcc %d %d\n", pv_vol[0], pv_vol[1], (double)S.rt_g, (double)SB.rt_g, (double)S.part_g, (double)SB.part_g, ed_part, set.oct, shadow[TP_BYTE], set.mx_vcc[0], set.mx_vcc[1]);
#endif
        return 1;
    }
    if (!strcmp(cmd, "fitscan")) {       /* (0.87) the blocks' texts against a column's room: a the room of a value
                                          * in FONT_M, FONT_B (FONT_S) and FONT_XS (50 up to 0.86, the columns 53
                                          * wide; 55 since 0.87, 57 wide, the side bar gone), b its room in FONT_L (48;
                                          * 55), c "s": FONT_S between FONT_B and FONT_XS (0.87), d "v": the texts
                                          * that take FONT_XS listed. Every value of every row's knob, the patch as it
                                          * is; the labels (FONT_XS, as always) against a; the neighbours' rule
                                          * (row_big) left out */
        const int32_t W = atoi(a), WL = atoi(b), withs = c[0] == 's';
        uint32_t r, k, v, nl = 0, nv[6] = {0, 0, 0, 0, 0, 0}, np = 0, npx = 0;
        for (r = 0; r < U_NROW; r++)
            for (k = 0; k < 4u; k++) {
                const u_param_t *p = &U_ROW[r].p[k];
                uint32_t x = 0;
                if (!p->label[0]) continue;
                np++;
                if (text_w(&FONT_XS, p_label(p)) > W) nl++;
                for (v = 0; v <= p_max(p); v++) {
                    char s[24];
                    value_text(p, v, s);
                    if (p->kind == V_SHAPE && text_w(&FONT_XS, s) > W) put_sint(s, (int32_t)v, 0);
                    if (text_w(&FONT_L, s) <= WL) nv[0]++;
                    else if (text_w(&FONT_M, s) <= W) nv[1]++;
                    else if (text_w(&FONT_B, s) <= W) nv[2]++;
                    else if (withs && text_w(&FONT_S, s) <= W) nv[3]++;
                    else {
                        nv[text_w(&FONT_XS, s) <= W ? 4 : 5]++;
                        if (d[0] && !x) printf("    %s: %s\n", p_label(p), s);
                        x = 1;
                    }
                }
                npx += x;
            }
        printf("  fitscan %s %s %s: %u knobs, labels too wide %u; values L %u M %u B %u S %u XS %u cut %u; knobs with "
               "an XS value %u\n", a, b, c, (unsigned)np, (unsigned)nl, (unsigned)nv[0], (unsigned)nv[1],
               (unsigned)nv[2], (unsigned)nv[3], (unsigned)nv[4], (unsigned)nv[5], (unsigned)npx);
        return 1;
    }
    if (!strcmp(cmd, "kit")) { printf("  kit BD lev %d\n", drk.k[0].lev); return 1; }
    if (!strcmp(cmd, "sizes")) {
        uint32_t o;
        printf("  sizes");
        for (o = OBJ_SEQ; o < OBJ_NOBJ; o++) printf(" %d", plat_store_size(o));
        printf("\n");
        return 1;
    }
    if (!strcmp(cmd, "mxmode")) {
#ifdef HYB_MULTI
        set.mx_mode = (uint8_t)atoi(a);
        if (set.mx_mode == SY_M_SINGLE) part_edit(0);   /* (as GLO > MULTI > MODE) */
        mx_apply();
#endif
        return 1;
    }
    if (!strcmp(cmd, "mxgv")) {          /* (1.16) GLO > MULTI's MODE (a "mode") or SPLIT (a "split") to b, as turned there
                                          * (gv_set: the notes ended on a MODE change, LAYER's one HOLD) */
#ifdef HYB_MULTI
        if (!strcmp(a, "mode")) gv_set(G_MMODE, (uint32_t)atoi(b));
        else if (!strcmp(a, "split")) gv_set(G_MSPLIT, (uint32_t)atoi(b));
        else printf("FAIL mxgv: no %s\n", a);
#endif
        return 1;
    }
    if (!strcmp(cmd, "sqkeysis")) {      /* (1.16) a check: arp sequencer a's (0 A, 1 B) input notes (the keys held, HOLD's;
                                          * sq_keys_put as now) are b "60,64" ("-" none); FAIL else (the builds with
                                          * two parts; else ignored) */
#ifdef HYB_MULTI
        char w[160] = "";
        int k;
        const uint32_t i = (uint32_t)atoi(a) & 1u;
        sq_keys_put(ui.keys);
        for (k = 0; k < 128; k++)
            if (sqr[i].keys[k >> 5] >> (k & 31) & 1u) sprintf(w + strlen(w), "%s%d", w[0] ? "," : "", k);
        if (!w[0]) strcpy(w, "-");
        printf("%s sqkeysis %s %s\n", strcmp(w, b) ? "FAIL" : "  ok", a, w);
#endif
        return 1;
    }
    if (!strcmp(cmd, "holdtile")) {      /* (1.16) a check: HOME's HOLD tile reads a (ON, A_ON, B_ON, OFF; "_" a space) as
                                          * draw_home and ui.c's home_tiles make it; FAIL else */
#ifdef HYB_MULTI
        const uint32_t h = hold_on | (set.mx_mode >= SY_M_SPLIT ? hold_o << 1 : 0u);
        const char *t = h & 1u ? "ON" : h ? (ed_part ? "A_ON" : "B_ON") : "OFF";
#else
        const char *t = hold_on ? "ON" : "OFF";
#endif
        printf("%s holdtile %s (%s)\n", strcmp(t, a) ? "FAIL" : "  ok", a, t);
        return 1;
    }
    if (!strcmp(cmd, "part")) {
#ifdef HYB_MULTI
        printf("  part ed %d mode %d acc %04x view %d\n", ed_part, set.mx_mode, u_acc, ui.view);
#endif
        return 1;
    }
    if (!strcmp(cmd, "sq2")) {           /* both arp sequencers (the USB build) */
#ifdef HYB_MULTI
        uint32_t i, k;
        for (i = 0; i < SQ_NI; i++) {
            char on[65];
            for (k = 0; k < 64u; k++) on[k] = sqpa[i].st[k].ev & SQ_ON ? (char)('0' + (sqpa[i].st[k].ev & 15u)) : '.';
            on[64] = 0;
            printf("  sq%c pat %d none %d base %d lc %d run %d on %d step %d parp %d hold %d keys %08x%08x %s\n",
                   'A' + i,
                   i ? set.sqb_pat : set.sq_pat, i ? set.sqb_pad & 1 : set.sq_pad & 1, i ? set.sqb_base : set.sq_base,
                   sqpa[i].lc, sqr[i].run, sqr[i].on, sqr[i].step, sqpa[i].pad, sqr[i].hold, sqr[i].keys[1],
                   sqr[i].keys[2], on);
        }
        printf("  shown %d view %d theme %d ed %d arpA %d arpB %d shA %d shB %d\n", sq_mi, ui.view, u_th == &U_THEMES[1], ed_part, S.patch[129], SB.patch[129],
               ed_part ? shadow_o[129] : shadow[129], ed_part ? shadow[129] : shadow_o[129]);
#endif
        return 1;
    }
    if (!strcmp(cmd, "gcnt")) {          /* (1.19.2) a check: note a's gated voices on part d (default A): b of them,
                                          * c of those held by the sustain pedal (gate 2; c "-": not checked); FAIL else */
        const int p = d[0] ? atoi(d) : 0;
        const sy_synth *s = HOST_PART(p < HOST_NP ? p : 0);
        int i, n = 0, h = 0;
        for (i = 0; i < HOST_NV(s); i++)
            if (s->v[i].on && s->v[i].gate && s->v[i].key == atoi(a)) { n++; h += s->v[i].gate == 2; }
        printf("%s gcnt %s part %d: %d gated, %d held by the pedal\n",
               n != atoi(b) || (c[0] && c[0] != '-' && h != atoi(c)) ? "FAIL" : "  ok", a, p, n, h);
        return 1;
    }
    if (!strcmp(cmd, "pedis")) {         /* (1.19.2) a check: the sustain pedal down (1) or up (0) in part A's engine
                                          * (a) and part B's (b, the builds with two parts); its CC 64 value (c, "-":
                                          * not checked) as the matrix reads it; FAIL else */
        int bad = S.sustain_hold != atoi(a) || (c[0] && c[0] != '-' && S.cc[64] != atoi(c));
#ifdef HYB_MULTI
        if (b[0] && SB.sustain_hold != atoi(b)) bad = 1;
        printf("%s pedis A %d B %d cc64 %d\n", bad ? "FAIL" : "  ok", S.sustain_hold, SB.sustain_hold, S.cc[64]);
#else
        printf("%s pedis A %d cc64 %d\n", bad ? "FAIL" : "  ok", S.sustain_hold, S.cc[64]);
#endif
        return 1;
    }
#ifdef HYB_MULTI
    if (!strcmp(cmd, "pmccis")) {        /* (1.19.2) a check: PLAY mode's control a (0 K1 .. 6 ALGO) on MIDI CC b ("OFF":
                                          * none), LEARN armed for control c (given; "-" none); FAIL else */
        const int w = strcmp(b, "OFF") ? atoi(b) + 1 : 0;
        const int bad = set.pm_cc[atoi(a) % 7] != w || (c[0] && pm_learn != (c[0] == '-' ? 0 : atoi(c) + 1));
        printf("%s pmccis %s %s %s (%d, learn %d)\n", bad ? "FAIL" : "  ok", a, b, c, set.pm_cc[atoi(a) % 7] - 1, pm_learn - 1);
        return 1;
    }
    if (!strcmp(cmd, "pmset")) { set.pm_cc[atoi(a) % 7] = (uint8_t)atoi(b); return 1; }   /* (1.19.2) a control's CC
                                          * as stored (0 OFF, 1 + the controller), any value */
    if (!strcmp(cmd, "peis")) {          /* (1.19.2) a check: the assignment editor's line a reads b ("_" a space); its
                                          * strip c and d (given); FAIL else */
        char w[48] = "", x[40] = "", y[40] = "", wb[48] = "", wc[40] = "", wd[40] = "", pre[6] = "";
        uint32_t k, fl = 0;
        int bad;
        for (k = 0; k < 47u && b[k]; k++) wb[k] = b[k] == '_' ? ' ' : b[k];
        for (k = 0; k < 39u && c[k]; k++) wc[k] = c[k] == '_' ? ' ' : c[k];
        for (k = 0; k < 39u && d[k]; k++) wd[k] = d[k] == '_' ? ' ' : d[k];
        pv_fill();
        pe_item((uint32_t)atoi(a), w, pre, &fl);
        play_lcd(x, y);
        bad = strcmp(w, wb) || (c[0] && strcmp(x, wc)) || (d[0] && strcmp(y, wd));
        printf("%s peis %s \"%s\" strip \"%s\" \"%s\"\n", bad ? "FAIL" : "  ok", a, w, x, y);
        return 1;
    }
    if (!strcmp(cmd, "ebyteis")) {       /* (1.19.2) a check: part A's engine's patch byte a is b (the panel not
                                          * checked: the fixed map's CCs go to the engine alone); FAIL else */
        printf("%s ebyteis %s %s (%d)\n", S.patch[atoi(a) & 255] != atoi(b) ? "FAIL" : "  ok", a, b, S.patch[atoi(a) & 255]);
        return 1;
    }
#endif
#ifdef HOST_XM
    if (!strcmp(cmd, "clkl")) { set.ck_fl = (uint8_t)(atoi(a) ? 0u : CK_FL_UNLOCK); return 1; }   /* (1.19.5) CLK LOCK
                                          * on (1) / off (0), as GLO's line sets it */
    if (!strcmp(cmd, "clklis")) {        /* (1.19.5) a check: CLK LOCK as GLO shows it (ON / OFF) */
        const int on = !(set.ck_fl & CK_FL_UNLOCK);
        printf("%s clklis %s (%s)\n", on == !strcmp(a, "ON") ? "  ok" : "FAIL", a, on ? "ON" : "OFF");
        return 1;
    }
    if (!strcmp(cmd, "xm")) {            /* (1.19.5) xm BPM [JIT_MS [BURST_MS [SEED]]]: the simulated master's START
                                          * and its clock from now (the first tick with START); "xm stop": STOP */
        if (!strcmp(a, "stop")) { hx.on = 0; hx_push(host_xnow, 0xFCu); return 1; }
        memset(&hx, 0, sizeof hx);
        hx.on = 1;
        hx.t0 = hx.ts = hx.alast = host_xnow;
        hx.bpm0 = hx.bpm1 = atof(a);
        hx.rt0 = hx.rt1 = 0.;
        hx.jit = c[0] || b[0] ? atof(b) * 1e-3 : 0.;
        hx.burst = c[0] ? atof(c) * 1e-3 : 0.;
        hx.seed = d[0] ? (uint32_t)atoi(d) : 1u;
        hx_push(host_xnow, 0xFAu);
        return 1;
    }
    if (!strcmp(cmd, "xmtempo")) {       /* (1.19.5) xmtempo BPM [MS]: the master's tempo to BPM, at once or a linear ramp */
        hx.bpm0 = hx_bpm(hx.ts);
        hx.bpm1 = atof(a);
        hx.rt0 = hx.ts;
        hx.rt1 = hx.ts + (b[0] ? atof(b) * 1e-3 : 0.);
        if (hx.rt1 <= hx.rt0) hx.bpm0 = hx.bpm1;
        return 1;
    }
    if (!strcmp(cmd, "xkis")) {          /* (1.19.5) CLK LOCK's state, printed */
        printf("  xkis %s: st %u lk %u pend %u cnt %u e %.2f ms ri %.4f per %.1f bpm %.2f mt %u\n", a, xk.st, xk.lk, xk.pend,
               xk.cnt, (double)xk.e * 1e3, (double)xk.ri, (double)xk.per, (double)S.bpm_now, (unsigned)ck.mt);
        return 1;
    }
    if (!strcmp(cmd, "xmdrop")) { hx.drop = (uint32_t)atoi(a); return 1; }   /* (1.19.5) the next N ticks lost */
    if (!strcmp(cmd, "xmmark")) { host_xnn = 0; host_xmk = 1; return 1; }   /* (1.19.5) the note-ons' times from now */
    if (!strcmp(cmd, "xmpull")) {        /* (1.19.5) xmpull FROM_S THR_MS [WANT_MS]: the time from FROM s after xm until
                                          * the notes' error (as xmstat's) stays within WANT +- THR */
        const double pf = hx.t0 + atof(a), thr = atof(b), want = c[0] ? atof(c) : 0.;
        double plast = -1., emax = 0.;
        uint32_t i, j = 0;
        for (i = 0; i < host_xnn && hx.nb; i++) {
            const double t = host_xnt[i];
            double er;
            while (j + 1u < hx.nb && fabs(hx.beat[j + 1u] - t) <= fabs(hx.beat[j] - t)) j++;
            er = (t - hx.beat[j]) * 1e3 - want;
            if (t < pf) continue;
            if (fabs(er) > emax) emax = fabs(er);
            if (fabs(er) > thr) plast = t;
        }
        printf("  xmpull from %s s: the largest error %.2f ms, within %s ms after %.2f s\n", a, emax, b,
               plast < pf ? 0. : plast - pf);
        return 1;
    }
    if (!strcmp(cmd, "xmstat")) {        /* (1.19.5) xmstat LABEL T0 T1 [MAXERR_MS[:WANT_MS]]: the
                                          * note-ons between T0 and T1 s after xm against the master's beats (each to
                                          * the nearest): count, beats without a note or with two (want 0), the error
                                          * (note - beat, ms) mean / min / max, the step-to-step jitter (one error less
                                          * the one before: rms, max), the error every 30 s; a check: max |error - WANT|
                                          * <= MAXERR, no lost or double steps (no MAXERR: printed alone, "--") */
        const double t0 = hx.t0 + atof(b), t1 = hx.t0 + atof(c), mx = d[0] ? atof(d) : 1e9,
                     want = d[0] && strchr(d, ':') ? atof(strchr(d, ':') + 1) : 0.;
        static uint8_t hit[16384];
        double sum = 0., lo = 1e9, hi = -1e9, js = 0., jm = 0., ep = 0., ck = 30.;
        uint32_t i, nn = 0, lost = 0, dbl = 0, j = 0, nj = 0, bad;
        memset(hit, 0, sizeof hit);
        printf("  xmstat %s:", a);
        for (i = 0; i < host_xnn; i++) {
            const double t = host_xnt[i];
            double er;
            if (t < t0 || t > t1 || !hx.nb) continue;
            while (j + 1u < hx.nb && fabs(hx.beat[j + 1u] - t) <= fabs(hx.beat[j] - t)) j++;
            er = (t - hx.beat[j]) * 1e3;
            if (hit[j] < 255u) hit[j]++;
            sum += er; nn++;
            if (er < lo) lo = er;
            if (er > hi) hi = er;
            if (nn > 1u) { const double dj = er - ep; js += dj * dj; nj++; if (fabs(dj) > jm) jm = fabs(dj); }
            ep = er;
            if (t - hx.t0 >= ck) { printf(" @%.0fs %.1f", t - hx.t0, er); ck += 30.; }
        }
        for (i = 0; i < hx.nb; i++)
            if (hx.beat[i] > t0 + 0.25 && hx.beat[i] < t1 - 0.25 && hx.beat[i] < host_xnow - 0.25) {
                if (!hit[i]) lost++;
                else if (hit[i] > 1u) dbl++;
            }
        printf("\n");
        bad = !nn || lost || dbl || hi - want > mx || want - lo > mx;
        printf("%s xmstat %s: %u notes, lost %u, double %u; error mean %.2f min %.2f max %.2f ms (want %.0f +- %.1f); "
               "step jitter rms %.2f max %.2f ms; arrivals late %.2f ms mean", !d[0] ? "  --" : bad ? "FAIL" : "  ok", a, (unsigned)nn,
               (unsigned)lost, (unsigned)dbl, nn ? sum / nn : 0., lo, hi, want, mx, nj ? sqrt(js / nj) : 0., jm,
               hx.nl ? hx.late / hx.nl * 1e3 : 0.);
        printf("\n");
        return 1;
    }
#endif
    if (!strcmp(cmd, "momark")) { host_motn = 0; host_mt0 = plat_ms(); return 1; }   /* (1.19.4) the note-ons' times
                                          * afresh, from now */
    if (!strcmp(cmd, "mosave")) {        /* (1.19.4) the times since momark kept as set a (0..3); printed */
        const uint32_t k = (uint32_t)atoi(a) & 3u;
        uint32_t i;
        memcpy(host_mos[k], host_mot, sizeof host_mot); host_mosn[k] = host_motn;
        printf("  mosave %u: %u notes:", (unsigned)k, (unsigned)host_motn);
        for (i = 0; i < host_motn && i < 20u; i++) printf(" %u", (unsigned)host_mot[i]);
        printf("\n");
        return 1;
    }
    if (!strcmp(cmd, "modelta")) {       /* (1.19.4) a check: set y's note-on times less set x's (a "x:y"), notes b .. c - 1
                                          * (both sets that many), each within d (ms, "-20:3": -20 +- 3); FAIL else */
        const uint32_t x = (uint32_t)atoi(a) & 3u, y = (uint32_t)atoi(strchr(a, ':') ? strchr(a, ':') + 1 : a) & 3u,
                       i0 = (uint32_t)atoi(b), i1 = (uint32_t)atoi(c);
        const int w = atoi(d), tol = strchr(d, ':') ? atoi(strchr(d, ':') + 1) : 0;
        int lo = 99999, hi = -99999, bad = host_mosn[x] < i1 || host_mosn[y] < i1;
        uint32_t i;
        for (i = i0; i < i1 && !bad; i++) {
            const int dd = (int)host_mos[y][i] - (int)host_mos[x][i];
            if (dd < lo) lo = dd;
            if (dd > hi) hi = dd;
        }
        bad |= lo < w - tol || hi > w + tol;
        printf("%s modelta %u %u notes %u..%u: %d .. %d ms (want %s; counts %u %u)\n", bad ? "FAIL" : "  ok", (unsigned)x,
               (unsigned)y, (unsigned)i0, (unsigned)i1 - 1u, lo, hi, d, (unsigned)host_mosn[x], (unsigned)host_mosn[y]);
        return 1;
    }
    if (!strcmp(cmd, "mochs")) {         /* (1.19.4) the note-ons out since moclr on each channel ("ch2:4"), labelled a */
        uint32_t k;
        printf("  mochs %s:", a);
        for (k = 0; k < 16u; k++) if (host_mon[k]) printf(" ch%u:%u", (unsigned)k + 1u, (unsigned)host_mon[k]);
        printf("\n");
        return 1;
    }
#ifdef HYB_MULTI
    if (!strcmp(cmd, "gvx")) {           /* (1.19.4) a GLO line's value to list entry b, as turned there (gv_set): ain
                                          * (OFF, OMNI, 1..16), aout (OFF, 1..16), bin, bout, clko (0 = -50 ms), clk */
        static const char *const N[6] = {"ain", "aout", "bin", "bout", "clko", "clk"};
        static const uint8_t G[6] = {G_CH, G_OCH, G_MCHB, G_BOUT, G_CLKO, G_CLK};
        uint32_t k;
        for (k = 0; k < 6u && strcmp(a, N[k]); k++)
            ;
        if (k < 6u) gv_set(G[k], (uint32_t)atoi(b)); else printf("FAIL gvx: no %s\n", a);
        return 1;
    }
#endif
    if (!strcmp(cmd, "moclr")) { memset(host_mon, 0, sizeof host_mon); memset(host_mnn, 0, sizeof host_mnn); return 1; }   /* (1.19.2) the
                                          * counts afresh */
    if (!strcmp(cmd, "monote")) {        /* (1.19.3) a check: note-ons out on channel a (1..16) of note b since moclr: c
                                          * ("N+": at least N), and (d given) the last one's velocity d; FAIL else */
        const uint32_t ch = (uint32_t)(atoi(a) - 1) & 15u, nt = (uint32_t)atoi(b) & 127u, n = host_mnn[ch][nt],
                       w = (uint32_t)atoi(c), ge = strchr(c, '+') != 0, bad = (ge ? n < w : n != w)
                                                                         || (d[0] && host_mnv[ch][nt] != atoi(d));
        printf("%s monote ch %s note %s: %u (want %s) vel %u\n", bad ? "FAIL" : "  ok", a, b, (unsigned)n, c,
               (unsigned)host_mnv[ch][nt]);
        return 1;
    }
    if (!strcmp(cmd, "mohang")) {        /* (1.19.3) a check: no note left on out over MIDI on channel a (1..16; 0: any
                                          * channel): note-ons less note-offs, all notes off clearing; FAIL else */
        uint32_t ch, nt, k = 0;
        for (ch = 0; ch < 16u; ch++)
            for (nt = 0; nt < 128u; nt++)
                if ((!atoi(a) || ch + 1u == (uint32_t)atoi(a)) && host_non[ch][nt]) {
                    if (!k) printf("  mohang: ch %u note %u on\n", (unsigned)ch + 1u, (unsigned)nt);
                    k++;
                }
        printf("%s mohang %s (%u on)\n", k ? "FAIL" : "  ok", a, (unsigned)k);
        return 1;
    }
    if (!strcmp(cmd, "mochis")) {        /* (1.19.2) a check: note-ons out on channel a (1..16) since moclr: at least b
                                          * (b 0: none); FAIL else */
        const uint32_t c = (uint32_t)(atoi(a) - 1) & 15u, n = host_mon[c], w = (uint32_t)atoi(b);
        printf("%s mochis ch %s %u (want %s%s)\n", (w ? n < w : n != 0) ? "FAIL" : "  ok", a, (unsigned)n, w ? ">= " : "",
               b);
        return 1;
    }
    if (!strcmp(cmd, "seta")) {          /* (1.19.3) SEQ A: pattern a (0 NONE, else 1..32); b given: the drums' pattern
                                          * (0 NONE, else 1..16) */
        int p = atoi(a);
#ifdef HYB_MULTI
        const uint8_t mi = sq_mi;
        sq_mi = 0;
#endif
        if (p) { set.sq_pat = (uint8_t)(p - 1); set.sq_pad &= (uint8_t)~SQ_DISARM; sq_load(set.sq_pat); }
        else set.sq_pad |= SQ_DISARM;
#ifdef HYB_MULTI
        sq_mi = mi;
#endif
        if (b[0] && atoi(b)) { set.dr_pat = (uint8_t)(atoi(b) - 1); set.sq_pad &= (uint8_t)~DR_DISARM; dr_load(set.dr_pat); }
        else if (b[0]) set.sq_pad |= DR_DISARM;
        return 1;
    }
    if (!strcmp(cmd, "setb")) {          /* SEQ B: pattern a (0 NONE, else 1..32), split b */
#ifdef HYB_MULTI
        int p = atoi(a);
        if (p) { set.sqb_pat = (uint8_t)(p - 1); set.sqb_pad = 0; sq_mi = 1; sq_load(set.sqb_pat); sq_mi = 0; }
        else set.sqb_pad = SQ_DISARM;
        if (b[0]) { set.mx_split = (uint8_t)atoi(b); mx_apply(); }
#endif
        return 1;
    }
    if (!strcmp(cmd, "lev")) {
#ifdef DR_LMAX
        printf("  lev %d drv %d on %d mk %.3f gain %.4f\n", set.dr_lev, drk.drv, dv.on, (double)dv.mk, (double)dr.gain);
#endif
        return 1;
    }
    if (!strcmp(cmd, "pchk")) {          /* each part's engine patch against the panel's (A, B): bytes differing */
#ifdef HYB_MULTI
        const uint8_t *pa = ed_part ? shadow_o : shadow, *pb = ed_part ? shadow : shadow_o;
        int k, da = 0, db = 0;
        char na[24], nb[24];
        for (k = 0; k < 256; k++) {
            if (k == 129 || k == 144) continue;
            if (S.patch[k] != pa[k]) { da++; printf("  A byte %d eng %d panel %d\n", k, S.patch[k], pa[k]); }
            if (SB.patch[k] != pb[k]) { db++; printf("  B byte %d eng %d panel %d\n", k, SB.patch[k], pb[k]); }
        }
        patch_label(na, ed_part ? patch_o : set.patch, pa);
        patch_label(nb, ed_part ? set.patch : patch_o, pb);
        printf("  pchk ed %d A '%s' diff %d  B '%s' diff %d  req %d\n", ed_part, na, da, nb, db, (int)patch_req);
#endif
        return 1;
    }
    if (!strcmp(cmd, "arps")) {          /* each part's arpeggiator: mode, keys held, the note it plays; voices gated */
#ifdef HYB_MULTI
        int k, i;
        sy_synth *P[2] = {&S, &SB};
        printf("  arps");
        for (i = 0; i < 2; i++) {
            printf("  %c mode %d pat %d oct %d i %d t %.3f n %d note %d on %d v[", (char)(65 + i), P[i]->patch[129], P[i]->patch[128 + 2], P[i]->patch[128 + 3], P[i]->arp_i, (double)P[i]->arp_t, P[i]->arp_n, P[i]->arp_note, P[i]->arp_on);
            for (k = 0; k < P[i]->nv; k++) if (P[i]->v[k].gate) printf(" %d", (int)(P[i]->v[k].target + 0.5f));
            printf(" ]");
        }
        printf("\n");
#endif
        return 1;
    }
    if (!strcmp(cmd, "iso")) {           /* each part's patch number, its bytes against the bank's, its engine's */
#ifdef HYB_MULTI
        const uint8_t *pa = ed_part ? shadow_o : shadow, *pb = ed_part ? shadow : shadow_o;
        const uint32_t na = ed_part ? patch_o : set.patch, nb = ed_part ? set.patch : patch_o;
        const uint8_t *ba = patch_bytes(na);
        int k, da = 0, db = 0, ea = 0, eb = 0;
        for (k = 0; k < 240; k++) { if (pa[k] != ba[k]) da++; }
        ba = patch_bytes(nb);
        for (k = 0; k < 240; k++) { if (pb[k] != ba[k]) db++; }
        for (k = 0; k < 240; k++) { if (k == 129 || k == 144) continue; if (S.patch[k] != pa[k]) ea++; if (SB.patch[k] != pb[k]) eb++; }
        printf("  iso ed %d A %u edits %d eng %d  B %u edits %d eng %d  em %u\n", ed_part, na, da, ea, nb, db, eb, em_n);
#endif
        return 1;
    }
    if (!strcmp(cmd, "chb")) {
#ifdef HYB_MULTI
        set.mx_chb = (uint8_t)atoi(a); mx_apply();
#endif
        return 1;
    }
    if (!strcmp(cmd, "arpb")) {          /* part B's patch byte 129 (arp) to a, through the parameter path */
#ifdef HYB_MULTI
        if (ed_part) shadow[129] = (uint8_t)atoi(a); else shadow_o[129] = (uint8_t)atoi(a);
        ev_put(EV_PARAM_B, 129u, (uint32_t)atoi(a));
#endif
        return 1;
    }
    if (!strcmp(cmd, "arpa")) {
#ifdef HYB_MULTI
        if (ed_part) shadow_o[129] = (uint8_t)atoi(a); else shadow[129] = (uint8_t)atoi(a);
        ev_put(EV_PARAM_A, 129u, (uint32_t)atoi(a));
#endif
        return 1;
    }
    if (!strcmp(cmd, "stw")) { printf("  stw now %u store_t %u busy %u writes %u skipped %u auto %u\n", plat_ms(), store_t, st_busy_t, store_n[0], store_n[1], set.md_pad & MD_AUTO); return 1; }
    if (!strcmp(cmd, "autosave")) { set.md_pad = (uint8_t)(atoi(a) ? set.md_pad | MD_AUTO : set.md_pad & ~MD_AUTO); return 1; }
    if (!strcmp(cmd, "edzero")) {       /* before "reboot": the RAM a power cycle loses (the part edited) */
#ifdef HYB_MULTI
        ed_part = 0; u_theme(0); sq_mi = 0;
#endif
        sqv.last = 0;                    /* (SEQ's last page: its way's start again, 0.77) */
#ifdef HYB_MULTI
        sq_retv = 0;                     /* (the launcher as the last view: gone too) */
#endif
        un.n = 0;                        /* (1.18C33: the journal and the marked patterns' copies too: RAM) */
        un.key = 0;
        pk.m = 0;
        pk.ask = pk.dres = pk.ha = 0;
        {
            uint32_t i;
            for (i = 0; i < PK_NC; i++) pk.ck[i] = 0;
        }
        return 1;
    }
    if (!strcmp(cmd, "seqlast")) { sqv.last = (uint8_t)atoi(a); return 1; }   /* SEQ's last page (0.77): 0 none (its
                                          * way's start), 1 the arp sequencer's, 2 the drums' */
    if (!strcmp(cmd, "sendb")) {
#ifdef HYB_MULTI
        set.mx_send = (uint8_t)atoi(a); mx_apply();
#endif
        return 1;
    }
    if (!strcmp(cmd, "sqlog")) { sq_log = atoi(a); return 1; }   /* the arp sequencers' notes printed (1) or not */
    if (!strcmp(cmd, "sqlset")) {        /* (1.19.3) arp sequencer a's (0 A, 1 B) pattern in RAM: step b's (1..64) LENGTH
                                          * index c (0 .. SQ_LTOP; 17: a bar), not saved */
#ifdef HYB_MULTI
        sq_step_t *t = &sqpa[atoi(a) & 1].st[(uint32_t)(atoi(b) - 1) & 63u];
#else
        sq_step_t *t = &sqp.st[(uint32_t)(atoi(b) - 1) & 63u];
#endif
        t->la = (uint8_t)((t->la & ~31u) | ((uint32_t)atoi(c) & 31u));
        return 1;
    }
    if (!strcmp(cmd, "sqlen")) {         /* step a's (1..64) LENGTH: its index, its ticks; b: the index wanted (FAIL
                                          * else) */
        const uint32_t k = (uint32_t)atoi(a) - 1u, l = sqp.st[k & 63u].la & 31u;
        printf("%s sqlen step %s la %u ticks %u steps %u%s%s\n", b[0] && (uint32_t)atoi(b) != l ? "FAIL" : "  ok", a,
               (unsigned)l, (unsigned)SQ_LEN(l), (unsigned)(SQ_LEN(l) / 8u), b[0] ? " want " : "", b);
        return 1;
    }
    if (!strcmp(cmd, "sqlive")) {        /* a check: the arp sequencer's note a sounding (b 1) or not (b 0); FAIL
                                          * else */
        uint32_t i, on = 0;
        for (i = 0; i < SQ_NT; i++)
            if (sq.nt[i].live && sq.nt[i].note == (uint32_t)atoi(a)) on = 1;
        printf("%s sqlive %s %u (step %u)\n", on != (uint32_t)atoi(b) ? "FAIL" : "  ok", a, (unsigned)on,
               (unsigned)sq.step);
        return 1;
    }
    if (!strcmp(cmd, "lcdis")) {         /* a check: a sequencer's page's strip (no knob turned, no list up) reads a and
                                          * b, "_" for a space ("lcdis ARP_SEQ P1_STOP_1x"); FAIL else */
        char x[24] = "", y[24] = "", wa[24] = "", wb[24] = "";
        uint32_t k;
        for (k = 0; k < 23u && a[k]; k++) wa[k] = a[k] == '_' ? ' ' : a[k];
        for (k = 0; k < 23u && b[k]; k++) wb[k] = b[k] == '_' ? ' ' : b[k];
        if (ui.view == V_KIT) kit_lcd(x, y); else sq_lcd(x, y);   /* (0.98:
                                          * the kit's page too) */
        printf("%s lcdis \"%s\" \"%s\"\n", strcmp(x, wa) || strcmp(y, wb) ? "FAIL" : "  ok", x, y);
        return 1;
    }
    if (!strcmp(cmd, "rlis")) {          /* a check (0.79): a sequencer's roll shown (a 1) or its page whole (a 0: every
                                          * part drawn); b "new": drawn again since the last rlis, "same": not */
        static uint32_t last;
        const uint32_t np = ui.view == V_DRUM ? DR_NPART : SQ_NPART;
        uint32_t k, bad = (RL_SIG != 0) != (uint32_t)atoi(a);
        if (!atoi(a))
            for (k = 0; k < np; k++) bad |= !SQ_PART[k];
        if (b && !strcmp(b, "new")) bad |= RL_SIG == last;
        if (b && !strcmp(b, "same")) bad |= RL_SIG != last;
        printf("%s rlis %s %s (sig %08x, was %08x)\n", bad ? "FAIL" : "  ok", a, b ? b : "", (unsigned)RL_SIG,
               (unsigned)last);
        last = RL_SIG;
        return 1;
    }
    if (!strcmp(cmd, "sqstep")) {        /* a check: the shown arp pattern's step a (1..64) a NOTE at offset b, velocity
                                          * c at least (FAIL else) */
        const sq_step_t *t = &sqp.st[(uint32_t)(atoi(a) - 1) & 63u];
        const int ok = (t->ev & SQ_ON) && (t->ev & 15u) == SQ_NOTE && t->off == atoi(b) && t->vel >= atoi(c);
        printf("%s sqstep %s ev %d off %d vel %d len %d\n", ok ? "  ok" : "FAIL", a, t->ev, t->off, t->vel, t->la & 31u);
        return 1;
    }
    if (!strcmp(cmd, "parp")) {          /* PATCH ARP: the pattern's bit, the hold, the engine's and the panel's B 1;
                                          * expect a: the engine's B 1 nonzero (1) or 0 (FAIL else) */
#ifdef HYB_MULTI
        const int eng = S.patch[129], hold = sqr[0].hold;
#else
        const int eng = S.patch[129], hold = sq.hold;
#endif
        printf("%s parp pad %d hold %d engine B1 %d panel B1 %d\n",
               a[0] && (atoi(a) != 0) != (eng != 0) ? "FAIL" : "  ok", sqp.pad, hold, eng, shadow[129]);
        return 1;
    }
    if (!strcmp(cmd, "arpset")) {        /* the patch's arpeggiator (B 1) to a, as an edit through the parameter path */
#ifdef HYB_MULTI
        shadow[129] = (uint8_t)atoi(a);
        ev_put(EV_PARAM_A, 129u, (uint32_t)atoi(a));
#else
        shadow[129] = (uint8_t)atoi(a);
        ev_put(EV_PARAM, 129u, (uint32_t)atoi(a));
#endif
        return 1;
    }
    if (!strcmp(cmd, "latis")) {         /* KEY LATCH, a check: arp sequencer a's (0 A, 1 B) notes latched: the lowest
                                          * is b (0 none; FAIL else); and the BASE column of the one shown */
        const uint32_t i = (uint32_t)atoi(a);
        uint32_t k, lo = 0, cnt = 0, mx, bi;
        char t[16];
#ifdef HYB_MULTI
        sq_rt_t *q = &sqr[i & 1u];
#else
        sq_rt_t *q = &sq;
        if (i) { printf("  ok latis %u: one arp sequencer here\n", (unsigned)i); return 1; }
#endif
        for (k = 128u; k--;) if (q->lat[k >> 5] >> (k & 31u) & 1u) { lo = k; cnt++; }
        if (!cnt) lo = 0;
        sq_cv(11u, t, &mx, &bi);
        printf("%s latis %u lowest %u notes %u ldn %u run %u base col \"%s\"\n", lo != (uint32_t)atoi(b) ? "FAIL" : "  ok",
               (unsigned)i, (unsigned)lo, (unsigned)cnt, q->ldn, q->run, t);
        return 1;
    }
    if (!strcmp(cmd, "sqlast")) {        /* a check: arp sequencer a's last note on was b (FAIL else) */
        const uint32_t i = (uint32_t)atoi(a) & 1u;
#ifndef HYB_MULTI
        if (i) { printf("  ok sqlast %u: one arp sequencer here\n", (unsigned)i); return 1; }
#endif
        printf("%s sqlast %u note %u\n", sq_lastn[i] != (uint32_t)atoi(b) ? "FAIL" : "  ok", (unsigned)i,
               (unsigned)sq_lastn[i]);
        return 1;
    }
    if (!strcmp(cmd, "sqlastat")) {      /* a check (0.78): arp sequencer a's last note on was at step b (1..64) + c
                                          * eighths (FAIL else) */
        const uint32_t i = (uint32_t)atoi(a) & 1u, w = (uint32_t)(atoi(b) - 1) * 8u + (uint32_t)atoi(c);
        printf("%s sqlastat %u step %u + %u/8\n", sq_lastt[i] != w ? "FAIL" : "  ok", (unsigned)i,
               (unsigned)(sq_lastt[i] >> 3) + 1u, (unsigned)(sq_lastt[i] & 7u));
        return 1;
    }
    /* (1.18C33) the favorites (app.c fv_*) */
    if (!strcmp(cmd, "ubput")) {         /* ubput TAG NAME [V]: slot TAG ("C001": the first user bank, shown A001) saved as
                                          * SAVE does with the init patch named NAME, its cutoff (byte 40) V (default 64) */
        uint8_t sh[SH_LEN];
        uint32_t k;
        for (k = 0; k < 256u; k++) sh[k] = INIT_PATCH[k];
#ifdef HYB_SHIMMER
        for (k = 0; k < SHX_N; k++) sh[256u + k] = SHX_DEF[k];
        sh[256u + SHX_GL] = SHX_GDEF;
#endif
        for (k = 0; k < 10u; k++) sh[240u + k] = (uint8_t)(b[k] && k < strlen(b) ? b[k] : ' ');
        sh[40] = (uint8_t)(c[0] ? atoi(c) : 64);
        printf("%s ubput %s %s\n", ub_write(host_tag(a) - PU_0, sh) ? "FAIL" : "  ok", a, b);
        return 1;
    }
    if (!strcmp(cmd, "fvis")) {          /* a check: the favorites in their order are a ("C001,C005" as host tags, "-"
                                          * none); FAIL else */
        char w[160] = "", t[8];
        uint32_t k;
        for (k = 0; k < fv.n; k++) {
            const uint32_t q = fv.p[k];
            snprintf(t, sizeof t, "%c%03u", (char)('A' + q / 128u), (unsigned)(q % 128u + 1u));
            if (k) strcat(w, ",");
            strcat(w, t);
        }
        if (!fv.n) strcpy(w, "-");
        printf("%s fvis %s (%s)\n", strcmp(w, a) ? "FAIL" : "  ok", a, w);
        return 1;
    }
    if (!strcmp(cmd, "fvst")) {          /* a check: the list's section line a (fv.sec - 1: 0 EDIT FAVORITES, n favorite 1;
                                          * "-" not in it), the editor up b, its cursor c ("-": not checked), d the
                                          * removal asked (0 / 1, given) */
        const int sec = fv.sec ? (int)fv.sec - 1 : -1;
        const int bad = (strcmp(a, "-") ? sec != atoi(a) : sec != -1) || (b[0] && fv.ed != atoi(b))
                        || (c[0] && strcmp(c, "-") && fv.ei != atoi(c)) || (d[0] && (fv.ask != 0) != atoi(d));
        printf("%s fvst %s %s %s %s (sec %d ed %d ei %d ask %d aud %d)\n", bad ? "FAIL" : "  ok", a, b, c, d, sec,
               fv.ed, fv.ei, fv.ask, fv_aud);
        return 1;
    }
    if (!strcmp(cmd, "fvdisk")) {        /* a check: the store's record holds a favorites (-1: none), b pending writes
                                          * (fv.wr != 0: 1, 0; given) */
        uint8_t r[FV_REC];
        const int n = plat_store_read(OBJ_LSET, FV_OFF, r, FV_REC) == (int)FV_REC && r[0] == FV_M0 && r[1] == FV_M1 ? r[3] : -1;
        const int bad = n != atoi(a) || (b[0] && (fv.wr != 0) != atoi(b));
        printf("%s fvdisk %s %s (%d, wr %d, size %d)\n", bad ? "FAIL" : "  ok", a, b, n, fv.wr, plat_store_size(OBJ_LSET));
        return 1;
    }
    if (!strcmp(cmd, "liis")) {          /* a check: the list up's line a (from 0) reads b ("_" a space; its start), its
                                          * prefix c (given; "." none), a star (d "*") or none (d "-", given) */
        char t[40] = "", pre[6] = "", w[40] = "";
        uint32_t fl = 0, k;
        int bad;
        for (k = 0; k < 39u && b[k]; k++) w[k] = b[k] == '_' ? ' ' : b[k];
        if (!list_up() || (uint32_t)atoi(a) >= ls.n) { printf("FAIL liis %s: no such line\n", a); return 1; }
        ls.item((uint32_t)atoi(a), t, pre, &fl);
        bad = strncmp(t, w, strlen(w)) || (c[0] && strcmp(c[0] == '.' ? "" : c, pre)) || (d[0] && (d[0] == '*') != !!(fl & LS_STAR));
        printf("%s liis %s \"%s\" (\"%s\" pre \"%s\" star %d)\n", bad ? "FAIL" : "  ok", a, w, t, pre, !!(fl & LS_STAR));
        return 1;
    }
    if (!strcmp(cmd, "fvmute")) {        /* a check: the favorites editor's audition (a 1): part A heard, part B and the
                                          * drums muted; or not (0): as the launcher's mutes have them */
        const int on = fv_aud != 0;
#ifdef HYB_MULTI
        const int bad = on != atoi(a) || (on && (SB.rt_g > 1e-4f || S.rt_g < 1e-4f || dr.gain != 0.f));
        printf("%s fvmute %s (aud %d A %.3f B %.3f drums %.4f)\n", bad ? "FAIL" : "  ok", a, on, S.rt_g, SB.rt_g, dr.gain);
#else
        const int bad = on != atoi(a) || (on && dr.gain != 0.f);
        printf("%s fvmute %s (aud %d drums %.4f)\n", bad ? "FAIL" : "  ok", a, on, dr.gain);
#endif
        return 1;
    }
    if (!strcmp(cmd, "engis")) {         /* a check: part A's engine plays slot a as stored (its bytes but the tempo and
                                          * the arpeggiator's switch, 144, 129) (b 1) or not (0); FAIL else */
        const uint8_t *q = patch_bytes(host_tag(a));
        uint32_t k, same = 1;
        for (k = 0; k < 256u; k++)
            if (k != TP_BYTE && k != 129u && S.patch[k] != q[k]) same = 0;
        printf("%s engis %s %s (%u)\n", (int)same != atoi(b) ? "FAIL" : "  ok", a, b, same);
        return 1;
    }
    if (!strcmp(cmd, "stsnap") || !strcmp(cmd, "stsame")) {   /* the state the favorites editor must leave as it was:
                                          * both parts' engine bytes, both panel buffers, the patches, the edit memory,
                                          * the SysEx states, undo's depth, HOLD, the modes, the mutes, the octave
                                          * (stsnap keeps it, stsame compares: FAIL with what differs) */
        static uint8_t ea[256], eb[256], sa[SH_LEN], so[SH_LEN], emk[sizeof em];
        static uint32_t v[16];
        uint32_t w[16], k, n = 0;
        char bad[200] = "";
#ifdef HYB_MULTI
        w[n++] = patch_o; w[n++] = sx_o; w[n++] = hold_o; w[n++] = set.mx_mode; w[n++] = ed_part; w[n++] = ln_mute;
        w[n++] = (uint32_t)sqr[0].run | (uint32_t)sqr[1].run << 1;
#endif
        w[n++] = set.patch; w[n++] = sx_edit; w[n++] = hold_on; w[n++] = em_n; w[n++] = undo_n; w[n++] = (uint32_t)set.oct;
        w[n++] = (uint32_t)dr.run | (uint32_t)SQ_RUNS << 1;
        if (cmd[3] == 'n') {
            for (k = 0; k < 256u; k++) { ea[k] = S.patch[k];
#ifdef HYB_MULTI
                eb[k] = SB.patch[k];
#endif
            }
            for (k = 0; k < SH_LEN; k++) { sa[k] = shadow[k];
#ifdef HYB_MULTI
                so[k] = shadow_o[k];
#endif
            }
            memcpy(emk, em, sizeof em);
            for (k = 0; k < n; k++) v[k] = w[k];
            printf("  stsnap\n");
            return 1;
        }
        for (k = 0; k < 256u; k++) if (S.patch[k] != ea[k]) { snprintf(bad + strlen(bad), 40, " engA[%u] %u/%u", (unsigned)k, ea[k], S.patch[k]); break; }
#ifdef HYB_MULTI
        for (k = 0; k < 256u; k++) if (SB.patch[k] != eb[k]) { snprintf(bad + strlen(bad), 40, " engB[%u]", (unsigned)k); break; }
        for (k = 0; k < SH_LEN; k++) if (shadow_o[k] != so[k]) { snprintf(bad + strlen(bad), 40, " shadow_o[%u]", (unsigned)k); break; }
#endif
        for (k = 0; k < SH_LEN; k++) if (shadow[k] != sa[k]) { snprintf(bad + strlen(bad), 40, " shadow[%u]", (unsigned)k); break; }
        if (memcmp(emk, em, sizeof em)) strcat(bad, " em");
        for (k = 0; k < n; k++) if (w[k] != v[k]) { snprintf(bad + strlen(bad), 40, " state%u %u/%u", (unsigned)k, (unsigned)v[k], (unsigned)w[k]); }
        printf("%s stsame%s\n", bad[0] ? "FAIL" : "  ok", bad);
        return 1;
    }
    if (!strcmp(cmd, "patchis")) {       /* a check (0.78): the patch edited is a ("B005", "INIT"), the octave b, the patch
                                          * list up (c 1) or not (FAIL else) */
        char t[8];
        const int up = ui.plist_until != 0;
        patch_tag(t, set.patch);
        printf("%s patchis %s oct %d list %d\n", strcmp(t, a) || set.oct != atoi(b) || up != atoi(c) ? "FAIL" : "  ok", t,
               set.oct, up);
        return 1;
    }
    if (!strcmp(cmd, "plis")) {          /* a check (0.96): the patch list up with its cursor on place a (INIT 0), "RESET
                                          * PATCH ?" asked (b 1) or not (0), c places (given: INIT and the bank's
                                          * patches); FAIL else */
        const int up = list_up() && ls.item == pl_item;
        const int ask = rp_on();
        const int cur = (int)ls.cur - (int)PL_INIT, n = (int)ls.n - (int)PL_INIT;   /* (after 0.98: RANDOMIZE the place
                                          * -1; 1.18C33: the favorites' section before it) */
        const int bad = !up || cur != atoi(a) || ask != atoi(b) || (c[0] && n != atoi(c));
        printf("%s plis %s %s %s (up %d cur %d ask %d n %d)\n", bad ? "FAIL" : "  ok", a, b, c, up, cur, ask, n);
        return 1;
    }
    if (!strcmp(cmd, "qis")) {           /* a check (0.96): sequencer a's (0 A, 1 B, 2 the drums) pattern waiting for the
                                          * line is b (0 none, n pattern n, -1 NONE), its request c (SQ_QR_*), its
                                          * pattern now d (1.., 0 NONE; given), running e (given); FAIL else */
        const uint32_t q = (uint32_t)atoi(a) % 3u, t = sq_qt[q];
        const int tw = t == SQ_QNONE ? -1 : (int)t;
#ifdef HYB_MULTI
        const int pn = q == 2u ? DR_PATSH + 1 : q ? (set.sqb_pad & SQ_DISARM ? 0 : set.sqb_pat + 1)
                                                  : (set.sq_pad & SQ_DISARM ? 0 : set.sq_pat + 1);
#else
        const int pn = q == 2u ? DR_PATSH + 1 : (set.sq_pad & SQ_DISARM ? 0 : set.sq_pat + 1);
#endif
        const int run = sq_qrun(q);
        const int bad = tw != atoi(b) || sq_qr[q] != atoi(c) || (d[0] && pn != atoi(d));
        printf("%s qis %u target %d request %u state %u loaded %u pattern %d run %d\n", bad ? "FAIL" : "  ok",
               (unsigned)q, tw, (unsigned)sq_qr[q], (unsigned)sq_qs[q], (unsigned)sq_qm[q], pn, run);
        return 1;
    }
    if (!strcmp(cmd, "turnlate")) {      /* (1.18C21) encoder a (SELECT, K1..K4) turned by b detents that come in just after
                                          * the next read of it (the launcher's ln_pre), for the read after (input's): the
                                          * device's race of an interrupt's detent between the two */
        static const char *const N[NE] = {"SELECT", "ALGO", "PRESET", "K1", "K2", "K3", "K4"};
        int r;
        for (r = 0; r < NE; r++) if (!strcmp(a, N[r])) { host_late_n[r] += atoi(b); host_late_s[r] = 1; }
        return 1;
    }
    if (!strcmp(cmd, "lnset")) {         /* (1.18C10) the launcher's row a (1..16): SYNTH 1's pattern b, SYNTH 2's c, the
                                          * drums' d (0 none, else 1..) */
#ifdef HYB_MULTI
        const uint32_t r = (uint32_t)(atoi(a) - 1) % LN_ROWS;
        set.ln[r][0] = (uint8_t)atoi(b); set.ln[r][1] = (uint8_t)atoi(c); set.ln[r][2] = (uint8_t)atoi(d);
        if (set.ln[r][3] == LN_NO && (set.ln[r][0] | set.ln[r][1] | set.ln[r][2])) set.ln[r][3] = LN_REP;   /* (1.18C18: as the knobs do) */
#endif
        return 1;
    }
    if (!strcmp(cmd, "lnmd")) {          /* (1.18C10) row a's mode b (0 REP, 1 1x, 2 2x, 3 4x, 4 8x, 5 NO); c "is": a
                                          * check of it instead (FAIL else) */
#ifdef HYB_MULTI
        const uint32_t r = (uint32_t)(atoi(a) - 1) % LN_ROWS;
        const int m = set.ln[r][3] == LN_NO ? 5 : (int)set.ln[r][3] - 1;   /* (the scripts' numbers: 0 REP .. 4 8x, 5 NO; 1.18C18 stores NO first) */
        if (!strcmp(c, "is")) printf("%s lnmd %u %d\n", m != atoi(b) ? "FAIL" : "  ok", (unsigned)r + 1u, m);
        else set.ln[r][3] = (uint8_t)(atoi(b) == 5 ? LN_NO : atoi(b) + 1);
#endif
        return 1;
    }
    if (!strcmp(cmd, "lnsq")) {          /* (1.18C10) arp pattern a (1..32) stored as shipped with LENGTH b (0..2: 16, 32,
                                          * 64 steps), time division c (0..4); stopped */
        sq_pat_t p;
        const uint32_t n = (uint32_t)(atoi(a) - 1) % SQ_PATS;
        sq_defaults(&p, 0);
        p.lc = (uint8_t)((uint32_t)atoi(b) % SQ_NLC);
        p.div = (uint8_t)((uint32_t)atoi(c) % SQ_NDIV);
        p.mark = SQ_MARK;
        if (sq_store(&p, n, 3u) != 0) printf("FAIL lnsq %u: not stored\n", (unsigned)n + 1u);
        return 1;
    }
    if (!strcmp(cmd, "lnis")) {          /* (1.18C10) a check: the launcher's row playing a (1..16, 0 none), the one
                                          * waiting b (given), the row selected c (given), a list up d (0 none, given) */
#ifdef HYB_MULTI
        const int bad = ln.play != atoi(a) || (b[0] && ln.next != atoi(b)) || (c[0] && ln.sel + 1 != atoi(c))
                        || (d[0] && atoi(d));   /* (no list any more: d is 0) */
        printf("%s lnis play %d next %d sel %d r0 %u len %u sw %u mt %u view %d\n", bad ? "FAIL" : "  ok",
               ln.play, ln.next, ln.sel + 1, (unsigned)ln.r0, (unsigned)ln.len, (unsigned)ln.sw,
               (unsigned)ck.mt, ui.view);
#endif
        return 1;
    }
    if (!strcmp(cmd, "lnlen")) {         /* (1.18C15) a check: the row playing lasts a ticks (the longest of its patterns, in its own
                                          * LENGTH and time division, times its mode's repetitions; set once the row runs) */
#ifdef HYB_MULTI
        const uint32_t d = ln.sw - ln.r0;
        printf("%s lnlen %u (want %s) play %d next %d\n", d != (uint32_t)atoi(a) ? "FAIL" : "  ok", (unsigned)d, a, ln.play, ln.next);
#endif
        return 1;
    }
    if (!strcmp(cmd, "lntr")) { ln_tr = atoi(a); return 1; }   /* (after 1.18C14) the launcher's trace on (1) or off */
    if (!strcmp(cmd, "lncyc")) {         /* (1.18C11) SEQ's round with (1) or without (0, the other scripts' default) the launcher */
#ifdef HYB_MULTI
        ln_nocyc = (uint8_t)!atoi(a);
#endif
        return 1;
    }
    if (!strcmp(cmd, "batt")) {          /* the battery level plat_battery gives (-1 .. 4: not read, empty .. full, USB) */
        host_batt_v = clamp(atoi(a), -1, 4);
        return 1;
    }
    if (!strcmp(cmd, "lnbat")) {         /* a check: the launcher's battery cell shows the level a (signature: 1<<29 | level + 2) */
#ifdef HYB_MULTI
        const uint32_t g = ln_sg[LN_SHOW * 5u + 4u], want = 1u << 29 | (uint32_t)(atoi(a) + 2);
        printf("%s lnbat %d\n", g == want ? "  ok" : "FAIL", (int)(g & 0xFFu) - 2);
#endif
        return 1;
    }
    if (!strcmp(cmd, "lnbpm")) {         /* a check: the launcher's tempo cell shows a BPM */
#ifdef HYB_MULTI
        const uint32_t g = ln_sg[LN_SHOW * 5u + 3u];
        printf("%s lnbpm %d\n", g == (1u << 29 | (uint32_t)atoi(a)) ? "  ok" : "FAIL", (int)(g & 0xFFFFu));
#endif
        return 1;
    }
    if (!strcmp(cmd, "lncnt")) {         /* (1.18C11) a check: the launcher's cells drawn since the last call: a (given) */
#ifdef HYB_MULTI
        static uint32_t base;
        const int bad = a[0] && (int)(ln_nd - base) != atoi(a);
        printf("%s lncnt %d\n", bad ? "FAIL" : "  ok", (int)(ln_nd - base));
        base = ln_nd;
#endif
        return 1;
    }
    if (!strcmp(cmd, "lnpat")) {         /* (1.18C10) a check: the parts' patterns playing (or waited for): SYNTH 1 a,
                                          * SYNTH 2 b, the drums c (0 NONE, else 1..; "-" not checked), each running */
#ifdef HYB_MULTI
        const char *w[3] = {a, b, c};
        int bad = 0;
        uint32_t q;
        for (q = 0; q < 3u; q++) if (w[q][0] && w[q][0] != '-' && ln_cur(q) + 1 != atoi(w[q])) bad = 1;
        printf("%s lnpat %d %d %d run %d %d %d\n", bad ? "FAIL" : "  ok", ln_cur(0) + 1, ln_cur(1) + 1, ln_cur(2) + 1,
               sq_qrun(0), sq_qrun(1), sq_qrun(2));
#endif
        return 1;
    }
    if (!strcmp(cmd, "lnseq")) {         /* (1.18C16) a check: the sequencers' own current pattern numbers (SEQ A, SEQ B, the drums:
                                          * 0 NONE, else 1..; "-" not checked) as their pages show them (set.*_pat, DISARM) */
#ifdef HYB_MULTI
        const char *w[3] = {a, b, c};
        const int v[3] = {set.sq_pad & SQ_DISARM ? 0 : set.sq_pat + 1, set.sqb_pad & SQ_DISARM ? 0 : set.sqb_pat + 1,
                          set.sq_pad & DR_DISARM ? 0 : set.dr_pat + 1};
        int bad = 0, q;
        for (q = 0; q < 3; q++) if (w[q][0] && w[q][0] != '-' && v[q] != atoi(w[q])) bad = 1;
        printf("%s lnseq %d %d %d\n", bad ? "FAIL" : "  ok", v[0], v[1], v[2]);
#endif
        return 1;
    }
    if (!strcmp(cmd, "lncol")) {         /* (1.18C16) a check: the mode cell of the row shown w lines down (0 selected) is drawn as playing (a 1) or not */
#ifdef HYB_MULTI
        const int on = (int)(ln_sigc((uint32_t)atoi(a), 4u) >> 8 & 1u);
        printf("%s lncol %d (want %s)\n", on != atoi(b) ? "FAIL" : "  ok", on, b);
#endif
        return 1;
    }
    if (!strcmp(cmd, "lnep")) {          /* (1.18C21) a check: the part edited as the launcher shows it a (0 SYNTH 1, 1 SYNTH 2, 2 the
                                          * drums), its knob's background drawn so (after a frame) */
#ifdef HYB_MULTI
        uint32_t c, g = 0;
        for (c = 0; c < 3u; c++) g |= (ln_sg[LN_SHOW * 5u + c] >> 11 & 1u) << c;
        printf("%s lnep %u knobs %u (want %s)\n", LN_EP != (uint32_t)atoi(a) || g != 1u << atoi(a) ? "FAIL" : "  ok",
               (unsigned)LN_EP, (unsigned)g, a);
#endif
        return 1;
    }
    if (!strcmp(cmd, "lnmidi")) {        /* (1.19.3) a check: the launcher's SYNTH 2 column drawn as SEQ B's MIDI-only one
                                          * (SINGLE: grey cells, "MIDI" for its knob) a 1, or as part B's (a 0); FAIL else */
#ifdef HYB_MULTI
        const int m = (int)(ln_sigc(0u, 2u) >> 13 & 1u);
        printf("%s lnmidi %d (want %s)\n", m != atoi(a) ? "FAIL" : "  ok", m, a);
#endif
        return 1;
    }
    if (!strcmp(cmd, "lngrey")) {        /* (1.18C21) a check: the selected row's cells of the parts muted are drawn light grey: the
                                          * columns a (bits, as lnvol's) */
#ifdef HYB_MULTI
        uint32_t c, g = 0;
        for (c = 0; c < 3u; c++) g |= (ln_sigc(0u, c + 1u) >> 14 & 1u) << c;
        printf("%s lngrey %u (want %s)\n", g != (uint32_t)atoi(a) ? "FAIL" : "  ok", (unsigned)g, a);
#endif
        return 1;
    }
    if (!strcmp(cmd, "lnok")) {          /* (1.18C18) a check: the sequencers' own current patterns (as lnseq) are the playing row's cells
                                          * (SYNTH 2 left out with one synth; no row playing, or within 64 ticks of its line, or the next row's line within 160 ticks (the target loaded in the bar's last step): ok) */
#ifdef HYB_MULTI
        const int v[3] = {set.sq_pad & SQ_DISARM ? 0 : set.sq_pat + 1, set.sqb_pad & SQ_DISARM ? 0 : set.sqb_pat + 1,
                          set.sq_pad & DR_DISARM ? 0 : set.dr_pat + 1};
        int bad = 0, q;
        if (ln.play && ck.mt - ln.r0 >= 64u && !(ln.next && ln.sw - ck.mt <= 160u)) for (q = 0; q < 3; q++) if (v[q] != set.ln[ln.play - 1u][q]) bad = 1;
        printf("%s lnok row %d seq %d %d %d cur %d %d %d\n", bad ? "FAIL" : "  ok", ln.play, v[0], v[1], v[2],
               ln_cur(0) + 1, ln_cur(1) + 1, ln_cur(2) + 1);
#endif
        return 1;
    }
    if (!strcmp(cmd, "lnedx")) {         /* (1.18C18) cell a (0..2) of a row not playing (the other of rows 1, 2) stepped by b as K1..K3 do it */
#ifdef HYB_MULTI
        const uint8_t s0 = ln.sel;
        ln.sel = (uint8_t)(ln.play == 1u ? 1u : 0u);
        ln_cell_step((uint32_t)atoi(a) % 3u, atoi(b));
        ln.sel = s0;
#endif
        return 1;
    }
    if (!strcmp(cmd, "lnrow")) {         /* (1.18C10) a check: the launcher's row a's patterns b c d (lnset's) */
#ifdef HYB_MULTI
        const uint32_t r = (uint32_t)(atoi(a) - 1) % LN_ROWS;
        const int bad = set.ln[r][0] != atoi(b) || set.ln[r][1] != atoi(c) || set.ln[r][2] != atoi(d);
        printf("%s lnrow %u %d %d %d %d\n", bad ? "FAIL" : "  ok", (unsigned)r + 1u, set.ln[r][0], set.ln[r][1],
               set.ln[r][2], set.ln[r][3]);
#endif
        return 1;
    }
    if (!strcmp(cmd, "kitlev")) {        /* (1.18C27) a check: instrument a's (0 BD ..) LEVEL in the kit is b; the kit
                                          * changed and not saved yet (c 1) or not (0; given) */
        const dr_kp_t *q = &drk.k[atoi(a) % DR_NI];
        printf("%s kitlev %s %d dirty %d\n", q->lev != atoi(b) || (c[0] && (kv.dirty_t != 0) != atoi(c)) ? "FAIL" : "  ok", a,
               q->lev, kv.dirty_t != 0);
        return 1;
    }
    if (!strcmp(cmd, "lsx")) {           /* (1.18C27) the remote editor's sets (remote.c 07 .. 09), as the page sends them: a
                                          * "r" read slot b (1..16; 0: the format), "w" write slot b with slot c's record as
                                          * stored ("x": that record with its magic broken), "d" empty slot b; a check:
                                          * the answer's status is d (FAIL else); the answer printed */
#ifdef HYB_MULTI
        static uint8_t f[600];
        const uint32_t k = atoi(b) ? (uint32_t)atoi(b) - 1u : 0x7Fu;
        uint32_t n = 0, i, st;
        f[n++] = 0xF0; f[n++] = 0x7D; f[n++] = 0x52; f[n++] = 0x45;
        f[n++] = a[0] == 'r' ? 0x07 : a[0] == 'w' ? 0x08 : 0x09;
        f[n++] = (uint8_t)k;
        if (a[0] == 'w') {
            ls_rec_t r;
            uint8_t *q = (uint8_t *)&r;
            if (!ls_read((uint32_t)atoi(c) - 1u, &r)) for (i = 0; i < LS_REC; i++) q[i] = 0;
            if (a[1] == 'x') r.m0 ^= 1u;
            for (i = 0; i < LS_REC + (LS_REC + 6u) / 7u; i++) f[n + i] = 0;
            for (i = 0; i < LS_REC; i++) { f[n + i] = q[i] & 0x7Fu; f[n + LS_REC + i / 7u] |= (uint8_t)(q[i] >> 7 << (i % 7u)); }
            n += LS_REC + (LS_REC + 6u) / 7u;
        }
        f[n++] = 0xF7;
        host_sx_n = 0;
        host_sx_in(f, n);
        st = host_sx_n > 7u ? host_sx_last[6] : 0xFFu;
        printf("%s lsx %s %s -> %02X slot %u status %u (%u B)", (d[0] && st != (uint32_t)atoi(d)) || host_sx_n < 8u ? "FAIL" : "  ok",
               a, b, host_sx_n > 4u ? host_sx_last[4] : 0, host_sx_n > 5u ? host_sx_last[5] : 0, st, host_sx_n);
        if (a[0] == 'r' && k != 0x7Fu && !st && host_sx_n >= 9u + 220u + 8u)
            printf(" tags %.4s %.4s", (const char *)host_sx_last + 9 + 220, (const char *)host_sx_last + 13 + 220);
        if (a[0] == 'r' && k == 0x7Fu) printf(" slots %u version %u record %u", host_sx_last[7], host_sx_last[8], host_sx_last[9] << 7 | host_sx_last[10]);
        printf("\n");
#endif
        return 1;
    }
    if (!strcmp(cmd, "lsedge")) {        /* (1.18C27) SELECT past row 1 / 16 opens the sets' page (1, as built) or stops (0) */
#ifdef HYB_MULTI
        ls_noedge = (uint8_t)!atoi(a);
#endif
        return 1;
    }
    if (!strcmp(cmd, "sqbcyc")) {        /* (1.19.3) SEQ's round in SINGLE with SEQ B (a 1, the firmware's) or without it
                                          * (a 0, as before 1.19.3: the host's default, the older scripts' way round) */
#ifdef HYB_MULTI
        sqb_nocyc = (uint8_t)!atoi(a);
#endif
        return 1;
    }
    if (!strcmp(cmd, "lnopen")) {        /* (1.18C27) the launcher's page (SHIFT + SEQ's round's) at once */
#ifdef HYB_MULTI
        ln_open();
#endif
        return 1;
    }
    if (!strcmp(cmd, "lsis")) {          /* (1.18C27) a check: the sets' page up a (0 no, 1 up; set 2 no longer used), its
                                          * slot b (1..16), action c (0 LOAD 1 SAVE 2 CLEAR 3 DELETE), REC asked d (given) */
#ifdef HYB_MULTI
        const int bad = lset.on != atoi(a) || (b[0] && b[0] != '-' && lset.slot + 1 != atoi(b)) || (c[0] && c[0] != '-' && lset.act != atoi(c))
                        || (d[0] && (lset.ask != 0) != atoi(d));
        printf("%s lsis on %d slot %d act %d ask %d pend %d sel %d\n", bad ? "FAIL" : "  ok", lset.on, lset.slot + 1,
               lset.act, lset.ask, lset.pend, ln.sel + 1);
#endif
        return 1;
    }
    if (!strcmp(cmd, "lsfull")) {        /* (1.18C27) a check: the slots filled, a bit mask (hex), as the store holds them */
#ifdef HYB_MULTI
        uint32_t k, m = 0;
        ls_rec_t r;
        for (k = 0; k < LS_SLOTS; k++) if (ls_read(k, &r)) m |= 1u << k;
        printf("%s lsfull %04x (page %04x)\n", m != (uint32_t)strtoul(a, 0, 16) || ls_full() != m ? "FAIL" : "  ok", m, ls_full());
#endif
        return 1;
    }
    if (!strcmp(cmd, "lsrec")) {         /* (1.18C27) a check: slot a's record: part A's patch b, B's c ("C001"), VOL A d
                                          * (given); its rows and tempo printed */
#ifdef HYB_MULTI
        ls_rec_t r;
        char ta[8] = "-", tb[8] = "-";
        const int ok = ls_read((uint32_t)atoi(a) - 1u, &r);
        if (ok) { patch_tag(ta, (uint32_t)r.pt[0][0] | (uint32_t)r.pt[0][1] << 8); patch_tag(tb, (uint32_t)r.pt[1][0] | (uint32_t)r.pt[1][1] << 8); }
        printf("%s lsrec %s A %s %.10s B %s %.10s vol %d %d %d tempo %d row1 %d %d %d %d kitBD %d\n",
               strcmp(ta, b) || strcmp(tb, c) || (d[0] && (!ok || r.vol[0] != atoi(d))) ? "FAIL" : "  ok", a, ta,
               ok ? r.nm[0] : "", tb, ok ? r.nm[1] : "", ok ? r.vol[0] : -1, ok ? r.vol[1] : -1, ok ? r.dlev : -1,
               ok ? r.tempo : -1, r.ln[0][0], r.ln[0][1], r.ln[0][2], r.ln[0][3], ok ? r.kit.k[0].lev : -1);
#endif
        return 1;
    }
    if (!strcmp(cmd, "uniis")) {         /* (1.18C48) a check: GLO > AUDIO's UNI MAX SINGLE a, UNI MAX MULTI b, MONO UNI MAX c
                                          * (0 OFF, else the voices) as set, the engine's poly cap d (the mode's) and mono
                                          * cap (c); the stored fields too */
#ifdef HYB_MULTI
        const int bad = (int)SET_UNIS != atoi(a) || (int)SET_UNIL != atoi(b) || (int)SET_UNIM != atoi(c)
                        || (d[0] && M.uni_max != atoi(d)) || M.uni_mono != atoi(c);
        printf("%s uniis single %u multi %u mono %u engine %d / %d (mode %d; mx_uni %d mx_unim %d mx_unx %d)\n",
               bad ? "FAIL" : "  ok", (unsigned)SET_UNIS, (unsigned)SET_UNIL, (unsigned)SET_UNIM, M.uni_max, M.uni_mono,
               set.mx_mode, set.mx_uni, set.mx_unim, set.mx_unx);
#endif
        return 1;
    }
    if (!strcmp(cmd, "plrows")) {        /* (1.18C48) a check: PLAY mode's list (pl_build) holds every patch parameter of
                                          * the menus' rows (the matrix's last row, SLOT 6, too), each entry decoding to
                                          * a row whose knob has that byte; the entries, the rows, the highest entry,
                                          * SLOT 6's parameters found */
        uint32_t i, k, r, bad = 0, m6 = 0, top = 0;
        pl_n = 0;
        pl_build();
        for (r = 0; r < U_NROW; r++)
            for (k = 0; k < 4u; k++) {
                const u_param_t *q = &U_ROW[r].p[k];
                int32_t f;
                if (!q->label[0] || P_EXT(q)) continue;
                f = pl_find(q->byte);
                if (f < 0 || pl_p((uint32_t)f)->byte != q->byte) { bad++; printf("  plrows row %u %s %s byte %d not in the list\n", (unsigned)r, U_ROW[r].tag, q->label, q->byte); }
                else if (r == U_NROW - 1u) m6++;
            }
        for (i = 0; i < pl_n; i++) {
            if (pl_tab[i] > top) top = pl_tab[i];
            if (pl_row(pl_tab[i]) >= U_NROW || !pl_p(i)->label[0]) { bad++; printf("  plrows entry %u decodes to nothing\n", (unsigned)i); }
            for (k = 0; k < i; k++) if (pl_p(k)->byte == pl_p(i)->byte) { bad++; printf("  plrows entry %u byte %d twice\n", (unsigned)i, pl_p(i)->byte); }
        }
        printf("%s plrows n %u rows %u top %u slot6 %u\n", bad || !m6 ? "FAIL" : "  ok", (unsigned)pl_n, (unsigned)U_NROW,
               (unsigned)top, (unsigned)m6);
        return 1;
    }
    if (!strcmp(cmd, "pabis")) {         /* (1.18C27) a check: part A plays patch a, part B patch b ("C001", "INIT"), the
                                          * part edited's buffer and the other's names (the engines' bytes too) */
#ifdef HYB_MULTI
        char ta[8], tb[8];
        const uint8_t *ba = ed_part ? shadow_o : shadow, *bb = ed_part ? shadow : shadow_o;
        uint32_t k, e = 0;
        patch_tag(ta, ed_part ? patch_o : set.patch);
        patch_tag(tb, ed_part ? set.patch : patch_o);
        int fk = -1;
        for (k = 0; k < 240u; k++) if (k != TP_BYTE && k != 7u && k != 129u && (S.patch[k] != SHX_EB(k, ba[k]) || SB.patch[k] != SHX_EB(k, bb[k]))) { e++; if (fk < 0) fk = (int)k; }
        printf("%s pabis A %s %.10s B %s %.10s (engines differ %u, first %d)\n", strcmp(ta, a) || strcmp(tb, b) || e ? "FAIL" : "  ok",
               ta, (const char *)ba + 240, tb, (const char *)bb + 240, e, fk);
#endif
        return 1;
    }
    if (!strcmp(cmd, "lnzero")) {        /* (1.18C10, before "reboot") the table in RAM cleared: what loads is the store's */
#ifdef HYB_MULTI
        uint32_t k;
        for (k = 0; k < sizeof set.ln; k++) ((uint8_t *)set.ln)[k] = 0;
        ln.play = ln.next = 0;
        if (a[0] == 'a') { ln.prow = ln.sc0 = 0; lset.on = lset.ask = 0; }   /* (1.18C27, "lnzero all": the active row,
                                          * the parts seen, the sets' page too) */
        sq_retv = 0;                     /* (the launcher as the last view: a power cycle loses it) */
#endif
        return 1;
    }
    if (!strcmp(cmd, "lnvol")) {         /* (1.18C10) a check: the parts muted a (bits), VOL A b, VOL B c, the drums'
                                          * level d (given), their gains */
#ifdef HYB_MULTI
        const int bad = ln_mute != atoi(a) || (b[0] && pv_vol[0] != atoi(b)) || (c[0] && pv_vol[1] != atoi(c))
                        || (d[0] && set.dr_lev != atoi(d));
        printf("%s lnvol mute %d A %d B %d drums %d rtA %.3f rtB %.3f drg %.4f\n", bad ? "FAIL" : "  ok", ln_mute,
               pv_vol[0], pv_vol[1], set.dr_lev, (double)S.rt_g, (double)SB.rt_g, (double)dr.gain);
#endif
        return 1;
    }
    if (!strcmp(cmd, "mutehintis")) {    /* a check: the strip's message is the muted-part hint a ("A_MUTED", "_" for a space;
                                          * "none": no hint up: the message gone or another one), and, b given, drawn in
                                          * the colour of part b (0 A, 1 B, 2 the drums) */
#ifdef HYB_MULTI
        char w[24] = "";
        uint32_t k;
        const int up = plat_ms() < ui.msg_until && strstr(ui.msg[0], " MUTED") != 0;
        for (k = 0; k < 23u && a[k]; k++) w[k] = a[k] == '_' ? ' ' : a[k];
        const int bad = !strcmp(a, "none") ? up : (!up || strcmp(ui.msg[0], w) || (b[0] && ln_vq != atoi(b) + 1));
        printf("%s mutehintis \"%s\" (msg \"%s\" %s, colour %d)\n", bad ? "FAIL" : "  ok", a, ui.msg[0], up ? "up" : "gone", ln_vq);
#endif
        return 1;
    }
    if (!strcmp(cmd, "vqis")) {          /* a check: the strip's message is up in the colour of part a (0 A, 1 B, 2 the drums) and the
                                          * name of the patch edited (b "name": the launcher's PRESET, "-": any message) */
#ifdef HYB_MULTI
        const int up = plat_ms() < ui.msg_until;
        char t[24];
        t[0] = 0;
        patch_label(t, set.patch, shadow);
        const int bad = !up || ln_vq != atoi(a) + 1 || (strcmp(b, "-") && strcmp(ui.msg[0], t));
        printf("%s vqis %s (msg \"%s\" %s, colour %d, patch \"%s\")\n", bad ? "FAIL" : "  ok", a, ui.msg[0], up ? "up" : "gone", ln_vq, t);
#endif
        return 1;
    }
    if (!strcmp(cmd, "qbusy")) { sq_qbusy = atoi(a); return 1; }   /* (0.96) the main loop's pattern loads held off (1)
                                          * or not: the late load at the line (SQ_QS_WAIT) and its catching up (the
                                          * drums after 0.99: the spare's load and the change's end held off) */
    if (!strcmp(cmd, "qrun")) {          /* a check (0.96): sequencer a (0 A, 1 B, 2 the drums) running (b 1) or not */
        const int run = sq_qrun((uint32_t)atoi(a) % 3u);
        printf("%s qrun %s %d\n", run != atoi(b) ? "FAIL" : "  ok", a, run);
        return 1;
    }
    if (!strcmp(cmd, "plload")) {        /* a check (1.18C35): patches loaded (load_patch) and auditioned (the favorites
                                          * editor's fv_play) a times since the last plload; FAIL else */
        printf("%s plload %u\n", host_pl != (uint32_t)atoi(a) ? "FAIL" : "  ok", (unsigned)host_pl);
        host_pl = 0;
        return 1;
    }
    if (!strcmp(cmd, "fullis")) {        /* a check (0.97): a sequencer's page drawn whole a times and its knobs'
                                          * columns (and the instruments) b times since the last fullis ("-": any);
                                          * FAIL else */
        const int bad = (a[0] != '-' && sq_fulls != (uint32_t)atoi(a)) || (b[0] && b[0] != '-' && sq_ncol != (uint32_t)atoi(b));
        printf("%s fullis whole %u columns %u\n", bad ? "FAIL" : "  ok", (unsigned)sq_fulls, (unsigned)sq_ncol);
        sq_fulls = sq_ncol = 0;
        return 1;
    }
    if (!strcmp(cmd, "sqorg")) {         /* a check (0.96): arp sequencer a's origin (its ticks) is b, the drums' (a 2: the
                                          * clock's ticks) b; FAIL else */
        const uint32_t q = (uint32_t)atoi(a) % 3u, o = q == 2u ? dr_org : sq_org[q & 1u];
        printf("%s sqorg %u %u\n", o != (uint32_t)atoi(b) ? "FAIL" : "  ok", (unsigned)q, (unsigned)o);
        return 1;
    }
    if (!strcmp(cmd, "qmark")) { q_mark = ck.mt; return 1; }   /* (after 0.98) the shared clock's tick now, for qsync */
    if (!strcmp(cmd, "qsync")) {         /* a check (after 0.98): sequencer a's (0 A, 1 B, 2 the drums) origin on a line
                                          * of the shared clock's bars (its bar: SQ_QP8; the arp sequencers' ticks
                                          * from the clock's start) and the first line after the last qmark (b 1:
                                          * the request's time); FAIL else */
        const uint32_t q = (uint32_t)atoi(a) % 3u;
#ifdef HYB_MULTI
        const sq_pat_t *pp = &sqpa[q & 1u];
#else
        const sq_pat_t *pp = &sqp;
#endif
        const uint32_t sh = CK_SH(q == 2u ? drp.div : pp->div), p8 = SQ_QP8(q == 2u ? drp.div : pp->div,
                                                                             q == 2u ? drp.lc : pp->lc);
        const uint32_t o = q == 2u ? dr_org >> sh : sq_org[q & 1u], m = q_mark >> sh;
        const int bad = (o % p8) != 0u || (atoi(b) && (o < m || o > m + p8));
        printf("%s qsync %u origin %u bar %u mark %u\n", bad ? "FAIL" : "  ok", (unsigned)q, (unsigned)o,
               (unsigned)p8, (unsigned)m);
        return 1;
    }
    if (!strcmp(cmd, "dral")) {          /* a check (0.80): the drum pattern's chosen instrument's step a (1..64) has ALIGN
                                          * b (eighths, -7 .. 7; FAIL else) */
        const uint32_t k = (uint32_t)(atoi(a) - 1) & 63u;
        printf("%s dral %s ins %u align %d (st %02x neg %u)\n", DR_AL(sqv.ins, k) != atoi(b) ? "FAIL" : "  ok", a,
               (unsigned)sqv.ins, (int)DR_AL(sqv.ins, k), DR_ST(sqv.ins, k), (unsigned)DR_NEG(sqv.ins, k));
        return 1;
    }
    if (!strcmp(cmd, "bpm")) {           /* the tempo a BPM (63 .. 190; before PLAY: the tempo running then) */
        const uint32_t v = (uint32_t)(atoi(a) - 63) & 0x7Fu;
        shadow[TP_BYTE] = (uint8_t)v;
#ifdef HYB_MULTI
        shadow_o[TP_BYTE] = (uint8_t)v;  /* (1.18: the global tempo, both panel copies; not GLO's settings' change) */
#endif
        tp_set(v);
        return 1;
    }
    if (!strcmp(cmd, "drpat")) {         /* (after 0.99) drum pattern a (1..16) stored as: BD on every step of LENGTH c
                                          * (0..2: 16, 32, 64), time division b (0..4), the last step's BD at ALIGN d
                                          * (-7 .. 7 eighths), SD on step 1 + a (the pattern told apart); stopped */
        static dr_pat_t p;
        const uint32_t n = (uint32_t)(atoi(a) - 1) % DR_PATS, lc = (uint32_t)atoi(c) % SQ_NLC, ns = SQ_STEPS(lc);
        const int al = d[0] ? atoi(d) : 0;
        uint32_t k;
        dr_defaults(&p, 0, 0);
        p.div = (uint8_t)(atoi(b) % (int)SQ_NDIV);
        p.lc = (uint8_t)lc;
        for (k = 0; k < ns; k++) p.on[k >> 4][DR_BD] |= (uint16_t)(1u << (k & 15u));
        k = ns - 1u;
        p.st[k >> 4][DR_BD][k & 15u] = DR_SB(0u, DR_LEN1, (uint32_t)(al < 0 ? -al : al));
        if (al < 0) p.neg[k >> 4][DR_BD] |= (uint16_t)(1u << (k & 15u));
        k = n % ns;
        p.on[k >> 4][DR_SD] |= (uint16_t)(1u << (k & 15u));
        if (dr_store(&p, n, 3u) != 0) printf("FAIL drpat %u: not stored\n", (unsigned)n + 1u);
        if (n == set.dr_pat) dr_load(n);
        return 1;
    }
    if (!strcmp(cmd, "drq")) {           /* (after 0.99) PATTERN (K3 on the drums' page) turned to drum pattern a (1..16)
                                          * from the one shown (the one waiting, else the one playing) */
        const int32_t e = atoi(a) - 1 - SQ_QPAT(2u, DR_PATSH);
        if (e) dr_knob(2u, e);
        return 1;
    }
    if (!strcmp(cmd, "drstis")) {        /* (after 0.99) a check: the store's drum pattern a (1..16; 0: the one playing,
                                          * drp): instrument b's step c (1..64) on (d 1) or off (0); FAIL else */
        static dr_pat_t p;
        const uint32_t i = (uint32_t)atoi(b) % DR_NI, k = (uint32_t)(atoi(c) - 1) & 63u;
        uint32_t on;
        if (atoi(a)) {                   /* (the store's alone: the journal left out) */
            pk.raw = 1;
            dr_load_to(&p, (uint32_t)(atoi(a) - 1) % DR_PATS);
            pk.raw = 0;
        } else p = drp;
        on = p.on[k >> 4][i] >> (k & 15u) & 1u;
        printf("%s drstis %s %s %s %u\n", on != (uint32_t)atoi(d) ? "FAIL" : "  ok", a, b, c, (unsigned)on);
        return 1;
    }
    if (!strcmp(cmd, "drjr")) { dr_jn = dr_jl = dr_jmax = dr_jsum = dr_jst = dr_jbd = dr_jc = dr_jog = 0; dr_jw = store_n[0]; return 1; }
    if (!strcmp(cmd, "drjit")) {         /* (after 0.99) a check: the drums' hits since drjr: each one's lateness
                                          * (the clock's ticks after its own place; ms at the tempo running), the steps
                                          * passed against BD's hits (drpat: BD on every step: one missing a step
                                          * dropped), BD's hits at ALIGN 0 off the shared clock's step grid, the
                                          * store's writes meanwhile; FAIL: a hit later than a ms (given), more than b
                                          * steps missing (given), more than c writes (given), one off the grid */
        const float tk = ((float)S.patch[TP_BYTE] + 63.f) * (16.f * 2.f / 15.f);
        const float mx = (float)dr_jmax * 1000.f / tk, av = dr_jl ? (float)dr_jsum / (float)dr_jl * 1000.f / tk : 0.f;
        const int miss = (int)dr_jst - (int)dr_jbd, wr = (int)(store_n[0] - dr_jw);
        const int bad = (a[0] && mx > (float)atof(a)) || (b[0] && miss > atoi(b)) || (c[0] && wr > atoi(c)) || dr_jog;
        printf("%s drjit changes %u hits %u late %u max %.2f ms mean late %.2f ms steps %u bd %u missing %d writes %d offgrid %u\n",
               bad ? "FAIL" : "  ok", (unsigned)dr_jc, (unsigned)dr_jn, (unsigned)dr_jl, (double)mx, (double)av,
               (unsigned)dr_jst, (unsigned)dr_jbd, miss, wr, (unsigned)dr_jog);
        return 1;
    }
    if (!strcmp(cmd, "drlastat")) {      /* a check (0.80): instrument a's last hit was at step b (1..64) + c eighths (FAIL
                                          * else) */
        const uint32_t i = (uint32_t)atoi(a) % DR_NI, w = (uint32_t)(atoi(b) - 1) * 8u + (uint32_t)atoi(c) + 1u;
        printf("%s drlastat %u step %u + %u/8\n", dr_lastt[i] != w ? "FAIL" : "  ok", (unsigned)i,
               (unsigned)((dr_lastt[i] - 1u) >> 3) + 1u, (unsigned)((dr_lastt[i] - 1u) & 7u));
        dr_lastt[i] = 0;
        return 1;
    }
    if (!strcmp(cmd, "drneg")) {         /* a check (0.80): drum pattern a's (1..16) signs in the store: the head's (b) and
                                          * the rest's (c) room there (1), or not (0: as a release before 0.80 wrote it) */
        const uint32_t n = (uint32_t)(atoi(a) - 1) % DR_PATS;
        uint32_t go, gf;
        int h, x;
        dr_xneg(n, &go, &gf);
        h = plat_store_size(OBJ_DRUM) >= (int)(DR_NEG_OFF + (n + 1u) * DR_NEGH);
        x = plat_store_size(go) >= (int)(gf + DR_NEGX);
        printf("%s drneg %u head %d rest %d (obj %u at %u; sizes %d %d)\n", h != atoi(b) || x != atoi(c) ? "FAIL" : "  ok",
               (unsigned)n + 1u, h, x, (unsigned)go, (unsigned)gf, plat_store_size(OBJ_DRUM), plat_store_size(go));
        return 1;
    }
    if (!strcmp(cmd, "sqal")) {          /* a check (0.78): the shown arp pattern's step a (1..64) has ALIGN b (eighths,
                                          * -7 .. 7; FAIL else) */
        const sq_step_t *t = &sqp.st[(uint32_t)(atoi(a) - 1) & 63u];
        printf("%s sqal %s align %d (ev %02x la %02x)\n", SQ_AL(t) != atoi(b) ? "FAIL" : "  ok", a, (int)SQ_AL(t), t->ev,
               t->la);
        return 1;
    }
    if (!strcmp(cmd, "padis")) {         /* (1.14) a check: the shown arp pattern's settings: PATCH ARP a, KEY LATCH b,
                                          * REC QUANT c (1 ON, 0 OFF), its copy as last seen the same; FAIL else */
        const int pa = SQ_FON(sqp.pad, SQ_PARP), kl = SQ_FON(sqp.pad, SQ_NLAT), rq = SQ_FON(sqp.pad, SQ_NRQ);
        const int bad = pa != atoi(a) || kl != atoi(b) || rq != atoi(c) || sqp.pad != un_shs[UN_SQI].pad;
        printf("%s padis PATCH ARP %d KEY LATCH %d REC QUANT %d (pad %d, copy %d)\n", bad ? "FAIL" : "  ok", pa, kl, rq,
               sqp.pad, un_shs[UN_SQI].pad);
        return 1;
    }
    if (!strcmp(cmd, "padxis")) {        /* (1.14) a check: arp sequencer a's (0 A, 1 B) pattern's pad byte is b, its copy
                                          * as last seen the same; FAIL else */
#ifdef HYB_MULTI
        const uint32_t i = (uint32_t)atoi(a) & 1u;
        const sq_pat_t *q = &sqpa[i];
#else
        const uint32_t i = 0;
        const sq_pat_t *q = &sqp;
        if (atoi(a)) { printf("  ok padxis %s: one arp sequencer here\n", a); return 1; }
#endif
        printf("%s padxis %u pad %d copy %d\n", q->pad != atoi(b) || un_shs[i].pad != q->pad ? "FAIL" : "  ok",
               (unsigned)i, q->pad, un_shs[i].pad);
        return 1;
    }
    if (!strcmp(cmd, "baseis")) {        /* (1.14) a check: the BASE NOTE of arp sequencer A is a, of B (the USB build) b
                                          * (0 OFF); FAIL else */
#ifdef HYB_MULTI
        const int bad = set.sq_base != atoi(a) || set.sqb_base != atoi(b);
        printf("%s baseis A %d B %d\n", bad ? "FAIL" : "  ok", set.sq_base, set.sqb_base);
#else
        printf("%s baseis %d\n", set.sq_base != atoi(a) ? "FAIL" : "  ok", set.sq_base);
#endif
        return 1;
    }
    if (!strcmp(cmd, "sqdefis")) {       /* (1.14) a check: GLO > SEQ's defaults (set.sq_def) are a (the pad's bits:
                                          * 1 PATCH ARP ON, 2 KEY LATCH OFF, 4 REC QUANT OFF); b "saved": the settings
                                          * object holds them too; FAIL else */
        settings_t q;
        const int sv = settings_load(&q);
        const int bad = set.sq_def != atoi(a) || (b[0] && (!sv || q.sq_def != set.sq_def));
        printf("%s sqdefis %d (saved %d)\n", bad ? "FAIL" : "  ok", set.sq_def, sv ? q.sq_def : -1);
        return 1;
    }
    if (!strcmp(cmd, "sqpadst")) {       /* (1.14) arp pattern a (1..32) stored as an older release wrote it: the
                                          * steps as shipped, pad b (0..3: PATCH ARP, KEY LATCH; REC QUANT unknown
                                          * then), its head alone; FAIL if not stored */
        sq_pat_t p;
        const uint32_t n = (uint32_t)(atoi(a) - 1) % SQ_PATS;
        sq_defaults(&p, 0);
        p.pad = (uint8_t)(atoi(b) & 3);
        if (sq_store(&p, n, 1u) != 0) printf("FAIL sqpadst %u: not stored\n", (unsigned)n + 1u);
        return 1;
    }
    if (!strcmp(cmd, "recqt")) {         /* (1.14) a check: arp sequencer a's last note on (a step played) sounded where
                                          * RECORD's last key was heard, b loops of the pattern later, within c eighths
                                          * of a step (REC QUANT OFF: half of one, the rounding); FAIL else. In the
                                          * clock's ticks and in samples (the clock's ticks to the sample: its tempo) */
        const uint32_t i = (uint32_t)atoi(a) & 1u;
#ifdef HYB_MULTI
        const sq_pat_t *pp = &sqpa[i];
#else
        const sq_pat_t *pp = &sqp;
#endif
        const uint32_t sh = CK_SH(pp->div);
        const float bpm = S.bpm_now > 1.f ? S.bpm_now : (float)S.patch[128 + 16] + 63.f, tk = bpm * (16.f * 2.f / 15.f);
        const float loop = (float)(SQ_STEPS(pp->lc) * 8u << sh), want = host_rq_h + (float)atoi(b) * loop;
        const float d = (float)sq_lastm[i] - want, tol = (float)atof(c) * (float)(1u << sh) + 0.5f;
        printf("%s recqt %u played tick %u heard %.2f + %d loops: %+.2f ticks (%+.1f samples, %+.2f ms; tolerance %.2f ticks)\n",
               (d < 0.f ? -d : d) > tol ? "FAIL" : "  ok", (unsigned)i, (unsigned)sq_lastm[i], (double)host_rq_h,
               atoi(b), (double)d, (double)(d * S.fs / tk), (double)(d * 1000.f / tk), (double)tol);
        return 1;
    }
    if (!strcmp(cmd, "klat")) {          /* a check: the shown arp pattern's KEY LATCH is a (1 ON, 0 OFF; FAIL else) */
        const int on = !(sqp.pad & SQ_NLAT);
        printf("%s klat %d pad %d\n", on != atoi(a) ? "FAIL" : "  ok", on, sqp.pad);
        return 1;
    }
    if (!strcmp(cmd, "glolcdis")) {      /* (after 1.08) a check: GLO's strip reads a and b ("_" for a space; b "-": not
                                          * checked); FAIL else */
        char x[40] = "", y[40] = "", wa[24] = "", wb[24] = "";
        uint32_t k;
        for (k = 0; k < 23u && a[k]; k++) wa[k] = a[k] == '_' ? ' ' : a[k];
        for (k = 0; k < 23u && b[k]; k++) wb[k] = b[k] == '_' ? ' ' : b[k];
        glo_lcd(x, y);
        printf("%s glolcdis \"%s\" \"%s\"\n", strcmp(x, wa) || (strcmp(b, "-") && strcmp(y, wb)) ? "FAIL" : "  ok", x, y);
        return 1;
    }
    if (!strcmp(cmd, "glofit")) {        /* (1.18C48) a check: each line of GLO's page shown fits its room as the list draws
                                          * it (ui.c ls_line: the name in FONT_M from x 10, the value at the right end, 12
                                          * apart) with the value it has now; FAIL else (the line would run) */
        uint32_t i, bad = 0;
        for (i = 0; i < GLO_PN[ui.gpage]; i++) {
            char w[40] = "", pre[6] = "", *v;
            uint32_t fl = 0;
            int32_t x0, x1;
            glo_item(i, w, pre, &fl);
            v = ls_val(w);
            ls_win(pre, fl, v, &x0, &x1);
            if (text_w(&FONT_M, w) > x1 - x0) { bad++; printf("  glofit \"%s\" \"%s\" %d of %d\n", w, v, text_w(&FONT_M, w), x1 - x0); }
        }
        printf("%s glofit page %d (%d lines)\n", bad ? "FAIL" : "  ok", ui.gpage, GLO_PN[ui.gpage]);
        return 1;
    }
    if (!strcmp(cmd, "stripfit")) {      /* (1.14) a check: the LCD strip's name and value fit its room as draw_lcd
                                          * sets them (the name FONT_L or FONT_M, the value down to FONT_B): a "msg"
                                          * the message shown (app_say), "glo" GLO's strip; FAIL else */
        char x[40] = "", y[40] = "";
        const felucca_font_t *fa = &FONT_L, *fv = &FONT_L;
        int32_t room = 232 - 14, wv;
        if (a[0] == 'g') glo_lcd(x, y);
        else if (a[0] == 'p') play_lcd(x, y);   /* (1.19.2: "play", PLAY mode's page or its editor) */
        else { text_cat(x, ui.msg[0]); text_cat(y, ui.msg[1]); }
        wv = y[0] ? text_w(fv, y) + 10 : 0;
        if (text_w(fa, x) + wv > room) fa = &FONT_M;
        if (text_w(fa, x) + wv > room && y[0]) { fv = &FONT_M; wv = text_w(fv, y) + 10; }
        if (text_w(fa, x) + wv > room && y[0]) { fv = &FONT_B; wv = text_w(fv, y) + 10; }
        printf("%s stripfit %s \"%s\" \"%s\" (%d of %d)\n", text_w(fa, x) + wv > room ? "FAIL" : "  ok", a, x, y,
               (int)(text_w(fa, x) + wv), (int)room);
        return 1;
    }
    if (!strcmp(cmd, "glolcdhas")) {     /* (1.12) a check: GLO's strip's value holds a ("_" for a space), and b not (b
                                          * given); FAIL else */
        char x[40] = "", y[40] = "", wa[24] = "", wb[24] = "";
        uint32_t k;
        for (k = 0; k < 23u && a[k]; k++) wa[k] = a[k] == '_' ? ' ' : a[k];
        for (k = 0; k < 23u && b[k]; k++) wb[k] = b[k] == '_' ? ' ' : b[k];
        glo_lcd(x, y);
        printf("%s glolcdhas \"%s\" \"%s\"\n", !strstr(y, wa) || (wb[0] && strstr(y, wb)) ? "FAIL" : "  ok", x, y);
        return 1;
    }
    if (!strcmp(cmd, "glopageis")) {     /* (1.19.1) a check: the lines of GLO's page shown are a, by name, in this order
                                          * ("TRANSPOSE,VELOCITY,..."; "_" for a space); FAIL else */
        char w[400] = "", x[400] = "";
        uint32_t i;
        for (i = 0; i < GLO_PN[ui.gpage]; i++) { if (i) strcat(w, ","); strcat(w, G_NAME[GLO_PAGE[ui.gpage][i]]); }
        for (i = 0; i < sizeof x - 1u && a[i]; i++) x[i] = a[i] == '_' ? ' ' : a[i];
        printf("%s glopageis page %u: %s\n", strcmp(w, x) ? "FAIL" : "  ok", ui.gpage, w);
        return 1;
    }
    if (!strcmp(cmd, "glodimis")) {      /* (1.19.1) a check: the line named a on GLO's page shown is drawn dimmed (b 1) or
                                          * not (b 0; glo_item's LS_DIM, 32; 1.19.3: no line dims any more); FAIL else
                                          * (or no such line) */
        char w[24] = "", t[40] = "", pre[6] = "";
        uint32_t i, fl = 0;
        for (i = 0; i < 23u && a[i]; i++) w[i] = a[i] == '_' ? ' ' : a[i];
        for (i = 0; i < GLO_PN[ui.gpage] && strcmp(G_NAME[GLO_PAGE[ui.gpage][i]], w); i++)
            ;
        if (i == GLO_PN[ui.gpage]) { printf("FAIL glodimis %s: not on page %u\n", a, ui.gpage); return 1; }
        glo_item(i, t, pre, &fl);
        printf("%s glodimis %s %d\n", (int)!!(fl & 32u) != atoi(b) ? "FAIL" : "  ok", a, (int)!!(fl & 32u));
        return 1;
    }
    if (!strcmp(cmd, "glopos")) {        /* GLO (0.81): the cursor to the line named a (one word: AUDIO, REVERB; "_" for a
                                          * space, 1.18C26: DRUM_ACC) on the page shown, whatever the build's order; FAIL
                                          * if it has none */
        uint32_t i;
        char w[24] = "";
        for (i = 0; i < 23u && a[i]; i++) w[i] = a[i] == '_' ? ' ' : a[i];
        for (i = 0; i < GLO_PN[ui.gpage] && strcmp(G_NAME[GLO_PAGE[ui.gpage][i]], w); i++)
            ;
        if (i == GLO_PN[ui.gpage]) { printf("FAIL glopos %s: not on page %u\n", a, ui.gpage); return 1; }
        ui.gpos = (uint8_t)i;
        view_reset();
        return 1;
    }
    if (!strcmp(cmd, "hload")) {         /* (after 1.18C30) the blocks' load REVERB AUTO and the overload guard see (app.c
                                          * rvq_tick), % of a block; -1 (as booted): they stay idle */
        host_load = atoi(a);
        return 1;
    }
    if (!strcmp(cmd, "hgov")) {          /* (after 1.18C32) the governor's figures faked (app.c host_gov), % of a block:
                                          * a CTL, b a voice slot on the first core, c the wait for the second, d the
                                          * rest; hgov -1: the computer's own */
        host_gov[0] = atoi(a);
        host_gov[1] = b[0] ? atoi(b) : 0;
        host_gov[2] = c[0] ? atoi(c) : 0;
        host_gov[3] = d[0] ? atoi(d) : 0;
        return 1;
    }
    if (!strcmp(cmd, "hpk")) { host_pk = atoi(a); return 1; }   /* (after 1.18C32) the blocks' own time the governor's
                                          * calm sees (app.c host_pk), %; -1 none */
    if (!strcmp(cmd, "limis")) {         /* (after 1.18C32) a check: the governor's limit within a..b; FAIL else */
        const int bad = (int)GOV_MAX < atoi(a) || (int)GOV_MAX > atoi(b[0] ? b : a);
        printf("%s limis %s %s: the limit %d (the cap %d), %u sounding, calm %u blocks, xruns %u\n", bad ? "FAIL" : "  ok", a, b, (int)GOV_MAX, voice_cap(),
               (unsigned)stat_voices, (unsigned)(blocks - gov_busy), (unsigned)plat_xruns());
        return 1;
    }
#if GOV_BUD && GOV_FIX
    if (!strcmp(cmd, "budis")) {         /* (1.18C49) a check: the budget (app.c GOV_BUD) faded at least a voices early
                                          * (b: at most) since power-on, c (given): it acts (1) or not (0); with the
                                          * voices on and those fading; d (given): the voices on, not fading, exactly
                                          * this many; FAIL else */
        int i, on = 0, f = 0, p, bad;
        for (p = 0; p < HOST_NP; p++) {
            const sy_synth *s = HOST_PART(p);
            for (i = 0; i < HOST_NV(s); i++) { on += s->v[i].on; f += s->v[i].on && s->v[i].ufade; }
        }
        bad = bud_n < atoi(a) || (b[0] && bud_n > atoi(b)) || (c[0] && bud_on != atoi(c)) || (d[0] && on - f != atoi(d));
        printf("%s budis %s %s %s: %u faded early, acts %u (engine %u), the limit %d, %d on (%d fading), shed %u\n",
               bad ? "FAIL" : "  ok", a, b, c, (unsigned)bud_n, (unsigned)bud_on, (unsigned)sy_bud, (int)GOV_MAX, on, f, (unsigned)grd_n);
        return 1;
    }
#endif
    if (!strcmp(cmd, "grdis")) {         /* (after 1.18C30) a check: the overload guard shed at least a voices (b: at most,
                                          * given) since power-on; FAIL else */
        const int bad = grd_n < atoi(a) || (b[0] && grd_n > atoi(b));
        printf("%s grdis %s %s: %u shed, the limit %d\n", bad ? "FAIL" : "  ok", a, b, (unsigned)grd_n, (int)GOV_MAX);
        return 1;
    }
    if (!strcmp(cmd, "rvqis")) {         /* a check (0.81): GLO > AUDIO > REVERB saved as a (0 the build's own, 1 FULL, 2
                                          * HALF, 3 AUTO) and the engine's reverb running path b (full, half; def: the
                                          * build's own, AUTO's at rest FULL); FAIL else */
        extern int hyb_rv_is_full(void);
        const int want = !strcmp(b, "full") ? 1 : !strcmp(b, "half") ? 0 : RVQ_DEF_NOW != RVQ_HALF, f = hyb_rv_is_full();
        printf("%s rvqis %s %s: rv_q %d, the engine %s\n", (set.rv_q != (uint8_t)atoi(a) || f != want) ? "FAIL" : "  ok", a, b,
               set.rv_q, f ? "FULL" : "HALF");
        return 1;
    }
#ifdef HYB_MULTI
    if (!strcmp(cmd, "veis")) {          /* a check (1.18C26): GLO > VELOCITY as set: a VELOCITY (1 ON, 0 OFF), b FIXED, c
                                          * CURVE (0 SOFT2 .. 2 NORMAL .. 4 HARD2), d SENSITIVITY; "-" not checked; FAIL
                                          * else */
        const char *w[4] = {a, b, c, d};
        const int v[4] = {!(set.ve_fl & VE_OFF), set.ve_fix, set.ve_crv, set.ve_sens};
        int bad = 0, q;
        for (q = 0; q < 4; q++) if (w[q][0] && w[q][0] != '-' && v[q] != atoi(w[q])) bad = 1;
        printf("%s veis on %d fixed %d curve %d sens %d drum %d\n", bad ? "FAIL" : "  ok", v[0], v[1], v[2], v[3],
               (set.ve_fl & VE_DRUM) != 0);
        return 1;
    }
    if (!strcmp(cmd, "drchis")) {        /* a check (1.18C31): GLO > MIDI > DRUM CHANNEL as set is a (0 OFF); FAIL else */
        printf("%s drchis %d\n", set.dr_ch != atoi(a) ? "FAIL" : "  ok", set.dr_ch);
        return 1;
    }
    if (!strcmp(cmd, "settrunc")) {      /* (1.18C31) the settings object rewritten as its first a bytes (an older
                                          * release's object, e.g. 208: VELOCITY's, before DRUM CHANNEL) */
        static uint8_t w[sizeof(settings_t)];
        const int n = plat_store_load(OBJ_SETTINGS, w, sizeof w), m = 0
#ifdef HOST_XM
            + (!strcmp(a, "osc") ? (int)__builtin_offsetof(settings_t, osc_ok) : 0)   /* (1.19.5: an object before OSC) */
#endif
            + atoi(a);
        printf("  settrunc %d of %d: %d\n", m, n, n >= m ? plat_store_save(OBJ_SETTINGS, w, (uint32_t)m) : -1);
        return 1;
    }
    if (!strcmp(cmd, "glold")) {         /* (1.18C42) REVERB and LATENCY as shipped before 1.18C42 (AUTO for a REVERB
                                          * never turned, LATENCY LOW in fresh settings; here and after a "reboot"): the
                                          * scripts written against them. LATENCY taken now as GLO's list takes it */
        host_old_def = 1;
        set.lat = LAT_LOW;
        lat_req = set.lat;
        plat_audio_fifo(lat_fifo());
        rvq_apply();
        printf("  glold\n");
        return 1;
    }
    if (!strcmp(cmd, "gl47")) {          /* (1.18C48) GLO's and the kit's defaults as shipped until 1.18C47 (one UNISON
                                          * MAX 2 for both modes, MONO UNI MAX 5, PATCH ARP and PRECOUNT OFF, the kit all
                                          * 808 / SYN; here and after a "reboot" into fresh settings): the scripts
                                          * written against them. Silent (their output as 1.18C47's) */
#ifdef HYB_MULTI
        host_def47 = 1;
        set.mx_uni = 2u;
        set.mx_unim = 5u;
        set.mx_unx = 0;
        mx_apply();
#endif
        set.sq_def &= (uint8_t)~(SQ_PARP | SQ_PCM);
        sq_def_sync();
        drk.k[DR_BD].m909 = drk.k[DR_CH].m909 = drk.k[DR_OH].m909 = 0;
        return 1;
    }
    if (!strcmp(cmd, "setpoke")) {       /* (after 1.18C34) the settings object's byte a set to b in the store (a value an
                                          * older or a damaged object holds there: taken at the next reboot) */
        static uint8_t w[sizeof(settings_t)];
        const int n = plat_store_load(OBJ_SETTINGS, w, sizeof w);
        int k = atoi(a);
        if (!strcmp(a, "sq_def")) k = (int)__builtin_offsetof(settings_t, sq_def);   /* (1.18C48: a field by its name) */
#ifdef HYB_MULTI
        else if (!strcmp(a, "mx_uni")) k = (int)__builtin_offsetof(settings_t, mx_uni);
        else if (!strcmp(a, "mx_unim")) k = (int)__builtin_offsetof(settings_t, mx_unim);
        else if (!strcmp(a, "mx_unx")) k = (int)__builtin_offsetof(settings_t, mx_unx);
#ifdef HOST_XM
        else if (!strcmp(a, "ck_fl")) k = (int)__builtin_offsetof(settings_t, ck_fl);   /* (1.19.5: CLK LOCK's byte) */
#endif
#endif
        if (k >= 0 && k < n) w[k] = (uint8_t)atoi(b);
        printf("  setpoke %d = %d of %d: %d\n", k, atoi(b), n, k < n ? plat_store_save(OBJ_SETTINGS, w, (uint32_t)n) : -1);
        return 1;
    }
    if (!strcmp(cmd, "vedrum")) {        /* a check (1.18C26): DRUM ACC as set, a (0 RAW, 1 VELOCITY); FAIL else */
        printf("%s vedrum %d\n", ((set.ve_fl & VE_DRUM) != 0) != atoi(a) ? "FAIL" : "  ok", (set.ve_fl & VE_DRUM) != 0);
        return 1;
    }
    if (!strcmp(cmd, "vcstrip") || !strcmp(cmd, "vcsay") || !strcmp(cmd, "vcline")) {   /* checks (after 1.18C30):
                                          * GLO > VELOCITY > CALIBRATE's strip (name and value, the message aside),
                                          * the message said last ("vcsay": its two texts), line a's value; each holds
                                          * the text given ("_" for a space; vcline: b); FAIL else */
        char x[64] = "", y[40] = "", w[32] = "";
        const char *q = cmd[2] == 'l' ? b : a;
        uint32_t k;
        for (k = 0; k < 31u && q[k]; k++) w[k] = q[k] == '_' ? ' ' : q[k];
        if (cmd[2] == 's' && cmd[3] == 't') {
            ve_cal_lcd(x, y);
        } else if (cmd[2] == 's') {
            text_cat(x, ui.msg[0]);
            text_cat(y, ui.msg[1]);
        } else {
            for (k = 0; k < GLO_PN[GP_VCAL] && strcmp(G_NAME[GLO_PAGE[GP_VCAL][k]], a); k++)
                ;
            if (k < GLO_PN[GP_VCAL]) glo_val(GLO_PAGE[GP_VCAL][k], y);
        }
        text_cat(x, " ");
        text_cat(x, y);
        printf("%s %s %s: \"%s\"\n", strstr(x, w) ? "  ok" : "FAIL", cmd, q, x);
        return 1;
    }
    if (!strcmp(cmd, "topmodel")) {      /* (2026-11-14) the top row's probe as the host models it (fm1-base lib/touch.c):
                                          * a ms of rise from the contact, b counts of noise, c readings per press at most
                                          * (0: no limit), d the first reading's ms after the press (r: 0..3 at random) */
        th_rise = (uint32_t)atoi(a);
        th_noise = (uint32_t)atoi(b);
        th_reads = c[0] && atoi(c) ? (uint32_t)atoi(c) : 255u;
        th_phase = d[0] == 'r' ? 255u : (uint32_t)atoi(d);
        return 1;
    }
    if (!strcmp(cmd, "topkey")) {        /* top key a (0 C#5 .. 6 G5) reads b % of the row's peak plus c counts (the model) */
        const uint32_t i = (uint32_t)atoi(a) % VE_NTOP;
        th_gain[i] = atoi(b);
        th_off[i] = atoi(c);
        return 1;
    }
    if (!strcmp(cmd, "topwait")) {       /* the top row's note waits for its peak: a readings (0: as before, touch.c) */
        touch_top_wait((uint32_t)atoi(a));
        return 1;
    }
    if (!strcmp(cmd, "toppeak")) {       /* a check: top key a's latched peak is b (-1: none); FAIL else */
        const int p = touch_top_peak(VE_TOP0 + (uint32_t)atoi(a) % VE_NTOP);
        printf("%s toppeak %s %d\n", p != atoi(b) ? "FAIL" : "  ok", a, p);
        return 1;
    }
    if (!strcmp(cmd, "vecal")) {         /* a check (1.18C26): GLO > VELOCITY > CALIBRATE, top key a (0 C#5 .. 6 G5): its
                                          * ends b c as set ("0 0": the row's) and, d given, the velocity a peak of d gives
                                          * on it now (printed); FAIL else */
        const uint32_t i = (uint32_t)atoi(a) % VE_NTOP;
        const int bad = set.ve_lo[i] != atoi(b) || set.ve_hi[i] != atoi(c);
        printf("%s vecal %u ends %d %d", bad ? "FAIL" : "  ok", (unsigned)i, set.ve_lo[i], set.ve_hi[i]);
        if (d[0]) printf(" peak %s vel %d", d, touch_top_vel(VE_TOP0 + i, (uint32_t)atoi(d)));
        printf("\n");
        return 1;
    }
    if (!strcmp(cmd, "oscis")) {         /* a check (1.18C22): GLO > AUDIO > OSC saved as a (0 ORIGINAL, 1 CLEAN) and both
                                          * parts' engines on it (sy_synth osc_clean); FAIL else */
        const int w = atoi(a);
        printf("%s oscis %d: osc_cl %d, the engines %g %g\n", (set.osc_cl != (uint8_t)w || S.osc_clean != (float)w
               || SB.osc_clean != (float)w) ? "FAIL" : "  ok", w, set.osc_cl, (double)S.osc_clean, (double)SB.osc_clean);
        return 1;
    }
    if (!strcmp(cmd, "fltis")) {         /* a check (after 1.18C34): GLO > AUDIO > FILT KNOBS saved as a (0 ORIGINAL, 1
                                          * SMOOTH) and both parts' engines on it (sy_synth flt_sm); FAIL else */
        const int w = atoi(a);
        printf("%s fltis %d: flt_sm %d, the engines %d %d\n", (set.flt_sm != (uint8_t)w || S.flt_sm != (uint8_t)w
               || SB.flt_sm != (uint8_t)w) ? "FAIL" : "  ok", w, set.flt_sm, S.flt_sm, SB.flt_sm);
        return 1;
    }
    if (!strcmp(cmd, "glmark")) {        /* (after 1.18C34, FILT KNOBS) both parts' glides marked (glided) */
        memcpy(host_fs_t[0], S.fs_t, sizeof S.fs_t);
        memcpy(host_fs_t[1], SB.fs_t, sizeof SB.fs_t);
        return 1;
    }
    if (!strcmp(cmd, "glided")) {        /* a check (after 1.18C34): part a's (0 A, 1 B) value of patch byte b (40 .. 43)
                                          * took a new glide since glmark (c 1) or not (c 0; ORIGINAL never glides: a
                                          * step), at the byte's value d (if given), not gliding now; FAIL else */
        const sy_synth *p = atoi(a) ? &SB : &S;
        const uint32_t by = (uint32_t)atoi(b) & 255u, q = (by - 40u) & 3u;
        const int g = p->fs_t[q] != host_fs_t[atoi(a) ? 1 : 0][q];
        const int bad = by - 40u > 3u || g != atoi(c) || p->pv[by] != (float)p->patch[by] || (d[0] && p->patch[by] != atoi(d));
        printf("%s glided %s %s %s %s (pv %g byte %u)\n", bad ? "FAIL" : "  ok", a, b, c, d, (double)p->pv[by], p->patch[by]);
        return 1;
    }
    if (!strcmp(cmd, "glideis")) {       /* a check (after 1.18C34, FILT KNOBS): part a's (0 A, 1 B) engine value of
                                          * patch byte b gliding (c 1: between, not yet at the byte) or not (c 0: at it);
                                          * d (if given): the byte's value; FAIL else */
        const sy_synth *p = atoi(a) ? &SB : &S;
        const uint32_t by = (uint32_t)atoi(b) & 255u;
        const int g = p->pv[by] != (float)p->patch[by], bad = g != atoi(c) || (d[0] && p->patch[by] != atoi(d));
        printf("%s glideis %s %s %s %s (pv %g byte %u)\n", bad ? "FAIL" : "  ok", a, b, c, d, (double)p->pv[by], p->patch[by]);
        return 1;
    }
#endif
    if (!strcmp(cmd, "vlis")) {          /* a check (0.91): a parameter's list of values up (a 1) or not (0); b (given):
                                          * the parameter's label; FAIL else */
        const int up = ui.vl_until && plat_ms() < ui.vl_until && ui.vl_p;
        char w[24] = "";
        uint32_t k;
        for (k = 0; k < 23u && b[k]; k++) w[k] = b[k] == '_' ? ' ' : b[k];   /* ("_" a space: "FILT_1") */
        const int bad = up != atoi(a) || (b[0] && (!up || strcmp(ui.vl_p->label, w)));
        printf("%s vlis %s %s (up %d, %s)\n", bad ? "FAIL" : "  ok", a, b, up, up ? ui.vl_p->label : "-");
        return 1;
    }
    if (!strcmp(cmd, "rowis")) {         /* a check (0.84): the row K1..K4 act on is tagged a ("LEVEL", "SLOT_4"); FAIL
                                          * else */
        const int32_t r = row_on();
        const char *t = r >= 0 ? U_ROW[r].tag : "-";
        char w[24] = "";
        uint32_t k;
        for (k = 0; k < 23u && a[k]; k++) w[k] = a[k] == '_' ? ' ' : a[k];   /* ("_" a space: "SLOT_4", 0.86) */
        printf("%s rowis %s (%s, row %d, view %d)\n", strcmp(t, w) ? "FAIL" : "  ok", a, t, (int)r, ui.view);
        return 1;
    }
    if (!strcmp(cmd, "knobis")) {        /* a check (0.84): that row's knob a (1..4): its label b, the value under it c,
                                          * the strip's value d ("_" a space: "-4.0_dB"; c, d: if given); FAIL else */
        const int32_t r = row_on();
        const u_param_t *p = r >= 0 && atoi(a) >= 1 && atoi(a) <= 4 ? &U_ROW[r].p[atoi(a) - 1] : 0;
        char kt[24] = "", st[24] = "", wd[24] = "", wc[24] = "", wb[24] = "";
        uint32_t k;
        for (k = 0; k < 23u && b[k]; k++) wb[k] = b[k] == '_' ? ' ' : b[k];   /* (1.18C45: "_" a space in the label) */
        b = wb;
        for (k = 0; k < 23u && d[k]; k++) wd[k] = d[k] == '_' ? ' ' : d[k];
        for (k = 0; k < 23u && c[k]; k++) wc[k] = c[k] == '_' ? ' ' : c[k];   /* (0.89: "_" a space under the knob too) */
        c = wc;
        if (p) { value_text(p, shadow[p_byte(p)], kt); strip_text(p, shadow[p_byte(p)], st); }
        {
            const int bad = !p || strcmp(p->label, b) || (c[0] && strcmp(kt, c)) || (d[0] && strcmp(st, wd));
            printf("%s knobis %s %s %s \"%s\" (%s %s \"%s\")\n", bad ? "FAIL" : "  ok", a, b, c, wd, p ? p->label : "-", kt,
                   st);
        }
        return 1;
    }
    if (!strcmp(cmd, "byteis")) {        /* a check (0.84): the edited part's patch byte a is b, on the panel (shadow)
                                          * and in its engine; c (if given, two parts): the other part's byte; d (if
                                          * given): the value the one-part build has instead of b; FAIL else */
        const uint32_t by = (uint32_t)atoi(a) & 255u;
#ifdef HYB_MULTI
        const uint32_t e = ed_part ? SB.patch[by] : S.patch[by], o = shadow_o[by];
#else
        const uint32_t e = S.patch[by], o = 0;
        if (d[0]) b = d;
#endif
        const int bad = shadow[by] != (uint32_t)atoi(b) || e != (uint32_t)atoi(b)
#ifdef HYB_MULTI
                        || (c[0] && o != (uint32_t)atoi(c))
#endif
            ;
        printf("%s byteis %s %s %s (panel %d engine %d other %d)\n", bad ? "FAIL" : "  ok", a, b, c, shadow[by], (int)e,
               (int)o);
        return 1;
    }
    if (!strcmp(cmd, "fxmis")) {         /* a check (0.86): the edited part's engine: the matrix's offset on effect byte a
                                          * (105-119; the voice started last, fxm) is b, and (c given) the value its
                                          * effects read this block (fxv) c; within 0.01; FAIL else */
        const uint32_t by = (uint32_t)atoi(a);
#ifdef HYB_MULTI
        const sy_synth *e = ed_part ? &SB : &S;
#else
        const sy_synth *e = &S;
#endif
        const int in = by >= 105u && by <= 119u;
        const float m = in ? e->fxm[by - 105u] : -999.f, f = in ? e->fxv[by - 105u] : -999.f;
        float dm = m - (float)atof(b), df = c[0] ? f - (float)atof(c) : 0.f;
        if (dm < 0.f) dm = -dm;
        if (df < 0.f) df = -df;
        printf("%s fxmis %s %s %s (offset %.3f value %.3f)\n", (!in || dm > 0.01f || df > 0.01f) ? "FAIL" : "  ok", a, b, c,
               (double)m, (double)f);
        return 1;
    }
    if (!strcmp(cmd, "drsound")) {       /* a check (0.84): a drum voice sounding now (a 1) or none (a 0); FAIL else */
        printf("%s drsound %s (act %04x)\n", (dr.act != 0) != (atoi(a) != 0) ? "FAIL" : "  ok", a, dr.act);
        return 1;
    }
    if (!strcmp(cmd, "shpoke")) { shadow[atoi(a) & 255] = (uint8_t)atoi(b); return 1; }   /* (before "reboot": the
                                          * panel's byte a lost to b, as RAM would be; nothing else told) */
    if (!strcmp(cmd, "storedis")) {      /* a check (after 0.98): the edited part's panel patch (shadow) as the patch up's
                                          * stored bytes (a 1) or not (0; two parts, 1.18: but B 16, the global tempo),
                                          * and its engine as the panel (bytes 129, 144: the arpeggiator's, 7 the
                                          * part's volume: set apart); FAIL else */
#ifdef HYB_MULTI
        const sy_synth *en = ed_part ? &SB : &S;
#else
        const sy_synth *en = &S;
#endif
        const uint8_t *st = patch_bytes(set.patch);
        int k, ds = 0, de = 0;
        for (k = 0; k < 240; k++) {
#ifdef HYB_MULTI
            if (shadow[k] != st[k] && k != 144) ds++;   /* (1.18: B 16 the global tempo, not the patch's) */
#else
            if (shadow[k] != st[k]) ds++;
#endif
            if (k != 7 && k != 129 && k != 144 && en->patch[k] != shadow[k]) {
                if (!de) printf("  (engine byte %d: %d, panel %d)\n", k, en->patch[k], shadow[k]);
                de++;
            }
        }
        printf("%s storedis %s (differ %d, engine %d)\n", (ds == 0) != atoi(a) || de ? "FAIL" : "  ok", a, ds, de);
        return 1;
    }
#ifdef HYB_RAND
    if (!strcmp(cmd, "rzis") || !strcmp(cmd, "rzstat")) {   /* (after 0.98) checks of RANDOMIZE (app.c rz_*): rzis:
                                          * the edit buffer a roll (a 1: sx_edit 2, the name "~") or the original (0),
                                          * each byte within its class's bounds against rz_org (kept, +-1 a list,
                                          * +-20 % / 8 % of the range, the floor bytes a quarter of it down), the
                                          * engine as the panel. rzstat: every roll 1..255 of the original so checked,
                                          * the bytes moved on average (at least a, at most b), the original back;
                                          * FAIL else */
        const int stat = cmd[2] == 's';
        uint32_t sd, y, bad = 0, moved = 0, rolls = 0, keep = 0, o0 = rz_seed;
        for (sd = stat ? 1u : 0u; sd < (stat ? 256u : 1u); sd++) {
            if (stat) rz_apply(sd);
            rolls++;
            if ((sx_edit == 2u) != (stat || atoi(a)) || ((shadow[240] == '~') != (stat || atoi(a)) && rz_org[240] != '~')) bad++;
            for (y = 0; y < 240u; y++) {
                const u_param_t *pp = param_of(y);
                const int32_t o = rz_org[y], v = shadow[y], d = v - o;
                int32_t lim = 0;
                if (!d) continue;
                moved++;
                if (pp && !rz_in(RZ_KEEP, sizeof RZ_KEEP, y)) {
                    const int32_t mx = (int32_t)p_max(pp);
                    if (pp->kind == V_ENUM && (pp->en == E_LFOSH || pp->en == E_FXSH || pp->en == E_SAT)) lim = 1;
                    else if (pp->kind == V_NUM || pp->kind == V_BI || pp->kind == V_SHAPE)
                        lim = mx * (rz_in(RZ_SMALL, sizeof RZ_SMALL, y) ? 8 : 20) / 100 + 1;
                    if (d < 0 && (y == 36u || y == 40u || y == 61u)) lim = lim / 4 + 1;
                    if (v > mx) lim = 0;
                }
                if (d > lim || -d > lim) {
                    if (bad < 8u) printf("  (roll %u byte %u: %d -> %d, %s)\n", (unsigned)sd, (unsigned)y, (int)o, (int)v,
                                         pp ? pp->label : "no parameter");
                    bad++;
                }
            }
#ifdef HYB_MULTI
            const uint8_t *ep = ed_part ? SB.patch : S.patch;
#else
            const uint8_t *ep = S.patch;
#endif
            for (y = 0; !stat && y < 240u; y++)   /* (the engine: at the next block, rzis alone) */
                if (y != 7u && y != 129u && y != 144u && ep[y] != shadow[y]) keep++;
        }
        if (stat) { rz_apply(o0); }
        {
            const uint32_t avg = moved / rolls;
            const int f = bad || keep || (stat && (avg < (uint32_t)atoi(a) || avg > (uint32_t)atoi(b)));
            printf("%s %s (rolls %u, bytes moved %u a roll, out of bounds %u, engine apart %u)\n", f ? "FAIL" : "  ok",
                   cmd, (unsigned)rolls, (unsigned)avg, (unsigned)bad, (unsigned)keep);
        }
        return 1;
    }
#endif
    if (!strcmp(cmd, "smark") || !strcmp(cmd, "shashis")) {   /* (after 0.98) the edit buffer's bytes (and the
                                          * engine's) hashed into mark a (0..3), or checked as mark a (b 1) or not (0) */
        static uint32_t mk[4];
        uint32_t h = 2166136261u, k;
        for (k = 0; k < 240u; k++) h = (h ^ shadow[k]) * 16777619u;
        if (cmd[0] == 's' && cmd[1] == 'm') { mk[atoi(a) & 3] = h; printf("  smark %s %08x\n", a, (unsigned)h); return 1; }
        printf("%s shashis %s %s (%08x)\n", (h == mk[atoi(a) & 3]) != atoi(b) ? "FAIL" : "  ok", a, b, (unsigned)h);
        return 1;
    }
    if (!strcmp(cmd, "editedis")) {      /* a check (0.89; every build since 0.96): the patch up has edits (a 1: SAVE lit) or none (0); FAIL else */
        printf("%s editedis %s (%d)\n", em_edited(set.patch) != atoi(a) ? "FAIL" : "  ok", a, em_edited(set.patch));
        return 1;
    }
    if (!strcmp(cmd, "pedit")) {         /* (0.96) patch byte a to b as the panel's edit: pset's, and in the edit memory
                                          * (the patch edited, editedis) */
        em_set(set.patch, (uint32_t)atoi(a) & 255u, (uint32_t)atoi(b));
#ifdef HYB_MULTI
        if (ed_part) {                   /* (after 0.98: part B edited, its engine's) */
            shadow[atoi(a) & 255] = (uint8_t)atoi(b);
            ev_put(EV_PARAM_B, (uint32_t)atoi(a) & 255u, (uint32_t)atoi(b));
            return 1;
        }
#endif
        cmd = "pset";
    }
    if (!strcmp(cmd, "pset")) {          /* patch byte a to b, as an edit through the parameter path (part A's) */
        shadow[atoi(a) & 255] = (uint8_t)atoi(b);
#ifdef HYB_MULTI
        ev_put(EV_PARAM_A, (uint32_t)atoi(a) & 255u, (uint32_t)atoi(b));
#else
        ev_put(EV_PARAM, (uint32_t)atoi(a) & 255u, (uint32_t)atoi(b));
#endif
        return 1;
    }
    if (!strcmp(cmd, "arpis")) {         /* a check: part A's arpeggiator playing (a 1) or not (a 0); FAIL else */
        printf("%s arpis %s (mode %d on %d)\n", (S.patch[129] && S.arp_on) != (atoi(a) != 0) ? "FAIL" : "  ok", a,
               S.patch[129], S.arp_on);
        return 1;
    }
    if (!strcmp(cmd, "arpn")) {          /* part A's arpeggiator: on, the note it plays, its step; the voices gated */
        int k;
#ifdef HYB_MULTI
        const int nv = S.nv;
#else
        const int nv = SY_MAX_VOICES;
#endif
        printf("  arpn mode %d on %d note %d i %d v[", S.patch[129], S.arp_on, S.arp_note, S.arp_i);
        for (k = 0; k < nv; k++) if (S.v[k].gate) printf(" %d", (int)(S.v[k].target + 0.5f));
        printf(" ]\n");
        return 1;
    }
    if (!strcmp(cmd, "arpkeys")) {       /* (1.02) a check: the keys part A's arpeggiator holds, a "60,64,67" ("-":
                                          * none; FAIL else) */
        char w[160] = "";
        int k;
        for (k = 0; k < 128; k++)
            if (S.arp_vel[k]) sprintf(w + strlen(w), "%s%d", w[0] ? "," : "", k);
        if (!w[0]) strcpy(w, "-");
        printf("%s arpkeys %s (on %d)\n", strcmp(w, a) ? "FAIL" : "  ok", w, S.arp_on);
        return 1;
    }
    if (!strcmp(cmd, "arpord")) {        /* (1.08) a check: part A's arpeggiator's keys in their order (as played: its
                                          * AS PLAYED list), a "62,65,69" ("-": none); each once and the same keys as
                                          * it holds (arpkeys); FAIL else */
        char w[160] = "";
        int k, cnt = 0, bad = 0, j;
        for (k = 0; k < S.arp_n; k++) {
            sprintf(w + strlen(w), "%s%d", w[0] ? "," : "", S.arp_ord[k]);
            if (!S.arp_vel[S.arp_ord[k]]) bad = 1;
            for (j = 0; j < k; j++) if (S.arp_ord[j] == S.arp_ord[k]) bad = 1;
        }
        for (k = 0; k < 128; k++) cnt += S.arp_vel[k] != 0;
        if (cnt != S.arp_n) bad = 1;
        if (!w[0]) strcpy(w, "-");
        printf("%s arpord %s (held %d%s)\n", bad || strcmp(w, a) ? "FAIL" : "  ok", w, cnt, bad ? ", the list and the keys differ" : "");
        return 1;
    }
    if (!strcmp(cmd, "rlnotes")) {       /* (1.07) a check: the pitches the roll shows for the step selected (rl_notes:
                                          * "60,64,67"; "-" none, CHORD ENTRY with every note toggled out); FAIL else */
        int32_t p[SQ_CHV];
        char w[64] = "";
        uint32_t k, n = rl_notes(&sqp.st[sqv.sel], p);
        for (k = 0; k < n; k++) sprintf(w + strlen(w), "%s%d", k ? "," : "", (int)p[k]);
        if (!w[0]) strcpy(w, "-");
        printf("%s rlnotes step %d: %s\n", strcmp(w, a) ? "FAIL" : "  ok", sqv.sel + 1, w);
        return 1;
    }
    if (!strcmp(cmd, "sqch")) {          /* (1.02) a check: the shown arp pattern's step a (1..64) on, a CHORD of the
                                          * offsets b "0,4,7" from the base ("-": none recorded, the base alone); c (if
                                          * given) its chord's index (1..8); FAIL else */
        const sq_step_t *t = &sqp.st[(uint32_t)(atoi(a) - 1) & 63u];
        char w[64] = "";
        uint32_t k;
        if ((uint8_t)t->off < SQ_CHN)
            for (k = 0; k < SQ_CHV && sqp.ch[(uint8_t)t->off][k]; k++)
                sprintf(w + strlen(w), "%s%d", k ? "," : "", (int)sqp.ch[(uint8_t)t->off][k] - 64);
        if (!w[0]) strcpy(w, "-");
        printf("%s sqch %s ev %d chord %d: %s\n", !(t->ev & SQ_ON) || (t->ev & 15u) != SQ_CHORD || strcmp(w, b)
               || (c[0] && t->off + 1 != atoi(c)) ? "FAIL" : "  ok", a, t->ev, t->off + 1, w);
        return 1;
    }
    if (!strcmp(cmd, "ceis")) {          /* (1.03) a check: CHORD ENTRY on (a 1) or off (0); on: its step b (1..64), the
                                          * notes chosen c (offsets from the base, "0,4,7"; "-" none yet; "empty" all
                                          * toggled out, 1.07), the keyboard's
                                          * octave d (each if given); FAIL else */
        char w[64] = "";
        uint32_t k;
        const int on = SQ_CE != 0;
        for (k = 0; k < sqv.chn; k++) sprintf(w + strlen(w), "%s%d", k ? "," : "", (int)sqv.cht[k] - 64);
        if (!w[0]) strcpy(w, sqv.ce & SQ_CEE ? "empty" : "-");   /* (1.07: every note toggled out) */
        printf("%s ceis %s on %d step %d notes %s oct %d\n", on != atoi(a) || (on && b[0] && sqv.sel + 1 != atoi(b))
               || (on && c[0] && strcmp(w, c)) || (d[0] && set.oct != atoi(d)) ? "FAIL" : "  ok", a, on, sqv.sel + 1, w,
               set.oct);
        return 1;
    }
    if (!strcmp(cmd, "sqoff")) {         /* (1.18C31) a check: the shown arp pattern's step a (1..64) has the stored off
                                          * (NOTE's OFFSET; CHORD's chord index) b; FAIL else */
        const sq_step_t *t = &sqp.st[(uint32_t)(atoi(a) - 1) & 63u];
        printf("%s sqoff %s off %d (ev %02x)\n", t->off != atoi(b) ? "FAIL" : "  ok", a, t->off, t->ev);
        return 1;
    }
    if (!strcmp(cmd, "stepis")) {        /* (1.18C31) a check: the step whose view a sequencer's page shows (a 1..64; 0
                                          * none: the page as it is), its key let go and the view kept a while (b 1)
                                          * or held (0; "" no check); FAIL else */
        const int st = sqv.key ? sqv.sel + 1 : 0;
        printf("%s stepis %s step %d let go %d\n", st != atoi(a) || (b[0] && st && (sqv.lin != 0) != atoi(b))
               ? "FAIL" : "  ok", a, st, sqv.lin != 0);
        return 1;
    }
    if (!strcmp(cmd, "sqchs")) {         /* (1.02) the shown arp pattern's chords (the table), a "n" the ones not empty
                                          * (FAIL else; "" no check) */
        uint32_t i, k, n = 0;
        printf("  sqchs");
        for (i = 0; i < SQ_CHN; i++) {
            if (!sqp.ch[i][0]) continue;
            n++;
            printf(" [%u:", (unsigned)i + 1u);
            for (k = 0; k < SQ_CHV && sqp.ch[i][k]; k++) printf(" %d", (int)sqp.ch[i][k] - 64);
            printf("]");
        }
        printf("\n%s sqchs %u\n", a[0] && n != (uint32_t)atoi(a) ? "FAIL" : "  ok", (unsigned)n);
        return 1;
    }
    if (!strcmp(cmd, "sqev")) {          /* (1.02) a check: the shown arp pattern's step a (1..64) has the stored event
                                          * code b (0 FING POLY, 1 NOTE, 2-9 FING 1-8, 10 CHORD) and the EVENT column
                                          * reads c ("F_POLY": _ a space; if given); FAIL else */
        const sq_step_t *t = &sqp.st[(uint32_t)(atoi(a) - 1) & 63u];
        char x[16], y[24] = "";
        uint32_t k, mx, bi;
        const uint8_t sel = sqv.sel;
        for (k = 0; k < 23u && c[k]; k++) y[k] = c[k] == '_' ? ' ' : c[k];
        sqv.sel = (uint8_t)((atoi(a) - 1) & 63);
        sq_cv(1u, x, &mx, &bi);
        sqv.sel = sel;
        printf("%s sqev %s code %d text \"%s\" (alneg %d)\n", (int)(t->ev & 15u) != atoi(b) || (c[0] && strcmp(x, y))
               ? "FAIL" : "  ok", a, t->ev & 15u, x, (t->ev & SQ_ALNEG) != 0);
        return 1;
    }
    if (!strcmp(cmd, "sqon")) {          /* (1.02) a check: arp sequencer a's (0 A, 1 B) notes sounding (its own: nt[])
                                          * are b "60,64,67" ("-" none), each "L" if latched (c "L": all of them, "-":
                                          * none latched; if given); FAIL else */
        char w[160] = "";
        int k, lat = 0, any = 0;
        uint32_t i;
#ifdef HYB_MULTI
        const sq_rt_t *q = &sqr[atoi(a) & 1];
#else
        const sq_rt_t *q = &sq;
#endif
        for (k = 0; k < 128; k++)
            for (i = 0; i < SQ_NT; i++)
                if (q->nt[i].live && q->nt[i].note == k) {
                    sprintf(w + strlen(w), "%s%d%s", w[0] ? "," : "", k, q->nt[i].live == SQ_LVL ? "L" : "");
                    any++;
                    lat += q->nt[i].live == SQ_LVL;
                }
        {
            char v[160];
            uint32_t j = 0;
            for (k = 0; w[k]; k++) if (w[k] != 'L') v[j++] = w[k];
            v[j] = 0;
            if (!v[0]) strcpy(v, "-");
            printf("%s sqon %s: %s\n", strcmp(v, b) || (c[0] == 'L' && lat != any) || (c[0] == '-' && lat) ? "FAIL" : "  ok",
                   a, w[0] ? w : "-");
        }
        return 1;
    }
    if (!strcmp(cmd, "ubis")) {          /* (1.02) a check: user slot a (0 .. UB_N - 1) filled (b 1) or empty (0), its
                                          * name starting with c (if given); FAIL else */
        char nm[10];
        const uint32_t u = (uint32_t)atoi(a);
        const int f = ub_filled(u);
        nm[0] = 0;
        if (f) ub_name(u, nm);
        printf("%s ubis %s filled %d name %.10s\n", f != atoi(b) || (c[0] && strncmp(nm, c, strlen(c))) ? "FAIL" : "  ok",
               a, f, nm);
        return 1;
    }
    if (!strcmp(cmd, "svpend")) {        /* (1.02) a check: the patch's SAVE pending (a 1, its slot b if given) or not
                                          * (0); FAIL else */
        const int p = (sqv.sv_w & 4u) != 0;
        printf("%s svpend %s %s (pending %d slot %u)\n", p != atoi(a) || (b[0] && p && sqv.svpw[0] != atoi(b)) ? "FAIL"
               : "  ok", a, b, p, (unsigned)sqv.svpw[0]);
        return 1;
    }
    if (!strcmp(cmd, "sqlastv")) {       /* (1.02) a check: arp sequencer a's last note on had velocity b (FAIL else) */
        const uint32_t i = (uint32_t)atoi(a) & 1u;
        printf("%s sqlastv %u vel %u\n", sq_lastv[i] != (uint32_t)atoi(b) ? "FAIL" : "  ok", (unsigned)i, (unsigned)sq_lastv[i]);
        return 1;
    }
    if (!strcmp(cmd, "sqlastoff")) {     /* (1.02) a check: arp sequencer a's last note-off at step b (1..64) + c eighths
                                          * (the transport's ticks from its start, in the loop; FAIL else) */
        const uint32_t i = (uint32_t)atoi(a) & 1u, w = (uint32_t)(atoi(b) - 1) * 8u + (uint32_t)atoi(c);
        printf("%s sqlastoff %u step %u + %u/8\n", sq_lasto[i] != w ? "FAIL" : "  ok", (unsigned)i,
               (unsigned)(sq_lasto[i] >> 3) + 1u, (unsigned)(sq_lasto[i] & 7u));
        return 1;
    }
    if (!strcmp(cmd, "objsz")) {         /* (1.02) a check: store object a's size is b (-1: none; FAIL else). a: "seq" the
                                          * arp patterns' heads (OBJ_SEQ), or a number */
        const uint32_t o = !strcmp(a, "seq") ? OBJ_SEQ : (uint32_t)atoi(a);
        const int n = plat_store_size(o);
        printf("%s objsz %s %d\n", n != atoi(b) ? "FAIL" : "  ok", a, n);
        return 1;
    }
    if (!strcmp(cmd, "gset")) {          /* (1.02) a check: the keys of part A's voices gated in the engine, a set
                                          * "60,64,67" ("-" none; FAIL else) */
        char w[160] = "";
        int k, i;
#ifdef HYB_MULTI
        const int nv = S.nv;
#else
        const int nv = SY_MAX_VOICES;
#endif
        for (k = 0; k < 128; k++)
            for (i = 0; i < nv; i++)
                if (S.v[i].on && S.v[i].gate && S.v[i].key == k) { sprintf(w + strlen(w), "%s%d", w[0] ? "," : "", k); break; }
        if (!w[0]) strcpy(w, "-");
        printf("%s gset %s\n", strcmp(w, a) ? "FAIL" : "  ok", w);
        return 1;
    }
    if (!strcmp(cmd, "gsetb")) {         /* (1.15) a check: as gset, part B's (the builds with two parts; else "-") */
        char w[160] = "";
#ifdef HYB_MULTI
        int k, i;
        for (k = 0; k < 128; k++)
            for (i = 0; i < SB.nv; i++)
                if (SB.v[i].on && SB.v[i].gate && SB.v[i].key == k) { sprintf(w + strlen(w), "%s%d", w[0] ? "," : "", k); break; }
#endif
        if (!w[0]) strcpy(w, "-");
        printf("%s gsetb %s\n", strcmp(w, a) ? "FAIL" : "  ok", w);
        return 1;
    }
    if (!strcmp(cmd, "noteoutc")) {      /* (1.15) a check: note b on out over MIDI on channel a (1..16) now (c 1) or
                                          * not (c 0); FAIL else */
        const uint32_t on = host_non[(atoi(a) - 1) & 15][atoi(b) & 127];
        printf("%s noteoutc ch %s %s %u\n", (on != 0) != (atoi(c) != 0) ? "FAIL" : "  ok", a, b, (unsigned)on);
        return 1;
    }
    if (!strcmp(cmd, "passes")) { host_spin = (uint32_t)atoi(a); return 1; }   /* (1.18C46) see host_spin */
    if (!strcmp(cmd, "msodd")) {         /* (1.18C46) a check: the time now (plat_ms) odd (a 1) or even (a 0); FAIL else */
        printf("%s msodd %s (%u)\n", (plat_ms() & 1u) != (uint32_t)atoi(a) ? "FAIL" : "  ok", a, (unsigned)plat_ms());
        return 1;
    }
    if (!strcmp(cmd, "ltfade")) {        /* a check (1.18C46): from a to b ms (the trace's time) the keys' fade level
                                          * (plat_lights_fade) never falls and takes at least c levels (0 none counted);
                                          * d "night": no key lit, else every key lit at the end of the span; the
                                          * levels printed; FAIL else */
        uint32_t i, t0 = host_ltn ? host_lte[0].t : 0u, last = 0, n = 0, bad = 0, j;
        int lit_end = -1;
        printf("LTFADE");
        for (i = 0; i < host_ltn && i < HOST_LTN; i++) {
            const uint32_t t = host_lte[i].t - t0, lv = host_lte[i].lv;
            if (t < (uint32_t)atoi(a) || t >= (uint32_t)atoi(b)) continue;
            if (lv && lv != last) { printf(" %u@%u", (unsigned)lv, (unsigned)t); n++; }
            if (lv && lv < last) bad = 1;
            if (lv) last = lv;
            for (j = 0, lit_end = 1; j < NKEYS; j++) lit_end &= host_lte[i].k[j] == '#';
            for (j = 0; j < NKEYS && !strcmp(d, "night"); j++) bad |= host_lte[i].k[j] == '#';
        }
        printf("\n%s ltfade %s %s %s %s: %u levels, %u calls%s\n", bad || n < (uint32_t)atoi(c)
               || (strcmp(d, "night") && lit_end != 1) ? "FAIL" : "  ok", a, b, c, d, (unsigned)n, (unsigned)host_fade_n,
               bad ? ", falls or lit" : "");
        return 1;
    }
    if (!strcmp(cmd, "ltfadeoff")) {     /* a check (1.18C46): no key at the fade's level now (plat_lights_fade ended); FAIL
                                          * else */
        printf("%s ltfadeoff (keys %08x level %u)\n", host_lt[6] ? "FAIL" : "  ok", (unsigned)host_lt[6],
               (unsigned)host_lt[7]);
        return 1;
    }
    if (!strcmp(cmd, "lbstart")) { lb_st = 1; host_ltn = 0; return 1; }   /* (1.18C43) the power-on show again, as at a
                                          * power-on (the host's "reboot" keeps the RAM: not the show), its trace anew */
    if (!strcmp(cmd, "lttrace")) {       /* (1.18C43) the lights' changes kept since the boot (or lbstart): time (ms from
                                          * the first), keys, buttons (host_lt_str) */
        uint32_t i, t0 = host_ltn ? host_lte[0].t : 0u;
        for (i = 0; i < host_ltn && i < HOST_LTN; i++)
            printf("LT %5u K %s B %s\n", (unsigned)(host_lte[i].t - t0), host_lte[i].k, host_lte[i].b);
        return 1;
    }
    if (!strcmp(cmd, "ltkmax")) {        /* a check (1.18C43): from a to b ms (the trace's time) at most c keys not dark at
                                          * once; FAIL else */
        uint32_t i, j, mx = 0, t0 = host_ltn ? host_lte[0].t : 0u;
        for (i = 0; i < host_ltn && i < HOST_LTN; i++) {
            const uint32_t t = host_lte[i].t - t0;
            uint32_t n = 0;
            if (t < (uint32_t)atoi(a) || t >= (uint32_t)atoi(b)) continue;
            for (j = 0; j < NKEYS; j++) n += host_lte[i].k[j] != '.';
            if (n > mx) mx = n;
        }
        printf("%s ltkmax %s %s %s (%u)\n", mx > (uint32_t)atoi(c) || !mx ? "FAIL" : "  ok", a, b, c, (unsigned)mx);
        return 1;
    }
    if (!strcmp(cmd, "ltorder")) {       /* a check (1.18C43): before a ms the keys come on (not dark) for the first time
                                          * from F3 to G5, each later than the one before, and each goes through the glow,
                                          * the middle level (b 1: and lit; b 0: never lit) and back to dark; the buttons by columns from
                                          * GLO / REC to OCT- / OCT+ (lt_frame's LB_COL), later each, both of a column
                                          * always at one level; FAIL else */
        static const uint8_t col[LB_NCOL][2] = {{B_GLO, B_REC}, {B_EDIT, B_PLAY}, {B_LFO, B_SEQ}, {B_ENV, B_ARP},
                                                 {B_SEL, B_SAVE}, {B_FX, B_HOME}, {B_OCTDN, B_OCTUP}};
        uint32_t i, j, t0 = host_ltn ? host_lte[0].t : 0u, kf[NKEYS], cf[LB_NCOL], seen[NKEYS], pair = 0, bad = 0;
        const uint32_t lim = (uint32_t)atoi(a), need = atoi(b) ? 7u : 3u;   /* (bits: glow, middle, lit seen) */
        for (j = 0; j < NKEYS; j++) kf[j] = ~0u, seen[j] = 0;
        for (j = 0; j < LB_NCOL; j++) cf[j] = ~0u;
        for (i = 0; i < host_ltn && i < HOST_LTN; i++) {
            const uint32_t t = host_lte[i].t - t0;
            if (t >= lim) break;
            for (j = 0; j < NKEYS; j++) {
                const char c = host_lte[i].k[j];
                if (c != '.' && kf[j] == ~0u) kf[j] = t;
                seen[j] |= c == 'd' ? 1u : c == 'm' ? 2u : c == '#' ? 4u : 0u;
            }
            for (j = 0; j < LB_NCOL; j++) {
                const char c0 = host_lte[i].b[col[j][0]], c1 = host_lte[i].b[col[j][1]];
                if (c0 != c1) pair++;
                if (c0 != '.' && cf[j] == ~0u) cf[j] = t;
            }
        }
        for (j = 0; j < NKEYS; j++)
            if (kf[j] == ~0u || (j && kf[j] <= kf[j - 1]) || (seen[j] & need) != need || (!atoi(b) && (seen[j] & 4u)))
                bad |= 1u;                       /* (b 0, NIGHT: never lit) */
        for (j = 0; j < LB_NCOL; j++)
            if (cf[j] == ~0u || (j && cf[j] <= cf[j - 1])) bad |= 2u;
        printf("%s ltorder %s %s: keys first at %u .. %u ms, columns at", bad || pair ? "FAIL" : "  ok", a, b,
               (unsigned)kf[0], (unsigned)kf[NKEYS - 1]);
        for (j = 0; j < LB_NCOL; j++) printf(" %u", (unsigned)cf[j]);
        printf(" ms; pairs apart %u, bad %u\n", (unsigned)pair, (unsigned)bad);
        return 1;
    }
    if (!strcmp(cmd, "ltis")) {          /* a check (1.18C43): the lights now: keys a, buttons b (host_lt_str; '-' any one,
                                          * b "-": not checked); FAIL else */
        char k[NKEYS + 1], bb[NB + 1];
        uint32_t i, bad = strlen(a) != NKEYS || (strcmp(b, "-") && strlen(b) != NB);
        host_lt_str(k, bb);
        for (i = 0; !bad && i < NKEYS; i++) bad = a[i] != '-' && a[i] != k[i];
        for (i = 0; !bad && strcmp(b, "-") && i < NB; i++) bad = b[i] != '-' && b[i] != bb[i];
        printf("%s ltis %s %s (%s %s)\n", bad ? "FAIL" : "  ok", a, b, k, bb);
        return 1;
    }
    if (!strcmp(cmd, "ltbtnseen")) {     /* a check (1.18C43): button a (0 FX .. 13 OCT+, plat.h's B_*) took the levels b
                                          * ("#d": lit and the glow, each at least once, no other) in the trace's last c
                                          * ms (its level at their start counted); FAIL else */
        const uint32_t bi = (uint32_t)atoi(a) % NB, now = plat_ms(), w = (uint32_t)atoi(c);
        char seen[8] = {0};
        uint32_t i, n = 0, bad = 0;
        for (i = host_ltn; i-- > 0 && host_ltn - i <= HOST_LTN;) {
            const char l = host_lte[i % HOST_LTN].b[bi];
            if (!strchr(seen, l) && n < 7u) seen[n++] = l;
            if (now - host_lte[i % HOST_LTN].t > w) break;
        }
        for (i = 0; i < n; i++) bad |= !strchr(b, seen[i]);
        for (i = 0; b[i]; i++) bad |= !strchr(seen, b[i]);
        printf("%s ltbtnseen %s %s %s (%s)\n", bad ? "FAIL" : "  ok", a, b, c, seen);
        return 1;
    }
    if (!strcmp(cmd, "ltnolit")) {       /* a check (1.18C43, NIGHT): no light lit now (the middle level at most); FAIL else */
        char k[NKEYS + 1], bb[NB + 1];
        host_lt_str(k, bb);
        printf("%s ltnolit (%s %s)\n", strchr(k, '#') || strchr(bb, '#') ? "FAIL" : "  ok", k, bb);
        return 1;
    }
    if (!strcmp(cmd, "ltlast")) {        /* a check (1.18C43): the lights' last change (the trace's time) from a to b ms;
                                          * FAIL else */
        const uint32_t t = host_ltn ? host_lte[(host_ltn - 1u) % HOST_LTN].t - host_lte[0].t : 0u;
        printf("%s ltlast %s %s (%u, %u changes)\n", t < (uint32_t)atoi(a) || t > (uint32_t)atoi(b) ? "FAIL" : "  ok",
               a, b, (unsigned)t, (unsigned)host_ltn);
        return 1;
    }
    if (!strcmp(cmd, "keydarkis")) {     /* a check (1.18C43): KEY LIGHTS is a (0 ON, 1 OFF, 2 NIGHT); FAIL else */
        printf("%s keydarkis %s (%u)\n", set.key_dark != (uint8_t)atoi(a) ? "FAIL" : "  ok", a, (unsigned)set.key_dark);
        return 1;
    }
    if (!strcmp(cmd, "btnis")) {         /* a check (1.05): button a (REC, HOME, SAVE, EDIT, PLAY, SEQ) lit (b 1) or not (0)
                                          * as input() last set the lights; FAIL else */
        static const char *const nm[] = {"REC", "HOME", "SAVE", "EDIT", "PLAY", "SEQ"};
        const uint32_t bi[] = {B_REC, B_HOME, B_SAVE, B_EDIT, B_PLAY, B_SEQ};
        uint32_t i;
        for (i = 0; i < 6u && strcmp(nm[i], a); i++)
            ;
        if (i == 6u) { printf("FAIL btnis %s: no such button\n", a); return 1; }
        printf("%s btnis %s %d (%d)\n", (int)(host_lb >> bi[i] & 1u) != atoi(b) ? "FAIL" : "  ok", a, atoi(b),
               (int)(host_lb >> bi[i] & 1u));
        return 1;
    }
    if (!strcmp(cmd, "sqvelis")) {       /* a check (1.05): the shown arp pattern's step a (1..64) has VEL b; FAIL else */
        const sq_step_t *t = &sqp.st[(uint32_t)(atoi(a) - 1) & 63u];
        printf("%s sqvelis %s vel %d\n", t->vel != atoi(b) ? "FAIL" : "  ok", a, t->vel);
        return 1;
    }
    if (!strcmp(cmd, "sqplclr")) {       /* (1.05) the notes each step played, forgotten ("sqpl") */
        uint32_t i, k;
        for (i = 0; i < 2u; i++)
            for (k = 0; k < SQ_NMAX; k++) { sq_hplt[i][k] = 0; sq_hpl[i][k][0] = 0; }
        return 1;
    }
    if (!strcmp(cmd, "sqpl")) {          /* a check (1.05): arp sequencer a's (0 A, 1 B) step b (1..64) played the notes
                                          * c ("60,64,67" in their order; "-": none since sqplclr) the last time it
                                          * played; FAIL else */
        const uint32_t i = (uint32_t)atoi(a) & 1u, k = (uint32_t)(atoi(b) - 1) & 63u;
        char w[64] = "";
        uint32_t j;
        for (j = 0; j < 8u && sq_hpl[i][k][j]; j++) sprintf(w + strlen(w), "%s%d", j ? "," : "", sq_hpl[i][k][j] - 1);
        if (!w[0]) strcpy(w, "-");
        printf("%s sqpl %u step %s: %s\n", strcmp(w, c) ? "FAIL" : "  ok", (unsigned)i, b, w);
        return 1;
    }
    if (!strcmp(cmd, "mcclr")) { host_nmc = 0; return 1; }   /* (1.18C40) the metronome's clicks afresh */
    if (!strcmp(cmd, "mcis")) {          /* (1.18C40) a check: a clicks of the metronome since mcclr, click k at the
                                          * clock's tick b + k c (signed: PRECOUNT's before 0), accented on b's bar
                                          * and every fourth after it (b a bar's start); FAIL else */
        const int32_t n = atoi(a), t0 = atoi(b), bt = atoi(c);
        int32_t k, bad = (int32_t)host_nmc != n;
        printf("  mcis %u clicks:", (unsigned)host_nmc);
        for (k = 0; k < (int32_t)host_nmc && k < (int32_t)HOST_NMC; k++) {
            printf(" %d%s", (int)host_mc[k], host_mca[k] ? "!" : "");
            if (host_mc[k] != t0 + k * bt || (host_mca[k] != 0) != !(k & 3)) bad = 1;
        }
        printf("\n%s mcis %d at %d + k %d\n", bad ? "FAIL" : "  ok", (int)n, (int)t0, (int)bt);
        return 1;
    }
    if (!strcmp(cmd, "lastmis")) {       /* (1.18C40) a check: the clock's tick (signed) of arp sequencer a's last note
                                          * on (a 0 A, 1 B; "d": the drums' last step hit) is b; FAIL else */
        const int32_t m = a[0] == 'd' ? (int32_t)dr_hm : (int32_t)sq_lastm[atoi(a) & 1];
        printf("%s lastmis %s tick %d\n", m != atoi(b) ? "FAIL" : "  ok", a, (int)m);
        return 1;
    }
    if (!strcmp(cmd, "ckis")) {          /* (1.18C40) a check: the shared clock runs (a 1) or not (0); b "neg": its tick
                                          * before 0 (a PRECOUNT's count), "pos": 0 or later, "-": either; FAIL else */
        const int bad = (int)ck.on != atoi(a) || (b[0] == 'n' && (int32_t)ck.mt >= 0) || (b[0] == 'p' && (int32_t)ck.mt < 0);
        printf("%s ckis on %d tick %d\n", bad ? "FAIL" : "  ok", ck.on, (int)(int32_t)ck.mt);
        return 1;
    }
    if (!strcmp(cmd, "sqnclr")) { sq_hon[0] = sq_hon[1] = dr_hon = 0; return 1; }   /* (0.94) the counts afresh */
    if (!strcmp(cmd, "sqnis")) {         /* a check (0.94): the note-ons of arp sequencer a (0 A, 1 B) since sqnclr: b
                                          * of them ("+": one at least); the drums' step hits: c ("+": one at least,
                                          * "-": any); FAIL else */
        const int i = atoi(a) & 1, n = sq_hon[i];
        const int bad = (b[0] == '+' ? n < 1 : n != atoi(b))
                        || (c[0] == '+' ? dr_hon < 1 : c[0] && c[0] != '-' && dr_hon != atoi(c));
        printf("%s sqnis seq %d notes %d drums %d\n", bad ? "FAIL" : "  ok", i, n, dr_hon);
        return 1;
    }
    if (!strcmp(cmd, "gated")) {         /* a check (0.94): the voices gated in the engine (both parts), each on note
                                          * a (-1: none gated); FAIL else */
        int k, bad = 0, i;
#ifdef HYB_MULTI
        const sy_synth *P[2] = {&S, &SB};
        const int np = 2;
#else
        const sy_synth *P[1] = {&S};
        const int np = 1;
#endif
        printf("  gated:");
        for (i = 0; i < np; i++)
#ifdef HYB_MULTI
            for (k = 0; k < P[i]->nv; k++)
#else
            for (k = 0; k < SY_MAX_VOICES; k++)
#endif
                if (P[i]->v[k].on && P[i]->v[k].gate) {
                    printf(" %c%d", 'A' + i, (int)P[i]->v[k].key);
                    if (P[i]->v[k].key != atoi(a)) bad = 1;
                }
        printf("\n%s gated %s\n", bad ? "FAIL" : "  ok", a);
        return 1;
    }
    if (!strcmp(cmd, "gatedis")) {       /* (1.18C44) a check: part a's (0 A, 1 B) voices gated in the engine are on the
                                          * notes b ("60,64,67" lowest first, each once; "-" none) and no others; FAIL else */
        char w[160] = "";
        int k, i, g;
#ifdef HYB_MULTI
        const sy_synth *P = atoi(a) ? &SB : &S;
        const int nv = P->nv;
#else
        const sy_synth *P = &S;
        const int nv = SY_MAX_VOICES;
#endif
        for (k = 0; k < 128; k++) {
            for (i = g = 0; i < nv; i++) g |= P->v[i].on && P->v[i].gate && P->v[i].key == k;
            if (g) sprintf(w + strlen(w), "%s%d", w[0] ? "," : "", k);
        }
        if (!w[0]) strcpy(w, "-");
        printf("%s gatedis part %s %s\n", strcmp(w, b) ? "FAIL" : "  ok", a, w);
        return 1;
    }
    if (!strcmp(cmd, "audis")) {         /* (1.18C44) a check: the step held's audition (sqv.aud) is a (0 not yet, 1 tried,
                                          * nothing heard, 2 heard; "-" either) and one sustained sounds now (b 1) or not (0); FAIL else */
        const int on = sq_pv.n && (sq_pv.f & SQ_PV_AUD);
        printf("%s audis %d sustained %d\n", (a[0] != '-' && sqv.aud != atoi(a)) || on != atoi(b) ? "FAIL" : "  ok",
               sqv.aud, on);
        return 1;
    }
    if (!strcmp(cmd, "lastmmod")) {      /* (1.18C44) a check: the clock's tick of arp sequencer a's last note on is c
                                          * modulo b (on the grid); FAIL else */
        const int32_t m = (int32_t)sq_lastm[atoi(a) & 1], r = ((m % atoi(b)) + atoi(b)) % atoi(b);
        printf("%s lastmmod %s tick %d mod %s = %d\n", r != atoi(c) ? "FAIL" : "  ok", a, (int)m, b, (int)r);
        return 1;
    }
    if (!strcmp(cmd, "pvclr")) { pv_hn = pv_hbad = 0; dr_hpn = 0; return 1; }   /* (0.85) the previews counted afresh */
    if (!strcmp(cmd, "pvis")) {          /* a check (0.85): the arp step previews since pvclr: a of them, the last note
                                          * b (MIDI number), its part c ("-" either; a build with one part: "1" skips
                                          * the check), one sounding now d (0 / 1, and then a voice of its part gated on
                                          * it in the engine), none started over another; FAIL else */
        int bad = pv_hbad != 0 || pv_hn != atoi(a) || (pv_hn && pv_hlast != atoi(b)) || pv_hlive != (atoi(d) != 0)
                  || (pv_hlive != (sq_pv.n != 0));
#ifdef HYB_MULTI
        if (c[0] != '-' && pv_hn && pv_hpart != atoi(c)) bad = 1;
        if (pv_hlive) {
            const sy_synth *P = pv_hpart ? &SB : &S;
            int k, g = 0;
            for (k = 0; k < P->nv; k++) g |= P->v[k].on && P->v[k].gate && P->v[k].key == pv_hlast;
            if (!g) bad = 1;
        }
#else
        if (c[0] == '1') {
            printf("  skip pvis (one part)\n");
            return 1;
        }
        if (pv_hlive) {
            int k, g = 0;
            for (k = 0; k < SY_MAX_VOICES; k++) g |= S.v[k].on && S.v[k].gate && S.v[k].key == pv_hlast;
            if (!g) bad = 1;
        }
#endif
        printf("%s pvis n %d last %d part %d vel %d gate %d ms live %d over %d\n", bad ? "FAIL" : "  ok", pv_hn,
               pv_hlast, pv_hpart, pv_hvel, pv_hms, pv_hlive, pv_hbad);
        return 1;
    }
    if (!strcmp(cmd, "drpvis")) {        /* a check (0.85): the drums' previews since pvclr: a of them, the last
                                          * instrument b (0 BD ..), its ACCENT c, its LENGTH d (0..7); FAIL else */
        const int bad = dr_hpn != atoi(a) || (dr_hpn && ((dr_hpv & 15) != atoi(b) || (dr_hpv >> 8 & 3) != atoi(c)
                                                          || (dr_hpv >> 10 & 7) != atoi(d)));
        printf("%s drpvis n %d ins %d acc %d len %d\n", bad ? "FAIL" : "  ok", dr_hpn, dr_hpv & 15, dr_hpv >> 8 & 3,
               dr_hpv >> 10 & 7);
        return 1;
    }
    if (!strcmp(cmd, "bal")) {           /* a check, all quiet: no note out without its note-off (each channel), no
                                          * voice gated in the engine (each part), its arpeggiator off; FAIL else.
                                          * a: a label */
        uint32_t c, k, bad = 0;
        int i;
#ifdef HYB_MULTI
        sy_synth *P[2] = {&S, &SB};
        const int np = 2;
#else
        sy_synth *P[1] = {&S};
        const int np = 1;
#endif
        printf("  bal %s:", a);
        for (c = 0; c < 16u; c++)
            for (k = 0; k < 128u; k++)
                if (host_non[c][k]) { printf(" out ch%u n%u x%u", (unsigned)c + 1u, (unsigned)k, host_non[c][k]); bad++; }
        for (i = 0; i < np; i++) {
#ifdef HYB_MULTI
            const int nv = P[i]->nv;
#else
            const int nv = SY_MAX_VOICES;
#endif
            for (k = 0; k < (uint32_t)nv; k++)
                if (P[i]->v[k].on && P[i]->v[k].gate) { printf(" %c gated %d", 'A' + i, (int)P[i]->v[k].key); bad++; }
            if (P[i]->arp_on && P[i]->patch[129]) { printf(" %c arp on (note %d n %d)", 'A' + i, P[i]->arp_note, P[i]->arp_n); bad++; }
        }
        printf("\n%s bal %s\n", bad ? "FAIL" : "  ok", a);
        return 1;
    }
    (void)b;
    return 0;
}
