/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 J0KERPACK */
/* The development engine's screen (after the design canvas "FM-1 UI": a red LCD strip, the menus' rows of four knobs or a list of
 * lines in the big font, the status line). Part of app.c's unity build, after gfx.c: draws into the canvas (cv_*) and
 * blits. Each part is drawn only when it changed; ui_prof keeps the slowest draw of each part (microseconds), for the
 * budget. */

/* ------------------------------------------------------------- colours --- */
#define U_BG      RGB(11, 11, 12)
#define U_TEXT    RGB(232, 232, 234)
#define U_DIMTXT  RGB(125, 125, 132)
#define U_STATUS  RGB(154, 154, 160)
#define U_ROWOFF  RGB(19, 19, 21)
#define U_BORDER  RGB(36, 36, 40)
#define U_TRACK   RGB(44, 44, 49)
#define U_KNOB    RGB(29, 29, 33)
#define U_KNOBRIM RGB(54, 54, 60)
#define U_RULE    RGB(34, 34, 38)

#ifndef UI_GLOW
#define UI_GLOW 0                       /* the text's glow on the LCD strip: 2.5 ms a draw for little to see */
#endif
/* the colours that carry the accent: the design's red and orange; in the USB build (two parts) part
 * B edited turns them green, a green LED display (u_theme) */
typedef struct {
    uint16_t acc;                        /* the accent: lit rows, knobs' arcs, frames, tags, the list's cursor */
    uint16_t onacc;                      /* text on the accent */
    uint16_t valon;                      /* a lit knob's value */
    uint16_t lcdtxt, lcdglow, lcdrim, lcdbg;   /* the LCD strip: its text, glow, rim, background */
    uint16_t batt;                       /* HOME's battery bar */
    uint16_t led;                        /* a light on the screen lit (the LFOs') */
} u_theme_t;
#define U_THEME_A {RGB(255, 75, 31), RGB(20, 4, 0), RGB(255, 222, 206), RGB(255, 122, 82), RGB(150, 46, 24), \
                   RGB(122, 28, 16), RGB(90, 15, 8), RGB(220, 40, 28), RGB(255, 36, 24)}
/* the themes: part A's (0), part B's (1, the USB build), the drums' pages' (2, DRUM SEQ and DRUM KIT: an amber, the
 * drums belong to neither part); a pointer to the one up (4 bytes of RAM, set at the next draw: ui_theme_follow) */
static const u_theme_t U_THEMES[4] = {U_THEME_A,
    {RGB(60, 255, 110), RGB(2, 22, 8), RGB(206, 255, 220), RGB(110, 255, 140), RGB(30, 150, 60), RGB(16, 112, 40),
     RGB(8, 60, 20), RGB(40, 220, 80), RGB(60, 255, 90)},   /* (part B: a phosphor green, the red's steps) */
    {RGB(255, 189, 31), RGB(20, 14, 0), RGB(255, 241, 206), RGB(255, 204, 82), RGB(150, 113, 24), RGB(122, 91, 16),
     RGB(90, 66, 8), RGB(220, 164, 28), RGB(255, 187, 24)},   /* (the drums: part A's colours turned to amber, the
                                                               * hue 42 degrees, saturation and value kept) */
    {RGB(176, 176, 184), RGB(14, 14, 16), RGB(236, 236, 240), RGB(214, 214, 220), RGB(120, 120, 126),
     RGB(84, 84, 90), RGB(46, 46, 50), RGB(150, 150, 156), RGB(220, 220, 226)}};   /* (1.18C33: the favorites editor's
                                                               * greys, app.c fv_*) */
#define U_TH_DRUM 2u
#define U_TH_GREY 3u
static const u_theme_t *u_th = &U_THEMES[0];
#define U_TH (*u_th)
#define u_acc     U_TH.acc
#define U_ONACC   U_TH.onacc
#define U_VALON   U_TH.valon
#define U_LCDTXT  U_TH.lcdtxt
#define U_LCDGLOW U_TH.lcdglow
#define U_LCDRIM  U_TH.lcdrim
#define U_LCDBG   U_TH.lcdbg     /* the LCD strip: one colour (the design's gradient left out: cheaper) */
/* (1.18C35, the user's choice: the lists' and menus' cursor row (ls_line: GLO's menus, PRESET's list, the favorites,
 * SAVE's slots, the pickers; SAVE's letter) in the theme's dark accent step (its LCD glow: A 150, 46, 24) with its
 * label and value in white (the brightest); up to 1.18C34 the accent with dark text (acc, onacc), hard
 * to read. The other uses of the accent unchanged) */
#define U_CUR     U_TH.lcdglow
#define U_ONCUR   RGB(255, 255, 255)
#define U_CURVAL  U_ONCUR

static uint16_t mix565(uint16_t a, uint16_t b, uint32_t t)   /* a towards b by t / 256 */
{
    uint32_t ar = a >> 11, ag = (a >> 5) & 63u, ab = a & 31u, br = b >> 11, bg = (b >> 5) & 63u, bb = b & 31u;
    return (uint16_t)(((ar * (256u - t) + br * t) >> 8) << 11 | ((ag * (256u - t) + bg * t) >> 8) << 5 |
                      ((ab * (256u - t) + bb * t) >> 8));
}
static inline uint16_t cv_get(int32_t x, int32_t y) { return swap16(cv_px[(uint32_t)y * cv_w + (uint32_t)x]); }   /* (y: the
                                                     * canvas's own row) */
static inline void cv_blend(int32_t x, int32_t y, uint16_t c, uint32_t t)   /* (inside the canvas only) */
{
    y += cv_oy;                          /* (a band: draw_row) */
    if ((uint32_t)x >= cv_w || y < cv_y0 || y >= cv_y1 || !t)
        return;
    cv_px[(uint32_t)y * cv_w + (uint32_t)x] = swap16(t >= 256u ? c : mix565(cv_get(x, y), c, t));
}

/* -------------------------------------------------------------- knobs --- */
static float u_atan2(float y, float x)          /* within 0.3 degree */
{
    float ax = fm_fabsf(x), ay = fm_fabsf(y), a = fm_minf(ax, ay) / (fm_maxf(ax, ay) + 1e-9f), s = a * a, r;
    r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0.f) r = 3.14159274f - r;
    return y < 0.f ? -r : r;
}
static inline uint32_t cover(float d)           /* a pixel's coverage at signed distance d inside an edge */
{
    return d >= 0.5f ? 256u : d <= -0.5f ? 0u : (uint32_t)((d + 0.5f) * 256.f);
}
/* a knob, 30 x 30 at x, y: the track over 270 degrees from bottom left, the value's arc (from the start, or from the
 * top when bipolar), the cap and its pointer */
static UI_SMALL void knob(int32_t x, int32_t y, float v, int bi, int on, int32_t d)   /* d: its size in pixels (30 the
                                                     * design's; every radius scales with it) */
{
    const uint16_t arc = on ? u_acc : RGB(92, 92, 99), ptr = on ? C_WHITE : RGB(142, 142, 148);
    const float a = (0.75f + 1.5f * v) * 3.14159274f, ca = fm_cosf(a), sa = fm_sinf(a);
    const float lo = bi ? fm_minf(v, 0.5f) : 0.f, hi = bi ? fm_maxf(v, 0.5f) : v;
    const float k = (float)d * (1.f / 30.f), m = 0.5f * (float)(d - 1);
    int32_t i, j, j1 = cv_y1 - cv_oy - y;        /* (0.87: the rows of the canvas's band alone; a row is drawn
                                                 * in six bands, each its own rows of the knob, not all of them) */
    j = cv_y0 - cv_oy - y;
    if (j < 0) j = 0;
    if (j1 > d) j1 = d;
    for (; j < j1; j++)
        for (i = 0; i < d; i++) {
            float dx = (float)i - m, dy = (float)j - m, r = fm_sqrtf(dx * dx + dy * dy) / k, t;
            uint32_t c;
            if (r > 14.f) continue;
            if (r > 9.5f) {                      /* the ring: radius 12, 3 wide */
                c = cover((1.5f - fm_fabsf(r - 12.f)) * k);
                if (!c) continue;
                t = u_atan2(dy, dx) * (1.f / 3.14159274f);   /* -1..1, 0 at the right, 0.5 at the bottom */
                t = t - 0.75f;
                if (t < 0.f) t += 2.f;
                t *= (1.f / 1.5f);               /* 0 at the bottom left, 1 at the bottom right */
                if (t > 1.f) continue;
                cv_blend(x + i, y + j, (t >= lo && t <= hi && hi > lo) ? arc : U_TRACK, c);
            } else {                             /* the cap: radius 8, its rim, the pointer */
                c = cover((8.f - r) * k);
                if (!c) continue;
                cv_blend(x + i, y + j, cover((7.f - r) * k) ? U_KNOB : U_KNOBRIM, c);
                {   /* the pointer: from 3.5 to 8 along the value's angle, 1.8 wide */
                    float along = (dx * ca + dy * sa) / k, across = fm_fabsf(dy * ca - dx * sa);
                    if (along > 3.f && along < 8.f)
                        cv_blend(x + i, y + j, ptr, cover(0.9f * k - across));
                }
            }
        }
}

/* ----------------------------------------------------- the patch's map --- */
/* (the bytes' meanings: the reconstruction's parameters.md, page B byte = 128 + n) */
enum { V_NUM, V_BI, V_SEMI, V_ENUM, V_BPM, V_OFFON,
       V_WPW,                            /* osc wave / PW in one knob: byte the wave, byte - 1 the PW, byte - 2 the shape */
       V_O3ON,                           /* osc 3 on / off: its mode 0, or back to the last mode */
       V_NOTE, V_UNI, V_PLUS1,           /* a note name; unison (0 off, n: n + 1 voices); the value + 1 */
       V_FXT,                            /* 114 by the mode (112): delay time, or the reverb's pre-delay (in modes 3 and
                                          * 4 the feedback's repeat time too; "--" when the effect is off, a delay clock
                                          * sets the time, or a sync pattern: p_idle) */
       V_ROOM,                           /* 117 in the reverb: 0, 1, 2, 3+ the four rooms (effects.md; the engine) */
       V_SHAPE,                          /* an oscillator's shape (17, 22) in words: its wave (byte + 2) at 0, the morph
                                          * to the saw (64), the square at 127 (value_text) */
       V_DRY,                            /* B 58, the dry path's level (0.84; the engine's HYB_OUT2, 0 = 0 dB, 127
                                          * silent, the delay's / reverb's wet path kept): 0-127, the knob the number,
                                          * the strip its dB (strip_text) */
       V_SXAMT, V_SXEN, V_SXMS, V_SXKHZ, V_SXS, V_SXGL };   /* (0.89, HYB_SHIMMER) the patch's extension, FX >
                                          * SHIMMER: byte the value's index past the patch (shadow[256 + byte], p_byte):
                                          * AMOUNT (OFF at 0; 1.18C47 WIDTH too), a list (en: LAYERS; 0.91 TAIL), GRAIN
                                          * (20-200 ms), TONE (1.5-10 kHz), SWELL (0.91, OFF, 0.01-3.00 s), W GLIDE
                                          * (1.18C47, 0.50-4.00 s); the strip with the unit. Kinds V_SXAMT.. only */
#ifdef HYB_SHIMMER
#define P_EXT(p) ((p)->kind >= V_SXAMT)
#define P_BYTE0(p) (P_EXT(p) ? 256u + (p)->byte : (uint32_t)(p)->byte)   /* (its byte in shadow, the wave / PW's
                                          * own; app.c param_of, col_ok) */
#else
#define P_BYTE0(p) ((uint32_t)(p)->byte)
#endif
typedef struct u_param_s { uint8_t byte; uint8_t kind; uint8_t en; uint8_t max; char label[9]; char name[14]; } u_param_t;   /* (label:
                                          * 8 letters at most, "CHO RATE"; max 0: the kind's own, 127 or the list's end;
                                          * name: p_name) */
typedef struct { char tag[10]; u_param_t p[4]; } u_row_t;   /* four knobs (an empty label: none) */
typedef struct { char name[13]; uint8_t nr; } u_group_t;                   /* an EDIT group: nr rows */
typedef struct { char name[8]; uint8_t btn, nb, ng; } u_menu_t;            /* a menu: nb blocks, ng groups */

/* the value lists (names of 8 letters at most: they show in a knob's column) */
enum { E_ROUTE, E_LFOSH, E_DLYMODE, E_ARP, E_FILT, E_SAT, E_CLOCK, E_MXSRC, E_MXDST, E_DIST, E_O3, E_FMMODE, E_POL,
       E_POLY, E_KEYMODE, E_INMODE, E_SMOOTH, E_STAGES, E_FXSH, E_L3DST,
#ifdef HYB_SHIMMER
       E_SHLAY, E_SHTAIL,
#endif
       E_N };
static const char *const EN_ROUTE[] = {"SER 4", "SER 6", "PARALLEL", "SPLIT", 0};
static const char *const EN_LFOSH[] = {"SINE", "TRI", "SAW", "SQUARE", "S&H", "S&G", 0};   /* (6+: the waves) */
static const char *const EN_DLYMODE[] = {"OFF", "DELAY", "REVERB", "REV+FB 1", "REV+FB 2", "2:1", "4:3", "4:1", "8:7",
    "SYNC 1+1", "SYNC 2+1", "SYNC 3+1", "SYNC 4+1", "SYNC 5+1", "SYNC 2+3", "SYNC 2+5", "SYNC 3+2", "SYNC 3+3",
    "SYNC 3+4", "SYNC 3+5", "SYNC 4+3", "SYNC 4+5", "SYNC 5+2", "SYNC 5+3", "SYNC 5+4", "SYNC 5+5", 0};   /* (26+: as 1) */
static const char *const EN_ARP[] = {"OFF", "UP", "DOWN", "UP/DOWN", "PLAYED", "RANDOM", "CHORD", 0};
static const char *const EN_FILT[] = {"LP", "HP", "BP", "BS", "MUTE", "ANALOG 2", "BYPASS", "ANALOG 4", 0};
static const char *const EN_SAT[] = {"OFF", "LIGHT", "SOFT", "MIDDLE", "HARD", "DIGITAL", "SHAPER", "RECTIFY",
    "BIT RED", "RATE RED", "RATE+FLW", "LOWPASS", "LP+FLW", "HIGHPASS", "HP+FLW", 0};
static const char *const EN_CLOCK[] = {"OFF", "1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "3/64", "3/32", "3/16",
    "3/8", "1/16 T", "1/8 T", "1/4 T", "1/2 T", "2/3 BAR", "3/4 BAR", "1 BAR", "2 BARS", "4 BARS", 0};
static const char *const EN_MXSRC[] = {"OFF", "BEND", "PRESSURE", "CC 1", "CC 2", "CC 3", "CC 4", "CC 6", "CC 8",
    "CC 9", "CC 11", "CC 12", "CC 13", "CC 14", "CC 15", "CC 16", "CC 64", "CC 65", "CC 66", "AMP ENV", "FILT ENV",
    "LFO 1", "LFO 2", "LFO 3", "VELOCITY", "VELO 2", "KEYFLW", "CONST", 0};   /* (25: as 24; 27: a constant, measured) */
static const char *const EN_MXDST[] = {"OFF", "VOLUME", "VOLUME 2", "PAN", "TRANSP", "PORTA", "O1 SHAPE", "O1 PW",
    "O1 WAVE", "O1 PITCH", "O1 KEYF", "O2 SHAPE", "O2 PW", "O2 WAVE", "O2 PITCH", "O2 DETUN", "FM AMT", "FE>PITCH",
    "FE>FM", "O2 KEYF", "OSC BAL", "SUB VOL", "OSC VOL", "NOISE", "CUTOFF 1", "CUTOFF 2", "RESO 1", "RESO 2", "FENV 1",
    "FENV 2", "FKEYF 1", "FKEYF 2", "F BAL", "FE ATT", "FE DEC", "FE SUS", "FE SLOPE", "FE REL", "AE ATT", "AE DEC",
    "AE SUS", "AE SLOPE", "AE REL", "L1 RATE", "L1 SYMM", "L1>OSC 1", "L1>OSC 2", "L1>PW", "L1>RESO", "L1>GAIN",
    "L2 RATE", "L2 SYMM", "L2>SHAPE", "L2>FM", "L2>CUT 1", "L2>CUT 2", "L2>PAN", "L3 RATE", "L3 AMT", "--",
    "--", "--", "CHO MIX", "CHO RATE", "CHO DPTH", "CHO DLY", "CHO FB", "FX SEND", "DLY TIME", "DLY FB",
    "DLY RATE", "DLY DPTH", 0};          /* (matrix.md; 22: the osc volume in this engine; "--": none in the reference,
                                          * selectable, nothing moves. 72-127 (1.00): one name each, the short one,
                                          * MX_SHORT; the list is MXD_N long, en_count) */
static const char *const EN_DIST[] = {"OFF", "LIGHT", "SOFT", "MIDDLE", "HARD", "DIGITAL", "SHAPER", "RECTIFY",
    "BIT RED", "RATE RED", "LOWPASS", "HIGHPASS", 0};
static const char *const EN_O3[] = {"OFF", "SLAVE", "SAW", "SQUARE", "SINE", "TRI", 0};   /* (1: per the manual osc
                                          * 2's slave, silent in the measurement; 6+: the waves) */
static const char *const EN_FMMODE[] = {"FM 1", "FM 2", "FM 3", "NOISE", "INPUT", 0};
static const char *const EN_POL[] = {"NEG", "POS", 0};
static const char *const EN_POLY[] = {"POLY", "MONO", 0};
static const char *const EN_KEYMODE[] = {"POLY", "MONO 1", "MONO 2", "MONO 3", "MONO 4", "HOLD", 0};
static const char *const EN_INMODE[] = {"OFF", "DYNAMIC", "STATIC", "TO FX", 0};
static const char *const EN_SMOOTH[] = {"STEP", "GLIDE", "STEP 2", "GLIDE 2", 0};
static const char *const EN_STAGES[] = {"1", "2", "3", "4", "5", "6", 0};
static const char *const EN_FXSH[] = {"SINE", "TRI", "SAW", "SQUARE", "S&H", "S&G", 0};   /* (the chorus's; 6, 7 not
                                          * analysed) */
static const char *const EN_L3DST[] = {"OSC 1", "OSC 1 B", "OSC 2", "DEST 3", "DEST 4", "DEST 5", "DEST 6", 0};   /* (0
                                          * and 1 measured on osc 1; ASSUMPTION 2 osc 2, 3-6 not known: the engine's
                                          *) */
#ifdef HYB_SHIMMER
static const char *const EN_SHLAY[] = {"OCT", "OCT+5TH", "2 OCT", "OCT+SUB", "5TH", 0};   /* (the engine's layer sets:
                                          * rv_shim.inc RVS_R, +12; +12 and +19; +12 and +24; +12 and -12; +7) */
static const char *const EN_SHTAIL[] = {"LIMITED", "FREE", 0};   /* (0.91: the shimmer's tail coupled to the
                                          * reverb's, or free as 0.89 / 0.90; the extension's 0, 1) */
#endif
static const char *const *const U_EN[] = {EN_ROUTE, EN_LFOSH, EN_DLYMODE, EN_ARP, EN_FILT, EN_SAT, EN_CLOCK, EN_MXSRC,
    EN_MXDST, EN_DIST, EN_O3, EN_FMMODE, EN_POL, EN_POLY, EN_KEYMODE, EN_INMODE, EN_SMOOTH, EN_STAGES, EN_FXSH, EN_L3DST,
#ifdef HYB_SHIMMER
    EN_SHLAY, EN_SHTAIL,
#endif
};
_Static_assert(sizeof U_EN / sizeof U_EN[0] == E_N, "a list an E_*");

#define PB_(n) (128 + (n))
#define N_(b, l, n) {b, V_NUM, 0, 0, l, n}
#define BI(b, l, n) {b, V_BI, 0, 0, l, n}
#define EN(b, e, m, l, n) {b, V_ENUM, e, m, l, n}
#define OO(b, l, n) {b, V_OFFON, 0, 1, l, n}
#define DRY_P {PB_(58), V_DRY, 0, 0, "DRY", "Dry Level"}   /* (until 0.83 an OFF / ON switch "OUT 2": a turn set 0 or 1) */
#define MX(i, s, d, a) {"SLOT " #i, {EN(PB_(s), E_MXSRC, 0, "SOURCE", "Mx" #i " Source"), \
                         EN(PB_(d), E_MXDST, 0, "DEST", "Mx" #i " Dest"), BI(PB_(a), "AMOUNT", "Mx" #i " Amount")}}
/* the rows: the menus' blocks first (U_NBLK, in the menus' order: the quick-access list holds their indices), then
 * the EDIT groups' rows in the groups' order (U_GRP) */
static const u_row_t U_ROW[] = {
    /* FX (delay and reverb one effect: the mode 112 picks; app.c shows DLY/REV, then the blocks of its mode: menu_blk.
     * The indices stay those of the earlier blocks DELAY, REVERB, DLY/REV 1 and 2 (the quick-access list holds them):
     * the new blocks take the places of the old ones nearest to them) */
    {"DELAY", {N_(114, "TIME", "Delay Time"), N_(115, "FEEDBK", "Delay Feedbk"),
               EN(PB_(20), E_CLOCK, 0, "CLOCK", "Delay Clock"), BI(119, "COLOUR", "FX Colour")}},
    {"REVERB", {N_(116, "DECAY", "Reverb Decay"), {114, V_FXT, 0, 0, "PREDLY", "Pre-Delay"},
                {117, V_ROOM, 0, 3, "ROOM", "Reverb Room"}, N_(115, "FEEDBK", "Delay Feedbk")}},   /* (FEEDBK: modes 3
                                          * and 4 repeat the input at the pre-delay's time, measured; mode 2 none) */
    {"DLY/REV", {EN(112, E_DLYMODE, 0, "MODE", "FX Mode"), N_(113, "SEND", "FX Send"),
                 {114, V_FXT, 0, 0, "TIME", "Delay Time"}, BI(119, "COLOUR", "FX Colour")}},
    {"DELAY MOD", {N_(116, "RATE", "Delay Rate"), N_(117, "DEPTH", "Delay Depth"),
                   EN(118, E_FXSH, 7, "SHAPE", "Delay Shape")}},
    {"PHASER", {N_(PB_(85), "MIX", "Phaser Mix"), N_(PB_(86), "RATE", "Phaser Rate"),
                   N_(PB_(87), "DEPTH", "Phaser Depth"), BI(PB_(89), "FEEDBK", "Phaser Feedbk")}},
    {"CHORUS", {N_(105, "MIX", "Chorus Mix"), N_(106, "RATE", "Chorus Rate"), N_(107, "DEPTH", "Chorus Depth"),
                   BI(109, "FEEDBK", "Chorus Feedbk")}},
    {"DIST", {EN(PB_(100), E_DIST, 0, "TYPE", "Dist Type"), N_(PB_(101), "INTENS", "Dist Intens"),
                 N_(PB_(97), "BOOST", "Boost"), N_(PB_(98), "TUNE", "Boost Tune")}},
    /* SEL page 1: the filters */
    {"FILTER 1", {N_(40, "CUTOFF", "Cutoff"), N_(42, "RESO", "Resonance"), N_(44, "ENV AMT", "Filt Env Amt"),
                     BI(46, "KEYFLW", "Filt Keyflw")}},
    {"FILTER 2", {BI(41, "CUTOFF", "Cutoff 2"), N_(43, "RESO", "Resonance 2"), N_(45, "ENV AMT", "Filt2 Env Amt"),
                     BI(47, "KEYFLW", "Filt2 Keyflw")}},
    {"FLT MODES", {EN(51, E_FILT, 0, "FILT 1", "Filt1 Mode"), EN(52, E_FILT, 3, "FILT 2", "Filt2 Mode"),
                      EN(53, E_ROUTE, 0, "ROUTING", "Routing"), BI(48, "BALANCE", "Filt Balance")}},
    /* SEL page 2: the oscillators */
    {"OSC 1", {{17, V_SHAPE, 0, 0, "SHAPE", "Osc1 Shape"}, {19, V_WPW, 0, 0, "WAVE", "Osc1"},
                  {20, V_SEMI, 0, 0, "SEMI", "Osc1 Semi"}, BI(21, "KEYFLW", "Osc1 Keyflw")}},
    {"OSC 2", {{22, V_SHAPE, 0, 0, "SHAPE", "Osc2 Shape"}, {24, V_WPW, 0, 0, "WAVE", "Osc2"},
                  {25, V_SEMI, 0, 0, "SEMI", "Osc2 Semi"}, N_(26, "DETUNE", "Osc2 Detune")}},
    {"OSC 3", {EN(PB_(41), E_O3, 68, "MODE", "Osc3 Mode"), N_(PB_(42), "VOLUME", "Osc3 Volume"),
                  {PB_(43), V_SEMI, 0, 0, "SEMI", "Osc3 Semi"}, N_(PB_(44), "DETUNE", "Osc3 Detune")}},
    {"FM/SYNC", {{PB_(41), V_O3ON, 0, 1, "OSC 3", "Osc3 On"}, OO(28, "SYNC", "Osc Sync"),
                    N_(27, "FM AMT", "FM Amount"), EN(PB_(34), E_FMMODE, 0, "FM MODE", "FM Mode")}},
    {"MIX", {N_(33, "BALANCE", "Osc Balance"), N_(34, "SUB", "Sub Volume"), N_(36, "OSC VOL", "Osc Volume"),
                N_(35, "SUB SHP", "Sub Shape")}},
    {"MIX 2", {EN(49, E_SAT, 14, "SATUR", "Satur Type"), N_(37, "NOISE", "Noise Volume"), N_(38, "RING", "Ring Mod"),
                  BI(39, "N COLOR", "Noise Colour")}},
    /* ENV */
    {"AMP ENV", {N_(59, "ATTACK", "Amp Attack"), N_(60, "DECAY", "Amp Decay"), N_(61, "SUSTAIN", "Amp Sustain"),
                    N_(63, "RELEASE", "Amp Release")}},
    {"FILT ENV", {N_(54, "ATTACK", "Filt Attack"), N_(55, "DECAY", "Filt Decay"), N_(56, "SUSTAIN", "Filt Sustain"),
                     N_(58, "RELEASE", "Filt Release")}},
    /* LFO (amount and destination: the LFOs' assignable slots, the matrix law) */
    {"LFO 1", {N_(67, "RATE", "LFO1 Rate"), EN(68, E_LFOSH, 67, "SHAPE", "LFO1 Shape"),
                  BI(PB_(80), "AMOUNT", "LFO1 Amount"), EN(PB_(79), E_MXDST, 0, "DEST", "LFO1 Dest")}},
    {"LFO 2", {N_(79, "RATE", "LFO2 Rate"), EN(80, E_LFOSH, 67, "SHAPE", "LFO2 Shape"),
                  BI(PB_(82), "AMOUNT", "LFO2 Amount"), EN(PB_(81), E_MXDST, 0, "DEST", "LFO2 Dest")}},
    {"LFO 3", {N_(PB_(7), "RATE", "LFO3 Rate"), EN(PB_(8), E_LFOSH, 67, "SHAPE", "LFO3 Shape"),
                  N_(PB_(12), "AMOUNT", "LFO3 Amount"), EN(PB_(11), E_L3DST, 0, "DEST", "LFO3 Dest")}},
    /* ARP */
    {"ARP", {EN(PB_(1), E_ARP, 0, "MODE", "Arp Mode"), N_(PB_(2), "PATTERN", "Arp Pattern"),
                {PB_(3), V_PLUS1, 0, 3, "OCTAVES", "Arp Octaves"}, EN(PB_(17), E_CLOCK, 0, "CLOCK", "Arp Clock")}},
    {"GROOVE", {N_(PB_(5), "LENGTH", "Note Length"), N_(PB_(6), "SWING", "Swing"), OO(PB_(4), "HOLD", "Arp Hold"),
                   {PB_(16), V_BPM, 0, 0, "TEMPO", "Tempo"}}},
    /* FX's last block (0.84), after the menus' others: the quick-access list keeps the indices before it (FX_BLK) */
    {"LEVEL", {DRY_P, N_(113, "SEND", "FX Send"), N_(91, "VOLUME", "Patch Volume"), BI(10, "PAN", "Pan")}},
#ifdef HYB_SHIMMER
    /* FX's SHIMMER (0.89, the reverb modes, after REVERB): the patch's extension */
    /* (1.18C45) K3 MODULATION (the extension's value 4, reserved before) in GRAIN's place; GRAIN
     * on EDIT > SHIMMER */
    {"SHIMMER", {{0, V_SXAMT, 0, 127, "AMOUNT", "Shim Amount"}, {1, V_SXEN, E_SHLAY, 0, "LAYERS", "Shim Layers"},
                 {4, V_SXAMT, 0, 127, "MOD DPTH", "Shim Mod"}, {3, V_SXKHZ, 0, 127, "TONE", "Shim Tone"}}},
#define U_NBLK 25u
#define ROW_SHIM 24u
#else
#define U_NBLK 24u
#endif
#define ROW_GROOVE 22u
#define ROW_LEVEL 23u
    /* FX's EDIT groups */
    {"DLY/REV", {EN(112, E_DLYMODE, 0, "MODE", "FX Mode"), EN(PB_(20), E_CLOCK, 0, "CLOCK", "Delay Clock"),
                    N_(113, "SEND", "FX Send"), N_(114, "TIME", "Delay Time")}},
    {"DLY/REV", {N_(115, "FEEDBK", "Delay Feedbk"), N_(116, "RATE", "Delay Rate"), N_(117, "DEPTH", "Delay Depth"),
                    EN(118, E_FXSH, 7, "SHAPE", "Delay Shape")}},
    {"DLY/REV", {BI(119, "COLOUR", "FX Colour")}},
    {"PHASER", {EN(PB_(84), E_STAGES, 0, "STAGES", "Phaser Stages"), N_(PB_(88), "FREQ", "Phaser Freq"),
                   N_(PB_(90), "SPREAD", "Phaser Spread"), N_(PB_(85), "MIX", "Phaser Mix")}},
    {"PHASER", {N_(PB_(86), "RATE", "Phaser Rate"), N_(PB_(87), "DEPTH", "Phaser Depth"),
                   BI(PB_(89), "FEEDBK", "Phaser Feedbk")}},
    {"CHORUS", {N_(105, "MIX", "Chorus Mix"), N_(106, "RATE", "Chorus Rate"), N_(107, "DEPTH", "Chorus Depth"),
                   N_(108, "DELAY", "Chorus Delay")}},
    {"CHORUS", {BI(109, "FEEDBK", "Chorus Feedbk"), EN(110, E_FXSH, 7, "SHAPE", "Chorus Shape")}},
    {"DIST/BST", {EN(PB_(100), E_DIST, 0, "TYPE", "Dist Type"), N_(PB_(101), "INTENS", "Dist Intens"),
                     N_(PB_(97), "BOOST", "Boost"), N_(PB_(98), "TUNE", "Boost Tune")}},
    {"EQ", {BI(PB_(95), "LO GAIN", "EQ Low Gain"), N_(PB_(45), "LO FREQ", "EQ Low Freq"),
               BI(PB_(92), "MID GAIN", "EQ Mid Gain"), N_(PB_(93), "MID FREQ", "EQ Mid Freq")}},
    {"EQ", {N_(PB_(94), "MID Q", "EQ Mid Q"), BI(PB_(96), "HI GAIN", "EQ High Gain"),
               N_(PB_(46), "HI FREQ", "EQ High Freq")}},
    {"INPUT", {EN(101, E_INMODE, 0, "MODE", "Input Mode"), N_(102, "SELECT", "Input Select"),
                  N_(PB_(99), "RING", "Input Ring")}},
    {"OUTPUT", {DRY_P, N_(91, "VOLUME", "Patch Volume"), BI(10, "PAN", "Pan")}},
#ifdef HYB_SHIMMER
    /* EDIT > SHIMMER (0.91): the extension's TAIL and SWELL (app.c SHX_TAIL, SHX_SWELL; FX > SHIMMER keeps K1-K4;
     * FX's last group: the others keep their places); (1.18C45) K3 GRAIN, off FX > SHIMMER; (1.18C47) K4 WIDTH (the
     * extension's value 5), its second page K1 W GLIDE (app.c SHX_GL: past the values) */
    {"SHIMMER", {{6, V_SXEN, E_SHTAIL, 0, "TAIL", "Shim Tail"}, {7, V_SXS, 0, 127, "SWELL", "Shim Swell"},
                 {2, V_SXMS, 0, 127, "GRAIN", "Shim Grain"}, {5, V_SXAMT, 0, 127, "WIDTH", "Shim Width"}}},
    {"SHIMMER", {{8, V_SXGL, 0, 15, "W GLIDE", "Shim W Glide"}}},
#define U_NSHG 2u                        /* (its rows) */
#else
#define U_NSHG 0u
#endif
    /* SEL page 1's */
    {"SATURAT", {EN(49, E_SAT, 14, "TYPE", "Satur Type")}},
    {"ENV POL", {EN(PB_(30), E_POL, 0, "FILT 1", "Env Pol 1"), EN(PB_(31), E_POL, 0, "FILT 2", "Env Pol 2")}},
    {"CUT LINK", {OO(PB_(32), "LINK", "Cutoff Link")}},
    {"KEY BASE", {{PB_(33), V_NOTE, 0, 0, "BASE", "Key Base"}}},
    {"VELOCITY", {BI(PB_(54), "ENV 1", "Vel>FEnv 1"), BI(PB_(55), "ENV 2", "Vel>FEnv 2"),
                     BI(PB_(56), "RESO 1", "Vel>Reso 1"), BI(PB_(57), "RESO 2", "Vel>Reso 2")}},
    /* SEL page 2's */
    {"OSC 2 +", {BI(31, "KEYFLW", "Osc2 Keyflw"), BI(29, "FE>PITCH", "FEnv>O2 Pitch"), BI(30, "FE>FM", "FEnv>FM")}},
    {"PH/PUNCH", {N_(PB_(35), "PHASE", "Osc Phase"), N_(PB_(36), "PUNCH", "Punch")}},
    {"VELOCITY", {BI(PB_(47), "SHAPE 1", "Vel>Shape 1"), BI(PB_(48), "SHAPE 2", "Vel>Shape 2"),
                     BI(PB_(49), "PW", "Vel>PW"), BI(PB_(50), "FM", "Vel>FM")}},
    {"VELOCITY", {BI(PB_(60), "AMP", "Vel>Amp"), BI(PB_(61), "PAN", "Vel>Pan")}},
    {"UNISON", {{97, V_UNI, 0, 15, "VOICES", "Unison"}, N_(98, "DETUNE", "Uni Detune"),
                   N_(99, "SPREAD", "Uni Spread"), BI(100, "LFO PH", "Uni LFO Phase")}},   /* (15: ASSUMPTION, the
                                          * voices there are) */
    {"VOICE", {EN(94, E_KEYMODE, 0, "KEYMODE", "Key Mode"), N_(5, "PORTA", "Portamento"),
                  BI(93, "TRANSP", "Transpose"), EN(PB_(25), E_SMOOTH, 0, "SMOOTH", "Smooth Mode")}},
    {"VOICE", {{PB_(26), V_SEMI, 0, 0, "BEND UP", "Bend Up"}, {PB_(27), V_SEMI, 0, 0, "BEND DN", "Bend Down"}}},
    /* ENV's */
    {"SLOPES", {BI(57, "F SLOPE", "FEnv Slope"), BI(62, "A SLOPE", "AEnv Slope")}},
    /* LFO's (the fixed routes) */
    {"LFO 1", {OO(69, "ENV", "LFO1 Env Mode"), EN(70, E_POLY, 0, "MODE", "LFO1 Mode"), BI(71, "SYMM", "LFO1 Symm"),
                  N_(72, "KEYFLW", "LFO1 Keyflw")}},
    {"LFO 1", {OO(73, "TRIGGER", "LFO1 Trigger"), EN(PB_(18), E_CLOCK, 0, "CLOCK", "LFO1 Clock"),
                  BI(74, ">OSC 1", "LFO1>Osc1"), BI(75, ">OSC 2", "LFO1>Osc2")}},
    {"LFO 1", {BI(76, ">PW", "LFO1>PW"), BI(77, ">RESO", "LFO1>Reso"), BI(78, ">GAIN", "LFO1>Filt Gain")}},
    {"LFO 2", {OO(81, "ENV", "LFO2 Env Mode"), EN(82, E_POLY, 0, "MODE", "LFO2 Mode"), BI(83, "SYMM", "LFO2 Symm"),
                  N_(84, "KEYFLW", "LFO2 Keyflw")}},
    {"LFO 2", {OO(85, "TRIGGER", "LFO2 Trigger"), EN(PB_(19), E_CLOCK, 0, "CLOCK", "LFO2 Clock"),
                  BI(86, ">SHAPE", "LFO2>Shape"), BI(87, ">FM", "LFO2>FM")}},
    {"LFO 2", {BI(88, ">CUT 1", "LFO2>Cutoff 1"), BI(89, ">CUT 2", "LFO2>Cutoff 2"), BI(90, ">PAN", "LFO2>Pan")}},
    {"LFO 3", {EN(PB_(9), E_POLY, 0, "MODE", "LFO3 Mode"), N_(PB_(10), "KEYFLW", "LFO3 Keyflw"),
                  N_(PB_(13), "FADE IN", "LFO3 Fade-In"), EN(PB_(21), E_CLOCK, 0, "CLOCK", "LFO3 Clock")}},
    /* EDIT: the matrix, the reference's six slots: 1 one destination, 2 two (2A, 2B on one source), 3 three (3A-3C on
     * one source), 4-6 one each (B 103-111, 0.86) */
    MX(1, 64, 65, 66), MX(2A, 67, 68, 69), MX(2B, 67, 70, 71), MX(3A, 72, 73, 74), MX(3B, 72, 75, 76), MX(3C, 72, 77, 78),
    MX(4, 103, 104, 105), MX(5, 106, 107, 108), MX(6, 109, 110, 111),
};
#define U_NROW (sizeof U_ROW / sizeof U_ROW[0])
#define MX_N 9u                          /* the matrix's rows, U_ROW's last (slots 1, 2A, 2B, 3A-3C, 4, 5, 6) */
_Static_assert(U_NROW == 41u + U_NSHG + U_NBLK, "the blocks and the groups' rows (U_GRP)");
_Static_assert(U_NBLK == QA_NSEC, "the quick-access list's blocks (app.c QA_NSEC)");
static const u_group_t U_GRP[] = {
    {"DELAY/REVERB", 3}, {"PHASER", 2}, {"CHORUS", 2}, {"DIST/BOOST", 1}, {"EQ", 2}, {"INPUT", 1}, {"OUTPUT", 1},
#ifdef HYB_SHIMMER
    {"SHIMMER", 2},                      /* (0.91: TAIL, SWELL; 1.18C45: GRAIN; 1.18C47: WIDTH, W GLIDE) */
#endif
    {"SATURATION", 1}, {"ENV POLARITY", 1}, {"CUTOFF LINK", 1}, {"KEY BASE", 1}, {"VELOCITY", 1},
    {"OSC 2 MORE", 1}, {"PHASE/PUNCH", 1}, {"VELOCITY", 2}, {"UNISON", 1}, {"VOICE", 2},
    {"SLOPES", 1},
    {"LFO 1", 3}, {"LFO 2", 3}, {"LFO 3", 1},
    {"SLOT 1", 1}, {"SLOT 2A", 1}, {"SLOT 2B", 1}, {"SLOT 3A", 1}, {"SLOT 3B", 1}, {"SLOT 3C", 1},
    {"SLOT 4", 1}, {"SLOT 5", 1}, {"SLOT 6", 1},
};
#define U_NGRP (sizeof U_GRP / sizeof U_GRP[0])
enum { M_FX, M_SEL1, M_SEL2, M_ENV, M_LFO, M_MTX, M_ARP, U_NMENU };
_Static_assert(U_NMENU == sizeof ui.mpos, "a place a menu (app.c ui.mpos)");
static const u_menu_t U_MENU[U_NMENU] = {
    {"FX", B_FX, 7, 7 + (U_NSHG != 0u)},   /* (FX's nb: its blocks in U_ROW's run, menu_b0; LEVEL past ARP's, FX_BLK) */ {"FILTERS", B_SEL, 3, 5}, {"OSCS", B_SEL, 6, 5}, {"ENV", B_ENV, 2, 1}, {"LFO", B_LFO, 3, 3},
    {"MATRIX", B_EDIT, 0, MX_N}, {"ARP", B_ARP, 2, 0},
};
static uint32_t menu_b0(uint32_t m) { uint32_t i, n = 0; for (i = 0; i < m; i++) n += U_MENU[i].nb; return n; }
static uint32_t menu_g0(uint32_t m) { uint32_t i, n = 0; for (i = 0; i < m; i++) n += U_MENU[i].ng; return n; }
static uint32_t group_r0(uint32_t g) { uint32_t i, n = U_NBLK; for (i = 0; i < g; i++) n += U_GRP[i].nr; return n; }
/* the delay / reverb mode (112): 0 off, 1 a delay (1, 5-25, 26+ as 1), 2 a reverb (2-4) */
static uint32_t fx_class(const uint8_t *patch)
{
#ifdef HYB_MULTI
    uint32_t m = (ed_part ? shadow_o : patch)[112];   /* (after 0.98: part B edited, part A's: B sends into A's) */
#else
    uint32_t m = patch[112];
#endif
    return !m ? 0u : m >= 2u && m <= 4u ? 2u : 1u;
}
/* FX's blocks by the mode: DLY/REV first, then the mode's own (none when off), then PHASER, CHORUS, DIST, LEVEL
 * (U_ROW) */
#ifdef HYB_SHIMMER
static const uint8_t FX_BLK[3][7] = {{2, 4, 5, 6, ROW_LEVEL}, {2, 0, 3, 4, 5, 6, ROW_LEVEL},
                                     {2, 1, ROW_SHIM, 4, 5, 6, ROW_LEVEL}};   /* (SHIMMER after REVERB, 0.89) */
static const uint8_t FX_NB[3] = {5, 7, 7};
#else
static const uint8_t FX_BLK[3][7] = {{2, 4, 5, 6, ROW_LEVEL}, {2, 0, 3, 4, 5, 6, ROW_LEVEL}, {2, 1, 4, 5, 6, ROW_LEVEL}};
static const uint8_t FX_NB[3] = {5, 7, 6};
#endif
static uint32_t menu_nb(uint32_t m, const uint8_t *patch)   /* the menu's blocks now */
{
    return m == M_FX ? FX_NB[fx_class(patch)] : U_MENU[m].nb;
}
/* SEL page 2's blocks in the player's order, the mixers before FM/SYNC (U_ROW keeps its order: the quick-access
 * list holds the indices) */
static const uint8_t SEL2_BLK[6] = {10, 11, 12, 14, 15, 13};
static uint32_t menu_blk(uint32_t m, uint32_t pos, const uint8_t *patch)   /* its block pos (< menu_nb): a U_ROW */
{
    return m == M_FX ? FX_BLK[fx_class(patch)][pos] : m == M_SEL2 ? SEL2_BLK[pos] : menu_b0(m) + pos;
}
static int p_idle(const u_param_t *p)    /* a knob that does nothing in the mode now (drawn dim, its value "--") */
{
    /* MEASURED, the delay / reverb's knobs by the mode (A 112): 114 (any of its knobs) idle
     * with the effect off, a delay clock on (it sets the delay's time and the reverb's pre-delay) or a sync pattern
     * 9-25 (their own times); the clock (B 20) with the effect off or a sync pattern; the feedback (115) off or in
     * mode 2 (one pre-delayed reverb); the rate (116) and the depth (117) off; the shape (118) off or in a reverb
     * mode. (The bytes 114-118 and B 20 are the delay / reverb's only: no other knob has them) */
    const uint32_t b = p->byte, m = shadow[112];
    if (b == 114u) return !m || shadow[PB_(20)] || m - 9u < 17u;
    if (b == PB_(20)) return !m || m - 9u < 17u;
    if (b - 115u < 4u) return !m || (b == 115u && m == 2u) || (b == 118u && m - 2u < 3u);
    return 0;
}

#ifdef HYB_MULTI
/* (after 0.98) part B edited: a knob of part A's delay / reverb (its mode, time, feedback, colour, modulation, room,
 * clock; the shimmer): B has none of its own (it sends into A's: SEND stays B's), its bytes do nothing. No knob: the
 * row says "CHANGE IN PART A" / "REC: GO TO PART A" over two or more such columns side by side (one alone: dim, "--");
 * REC goes to part A's same page (app.c pa_rec), a turn says so (pa_turn) */
static int p_parta(const u_param_t *p)
{
    const uint32_t b = P_BYTE0(p);
    return ed_part && p->label[0] && (b >= 256u || b == 112u || (b >= 114u && b <= 119u) || b == PB_(20));
}
static UI_LATE void u_text_fit(int32_t cx, int32_t y, int32_t w, const char *s, uint16_t c)   /* centred on cx in w
                                          * pixels: FONT_M, else smaller */
{
    const felucca_font_t *f = &FONT_M;
    if (text_w(f, s) > w) f = &FONT_S;
    if (text_w(f, s) > w) f = &FONT_XS;
    cv_text(cx - text_w(f, s) / 2, y, f, s, c);
}
#else
#define p_parta(p) 0
#endif
#ifdef HYB_SHIM_LEND
/* (1.04, the Bluetooth two-part build) the shimmer's knobs (FX > SHIMMER, EDIT > SHIMMER: the extension's) while
 * Bluetooth's memory is not lent (Bluetooth on at power-on): no shimmer then. As part B's of part A's (p_parta): no
 * knob, the row says "SHIMMER: BT OFF" over them ("--" for one alone), a turn says so (app.c param_turn); the values
 * kept as they are (loaded, saved with the patch) */
#define P_BTOFF(p) (!bt_lent && (p)->label[0] && P_EXT(p))
#else
#define P_BTOFF(p) 0
#endif

static const char *const MX_SHARED[MX_N] = {0, "SOURCE SHARED: 2B", "SOURCE SHARED: 2A", "SOURCE SHARED: 3B, 3C",
                                            "SOURCE SHARED: 3A, 3C", "SOURCE SHARED: 3A, 3B", 0, 0, 0};

static uint32_t text_len(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }

#define MXD_L 72u                        /* the destinations with a name of their own in EN_MXDST (0 .. 71) */
#define MXD_N 128u                       /* the destinations (1.00: all of the reference's;
                                          * 72 .. 127 named by mx_short) */
static uint32_t en_count(uint32_t e)
{
    uint32_t n = 0;
    if (e == E_MXDST) return MXD_N;
    while (U_EN[e][n]) n++;
    return n;
}
static UI_LATE void mx_short(uint32_t d, char *s);
static uint32_t p_byte(const u_param_t *p)   /* the byte the knob changes now (wave / PW: by the shape's side; the
                                          * extension's: past the patch) */
{
#ifdef HYB_SHIMMER
    if (P_EXT(p)) return 256u + p->byte;
#endif
    return p->kind == V_WPW && shadow[p->byte - 2u] >= 64u ? p->byte - 1u : p->byte;
}
#ifdef HYB_SHIMMER
/* a destination the shimmer can be a target of (the matrix's, LFO 1's and 2's assigns, LFO 3's: app.c SHX_RB): its
 * list's entries, then SHIM AMT and SHIM MOD (0.89, SHX_NT; SHIM MOD 1.18C45 in SHIM TONE's place, code 2: the
 * MODULATION depth offset; LFO 3 on it moves the shimmer's pitch itself); the panel's byte
 * holds SHX_VD + t - 1 for those */
static const char *const SHX_TN[SHX_NT] = {"SHIM AMT", "SHIM MOD"};
#define P_RT(p) ((p)->kind == V_ENUM && ((p)->en == E_MXDST || (p)->en == E_L3DST))
static UI_LATE uint32_t rt_idx(const u_param_t *p, uint32_t v)   /* the byte v's place in the list */
{
    const uint32_t n = en_count(p->en);
    return v >= SHX_VD ? n + v - SHX_VD : v > n - 1u ? n - 1u : v;
}
static UI_LATE uint32_t rt_byte(const u_param_t *p, uint32_t i)   /* the list's place i as the panel's byte */
{
    const uint32_t n = en_count(p->en);
    return i >= n ? SHX_VD + i - n : i;
}
#endif
static uint32_t p_max(const u_param_t *p)
{
    if (p->kind == V_WPW) return p_byte(p) == p->byte ? 64u : 127u;   /* (the waves 0-64) */
    if (p->max) return p->max;
#ifdef HYB_SHIMMER
    if (p->kind == V_SXEN) return en_count(p->en) - 1u;
    if (P_RT(p)) return en_count(p->en) - 1u + SHX_NT;   /* (SHIM AMT, SHIM MOD after the list) */
#endif
    return p->kind == V_ENUM ? en_count(p->en) - 1u : p->kind == V_OFFON || p->kind == V_O3ON ? 1u : 127u;
}
static uint32_t p_list(const u_param_t *p)   /* a list of named values (4 or more): its length (the overlay), else 0 */
{
    uint32_t n = p_max(p) + 1u;
#ifdef HYB_SHIMMER
    if (p->kind == V_SXEN) return n >= 4u ? n : 0u;
#endif
    return (p->kind == V_ENUM || (p->kind == V_WPW && p_byte(p) == p->byte)) && n >= 4u ? n : 0u;
}
static const char *p_label(const u_param_t *p)
{
    if (p->kind == V_FXT) return fx_class(shadow) == 2u ? "PREDLY" : "TIME";
    return p->kind == V_WPW ? (p_byte(p) == p->byte ? "WAVE" : "PW") : p->label;
}
static void p_name(const u_param_t *p, char *s)   /* its name after s (wave / PW: the one the knob changes now) */
{
    if (p->kind == V_FXT) { text_cat(s, fx_class(shadow) == 2u ? "Pre-Delay" : "Delay Time"); return; }
    text_cat(s, p->name);
    if (p->kind == V_WPW) { text_cat(s, " "); text_cat(s, p_byte(p) == p->byte ? "Wave" : "PW"); }
}
static void put_sint(char *s, int32_t v, int sign)   /* a number, with + or - when sign */
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    if (sign && v > 0) *s++ = '+';
    if (v < 0) *s++ = '-';
    do t[n++] = (char)('0' + u % 10u); while ((u /= 10u) && n < 11);
    while (n) *s++ = t[--n];
    *s = 0;
}
static void wave_text(uint32_t w, char *s)   /* a wave of the table: 0 sine, 1 triangle, 2-64 */
{
    s[0] = 0;
    if (w < 2u) { text_cat(s, w ? "TRI" : "SINE"); return; }
    text_cat(s, "WAVE ");
    put_sint(s + 5, (int32_t)w, 0);
}
#ifdef HYB_SHIMMER
/* the shimmer's GRAIN and TONE as the engine takes them (rv_shim.inc rvs_setup: 20 x 10^(v / 127) ms, 1.5 x (20 / 3)^(v /
 * 127) kHz; its own exp2 is within 0.3 %) */
static UI_LATE uint32_t shx_ms(uint32_t v) { return (uint32_t)(20.f * sy_db2lin((float)v * (20.f / 127.f)) + 0.5f); }
static UI_LATE uint32_t shx_dkhz(uint32_t v)   /* (tenths of a kHz) */
{
    return (uint32_t)(15.f * sy_db2lin((float)v * (20.f * 0.82390874f / 127.f)) + 0.5f);
}
#endif
static uint8_t u_vraw;                   /* value_text: the value itself (a value's list: no "SEQ") */
static UI_LATE void value_text(const u_param_t *p, uint32_t v, char *s)   /* (UI_LATE: the flash code before the engine short, app.c UI_LATE) */
{
    s[0] = 0;
    if (!u_vraw && p_idle(p)) { s[0] = s[1] = '-'; s[2] = 0; return; }   /* (a knob that does nothing in the delay /
                                          * reverb mode now; a list's values themselves) */
    switch (p->kind) {
    case V_BI: put_sint(s, (int32_t)v - 64, 1); break;
    case V_SEMI: put_sint(s, (int32_t)v - 64, 1); break;
    case V_BPM: put_sint(s, (int32_t)v + 63, 0); break;
    case V_OFFON: case V_O3ON: text_cat(s, v ? "ON" : "OFF"); break;
    case V_PLUS1: put_sint(s, (int32_t)v + 1, 0); break;
    case V_FXT: put_sint(s, (int32_t)v, 0); break;   /* (idle: "--", above) */
    case V_SHAPE: {                      /* (0 the wave chosen: "SINE", "WAVE 5"; to 64 its morph to the saw, "TRI>SAW
                                          * 32", "W5>SAW 32"; "SAW 64"; the saw to the square, "SAW>SQR 96"; "SQUARE") */
        const uint32_t w = shadow[p->byte + 2u];
        if (!v) { wave_text(w, s); break; }
        if (v == 127u) { text_cat(s, "SQUARE"); break; }
        if (v < 64u) {
            if (w < 2u) text_cat(s, w ? "TRI" : "SINE");
            else { text_cat(s, "W"); put_sint(s + 1, (int32_t)w, 0); }
            text_cat(s, ">SAW ");
        } else text_cat(s, v == 64u ? "SAW " : "SAW>SQR ");
        put_sint(s + text_len(s), (int32_t)v, 0);
        break;
    }
    case V_ROOM: put_sint(s, (int32_t)(v > 3u ? 3u : v) + 1, 0); break;   /* (rooms 1-4: no names, which is
                                          * how large not measured) */
#if defined(HYB_MULTI) || FM1B_BLE
    case V_UNI:                          /* (the patch's; capped by GLO > AUDIO's UNISON MAX: "5>2") */
        if (!v) { text_cat(s, "OFF"); break; }
        put_sint(s, (int32_t)v + 1, 0);
        {
            const uint32_t m = shadow[94] >= 1u && shadow[94] <= 4u, lim = m ? SET_UNIM : SET_UNI;
            if (lim && v + 1u > lim) { text_cat(s, ">"); put_sint(s + text_len(s), (int32_t)lim, 0); }
        }
        break;
#else
    case V_UNI: if (v) put_sint(s, (int32_t)v + 1, 0); else text_cat(s, "OFF"); break;
#endif
    case V_NOTE: {                       /* (C4 = 60) */
        static const char N[12][3] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        text_cat(s, N[v % 12u]);
        put_sint(s + (N[v % 12u][1] ? 2 : 1), (int32_t)(v / 12u) - 1, 0);
        break;
    }
    case V_WPW:
        if (p_byte(p) == p->byte) wave_text(v, s);
        else put_sint(s, (int32_t)v, 0);
        break;
    case V_ENUM:
#ifdef HYB_MULTI
        if (p->en == E_ARP && v && !u_vraw && sqr[ed_part].hold) { text_cat(s, "SEQ"); break; }   /* (the part's sequencer has a
                                          * pattern: its arpeggiator held off, the patch's mode back at NONE) */
#else
        if (p->en == E_ARP && v && !u_vraw && sq.hold) { text_cat(s, "SEQ"); break; }   /* (the sequencer has a pattern: the
                                          * arpeggiator held off, the patch's mode back at NONE) */
#endif
#ifdef HYB_SHIMMER
        if (P_RT(p) && (v >= SHX_VD || (u_vraw && v >= en_count(p->en)))) {   /* (a target: the panel's byte, or the
                                          * list's place) */
            const uint32_t t = v >= SHX_VD ? v - SHX_VD : v - en_count(p->en);
            text_cat(s, t < SHX_NT ? SHX_TN[t] : "SHIM");
            break;
        }
#endif
        if (p->en == E_MXDST && v >= MXD_L && v < MXD_N) mx_short(v, s);   /* (72 ..: the short name, 1.00) */
        else if (v < en_count(p->en)) text_cat(s, U_EN[p->en][v]);
        else if (p->en == E_LFOSH || p->en == E_O3) wave_text(v - 4u, s);   /* (6: the wave 2) */
        else put_sint(s, (int32_t)v, 0);
        break;
#ifdef HYB_SHIMMER
    case V_SXAMT: if (v) put_sint(s, (int32_t)v, 0); else text_cat(s, "OFF"); break;
    case V_SXEN: if (v < en_count(p->en)) text_cat(s, U_EN[p->en][v]); else put_sint(s, (int32_t)v, 0); break;
    case V_SXMS: put_sint(s, (int32_t)shx_ms(v), 0); break;   /* (the unit in the strip: strip_text) */
    case V_SXS: case V_SXGL: {           /* SWELL: 3 s x (v / 127)^2, two decimals (rv_shim.inc rvs_setup); 0 OFF;
                                          * (1.18C47) W GLIDE: 0.5 x 2^(v / 5) s (sy_cln_rv.c rvs_c1) */
        static const uint16_t G5[5] = {1000u, 1149u, 1320u, 1516u, 1741u};   /* (2^(i / 5) x 1000) */
        uint32_t c = (300u * v * v + 8064u) / 16129u;
        if (p->kind == V_SXGL) { if (v > 15u) v = 15u; c = ((50u << (v / 5u)) * G5[v % 5u] + 500u) / 1000u; }
        else if (!v) { text_cat(s, "OFF"); break; }
        put_sint(s, (int32_t)(c / 100u), 0);
        s += text_len(s);
        *s++ = '.'; *s++ = (char)('0' + c / 10u % 10u); *s++ = (char)('0' + c % 10u); *s = 0;
        break;
    }
    case V_SXKHZ: {                      /* (one decimal) */
        const uint32_t d = shx_dkhz(v);
        put_sint(s, (int32_t)(d / 10u), 0);
        s += text_len(s);
        *s++ = '.'; *s++ = (char)('0' + d % 10u); *s = 0;
        break;
    }
#endif
    default: put_sint(s, (int32_t)v, 0); break;
    }
}

static UI_LATE void strip_text(const u_param_t *p, uint32_t v, char *s)   /* a value in the strip: value_text's;
                                          * the dry level (V_DRY) in dB, one decimal ("0.0 dB", "-4.0 dB" at 64, OFF at
                                          * 127): the engine's HYB_OUT2 in two straight lines (0-64, 64-126; within
                                          * 0.1 dB of it). (Under its knob the number: value_text) */
{
    const float g = v <= 64u ? 1.f - (float)v * 0.00576562f : 0.631f - (float)(v - 64u) * 0.00985806f;
    uint32_t t;
    value_text(p, v, s);
#ifdef HYB_SHIMMER
    if (p->kind == V_SXMS) { text_cat(s, " ms"); return; }
    if (p->kind == V_SXKHZ) { text_cat(s, " kHz"); return; }
    if (p->kind == V_SXS) { if (v) text_cat(s, " s"); return; }
    if (p->kind == V_SXGL) { text_cat(s, " s"); return; }
#endif
    if (p->kind != V_DRY) return;
    if (v >= 127u) { s[0] = 0; text_cat(s, "OFF"); return; }
    t = v ? (uint32_t)(-sy_lin2db(g) * 10.f + 0.5f) : 0u;
    if (t) *s++ = '-';
    put_sint(s, (int32_t)(t / 10u), 0);
    s += text_len(s);
    *s++ = '.'; *s++ = (char)('0' + t % 10u); *s = 0;
    text_cat(s, " dB");
}

/* ------------------------------------------------------------- drawing --- */
/* the layout: the LCD strip, the two rows of four knobs, the status line (240 x 240; no tab line) */
#define UI_LCD_H 58                      /* the strip at y 4 */
#define UI_ROW0_Y 66
#define UI_ROW1_Y 153
#define UI_ROW_H 84                      /* (the status line gone: its rows for the values) */
#define UI_BAND 16                       /* the rows a canvas band holds (app.h APP_CANVAS_PX): the strip in four, a row in six */
#define UI_KNOB 34                       /* the knobs' size (the design's 30; less than 40: room for the value in the big font) */
static uint32_t ui_prof[5];                      /* the slowest draw: lcd, tabs, row, row, status (us) */
static uint32_t u_t0;
static void prof_begin(void) { u_t0 = plat_ticks24(); }
static void prof_end(uint32_t i)
{
    uint32_t us = (plat_ticks24() - u_t0) / PLAT_TICKS_PER_US;
    if (us > ui_prof[i]) ui_prof[i] = us;
}

/* the LCD strip: the patch, then the parameter last turned and its value (or the section's name) */
/* the LCD strip: one line in the full height, a (the patch, the module or a parameter's name) on the left and v (a
 * value, may be empty) on the right, each in the biggest font that fits beside the other and the battery */
static char lcd_la[24];                          /* the strip's name as drawn (draw_lcd; empty: all of it again) */
#define la lcd_la
static void draw_lcd(const char *a, const char *v, int batt)   /* the same name and fonts as last time: the part right
                                                     * of the name alone drawn again (a knob turned: its value) */
{
    static const felucca_font_t *lfa, *lfv;
    static int lbatt = -9;
    int32_t w = 232, h = UI_LCD_H, room = w - 14 - (batt >= 0 ? 26 : 0), wv, b0, ox = 0, i;
    const felucca_font_t *fa = &FONT_L, *fv = &FONT_L;
    prof_begin();
    wv = v[0] ? text_w(fv, v) + 10 : 0;
    if (text_w(fa, a) + wv > room) fa = &FONT_M;
    if (text_w(fa, a) + wv > room && v[0]) { fv = &FONT_M; wv = text_w(fv, v) + 10; }
    if (text_w(fa, a) + wv > room && v[0]) { fv = &FONT_B; wv = text_w(fv, v) + 10; }   /* (a long value: "SAW>SQR 96") */
    for (i = 0; a[i] && a[i] == la[i]; i++)
        ;
    if (!a[i] && !la[i] && fa == lfa && fv == lfv && batt == lbatt && a[0])
        ox = 7 + text_w(fa, a) + 2;             /* (the name as it is: from its end) */
    for (i = 0; i < 23 && a[i]; i++) la[i] = a[i];
    la[i] = 0;
    lfa = fa; lfv = fv; lbatt = batt;
    for (b0 = 0; b0 < h; b0 += UI_BAND) {        /* in bands (the canvas holds UI_BAND rows: pool) */
        int32_t bh = h - b0 < UI_BAND ? h - b0 : UI_BAND;
        cv_begin((uint32_t)(w - ox), (uint32_t)bh, U_LCDBG);
        cv_oy = -b0; cv_y0 = 0; cv_y1 = bh;
        cv_rect(0, 0, w, 1, U_LCDRIM); cv_rect(0, h - 1, w, 1, U_LCDRIM);   /* one red, its rim */
        if (!ox) cv_rect(0, 0, 1, h, U_LCDRIM);
        cv_rect(w - 1 - ox, 0, 1, h, U_LCDRIM);
        if (!ox) cv_text(7, (h - fa->h) / 2, fa, a, U_LCDTXT);
        if (v[0]) cv_text(7 + room - wv + 10 - ox, (h - fv->h) / 2, fv, v, U_LCDTXT);
        if (batt >= 0) {                         /* the battery, 15 x 8 (4: on USB power, shown full) */
            int32_t bx = w - 24 - ox, by = 7, f = batt > 3 ? 3 : batt;
            cv_rect(bx, by, 14, 1, U_LCDTXT); cv_rect(bx, by + 7, 14, 1, U_LCDTXT);
            cv_rect(bx, by, 1, 8, U_LCDTXT); cv_rect(bx + 13, by, 1, 8, U_LCDTXT);
            cv_rect(bx + 14, by + 2, 2, 4, U_LCDTXT);
            cv_rect(bx + 2, by + 2, 3 * f, 4, U_LCDTXT);
        }
        cv_oy = 0;
        cv_blit((uint32_t)(4 + ox), (uint32_t)(4 + b0));
    }
    prof_end(0);
}
#undef la
static void u_theme(uint32_t b)          /* the colours of part A (0), B (1, green) or the drums' pages (2, amber); the
                                          * screen is drawn again (the caller: view_reset) */
{
    u_th = &U_THEMES[b < 4u ? b : 0u];
    lcd_la[0] = 0;                       /* (the strip: all of it) */
}

/* a row: four knobs with their labels and values, in columns over the whole width (0.87: the tag up the left side
 * gone, its room the columns'); lit when K1..K4 act on it (its frame and text in the accent; the block's name is in
 * the strip); a knob that does nothing in the delay / reverb mode now (p_idle): dim */
#define UI_COL_W 57                      /* a column's width (53 with the tag, up to 0.86) */
#define UI_COL_X(k) (2 + (int32_t)(k) * UI_COL_W)   /* a knob's column in a row: its left (the frame and a pixel) */
#define UI_COL_C 28                      /* its middle from its left */
#define UI_VAL_W 55                      /* a value's room in its column (and the label's) */
#define UI_RKNOB 40                      /* the rows' knobs (34 up to 0.86; PLAY mode's boxes keep UI_KNOB) */
static uint32_t row_big(uint32_t row, const uint8_t *patch)   /* bit k: value k in the big font (where it fits its
                                                     * column, UI_VAL_W, and leaves room to the next one: of two
                                                     * neighbours too wide together, the wider takes the smaller font) */
{
    const u_row_t *R = &U_ROW[row];
    int32_t w[4];
    uint32_t k, big = 0;
    for (k = 0; k < 4u; k++) {
        const u_param_t *p = &R->p[k];
        char t[16];
        w[k] = 0;
        if (!p->label[0]) continue;
        value_text(p, patch[p_byte(p)], t);
        w[k] = text_w(&FONT_L, t);
        if (w[k] <= UI_VAL_W) big |= 1u << k;
    }
    for (k = 0; k + 1u < 4u; k++)
        if ((big >> k & 3u) == 3u && (w[k] + w[k + 1u]) / 2 > UI_COL_W - 8)
            big &= ~(1u << (w[k] >= w[k + 1u] ? k : k + 1u));
    return big;
}
/* a column's knob, label and value (the menus' rows, the drum kit's): cx its middle, H the row's height; f the font
 * the value wants (FONT_L or FONT_M), smaller ones where it is too wide: FONT_B, FONT_S (0.87), FONT_XS, cut */
static void u_col_knob(int32_t cx, int32_t H, float v, int bi, int on, const char *lb, char *s,
                       const felucca_font_t *f, uint16_t vc)
{
    knob(cx - UI_RKNOB / 2, 2, v, bi, on, UI_RKNOB);
    cv_text(cx - text_w(&FONT_XS, lb) / 2, UI_RKNOB + 1, &FONT_XS, lb, on ? RGB(242, 242, 243) : RGB(150, 150, 156));
    if (text_w(f, s) > UI_VAL_W) f = &FONT_B;
    if (text_w(f, s) > UI_VAL_W) f = &FONT_S;
    if (text_w(f, s) > UI_VAL_W) f = &FONT_XS;
    while (text_w(f, s) > UI_VAL_W) s[text_len(s) - 1u] = 0;
    cv_text(cx - text_w(f, s) / 2, H - 2 - f->h + (f == &FONT_L ? 2 : f == &FONT_M ? -2 : -6), f, s, vc);
}
static void row_body_col(uint32_t row, int on, const uint8_t *patch, int32_t hl, int32_t col)   /* one row of knobs,
                                                     * its own coordinates (draw_row draws it in bands); col >= 0: that
                                                     * column alone, in a canvas of its width */
{
    const u_row_t *R = &U_ROW[row];
    uint16_t bg, bd;
    uint32_t k, big = row_big(row, patch);
    const int32_t H = UI_ROW_H, ox = col < 0 ? 0 : UI_COL_X(col);
    bg = on ? mix565(U_BG, u_acc, 28u) : U_ROWOFF;
    bd = on ? u_acc : U_BORDER;
    if (col >= 0) {                              /* the column: the frame's top and bottom over it, its body */
        cv_rect(0, 0, UI_COL_W, H, bg);
        cv_rect(0, 0, UI_COL_W, 1, bd); cv_rect(0, H - 1, UI_COL_W, 1, bd);
    } else {
        cv_rect(0, 0, 232, H, bd);               /* the frame, then the body inside it */
        cv_rect(1, 1, 230, H - 2, bg);
    }
#ifdef HYB_MULTI
    uint32_t pa = 0, lb = 0;             /* (part A's columns, p_parta: two or more side by side, the text instead) */
#ifdef HYB_SHIM_LEND
    uint32_t bo = 0;                     /* (1.04: the shimmer's with Bluetooth on, P_BTOFF: the same, its own text) */
#endif
    for (k = 0; k < 4u; k++) {
        if (p_parta(&R->p[k])) pa |= 1u << k;
#ifdef HYB_SHIM_LEND
        if (P_BTOFF(&R->p[k])) bo |= 1u << k;
#endif
        if (R->p[k].label[0]) lb |= 1u << k;
    }
#ifdef HYB_SHIM_LEND
    pa &= ~bo;
    bo &= (bo << 1) | (bo >> 1);
#endif
    pa &= (pa << 1) | (pa >> 1);         /* (one alone: a knob, dim) */
    if (pa) {                            /* (a run: from its first column to its last; all of them: the row's width) */
        const int32_t a = (pa == lb ? 0 : UI_COL_X(__builtin_ctz(pa))) - ox,
                      b = (pa == lb ? 232 : UI_COL_X(32 - __builtin_clz(pa))) - ox;
        const uint16_t c = on ? U_VALON : RGB(176, 176, 182);
        u_text_fit((a + b) / 2, H / 2 - 22, b - a - 4, "CHANGE IN PART A", c);
        u_text_fit((a + b) / 2, H / 2 + 6, b - a - 4, "REC: GO TO PART A", c);
    }
#ifdef HYB_SHIM_LEND
    if (bo) {                            /* (as part A's, over the shimmer's columns) */
        const int32_t a = (bo == lb ? 0 : UI_COL_X(__builtin_ctz(bo))) - ox,
                      b = (bo == lb ? 232 : UI_COL_X(32 - __builtin_clz(bo))) - ox;
        const uint16_t c = on ? U_VALON : RGB(176, 176, 182);
        u_text_fit((a + b) / 2, H / 2 - 22, b - a - 4, "SHIMMER: BT OFF", c);
        u_text_fit((a + b) / 2, H / 2 + 6, b - a - 4, "GLO: BT OFF, SAVE, RESTART", c);
        pa |= bo;
    }
#endif
#endif
    for (k = 0; k < 4u; k++) {
        const u_param_t *p = &R->p[k];
        const int pon = on && !p_idle(p) && !p_parta(p) && !P_BTOFF(p);   /* (the knob lit) */
        uint32_t v, mx;
        int32_t cx = UI_COL_X(k) + UI_COL_C - ox;   /* the column's middle */
        char s[16];
        if (!p->label[0] || (col >= 0 && (int32_t)k != col))
            continue;
#ifdef HYB_MULTI
        if (pa >> k & 1u) continue;
#endif
        v = patch[p_byte(p)];
        mx = p_max(p);
        if ((int32_t)k == hl) {         /* the parameter turned last: a frame round its column */
            cv_rect(cx - 27, 2, 55, 1, U_LCDTXT); cv_rect(cx - 27, H - 3, 55, 1, U_LCDTXT);
            cv_rect(cx - 27, 2, 1, H - 4, U_LCDTXT); cv_rect(cx + 27, 2, 1, H - 4, U_LCDTXT);
        }
        value_text(p, v, s);
        if (p_parta(p) || P_BTOFF(p)) { s[0] = s[1] = '-'; s[2] = 0; }
        if (p->kind == V_SHAPE && text_w(&FONT_XS, s) > UI_VAL_W) put_sint(s, (int32_t)v, 0);   /* (the words in the
                                                     * strip) */
        u_col_knob(cx, H, (float)(v > mx ? mx : v) / (float)mx, p->kind == V_BI || p->kind == V_SEMI, pon, p_label(p), s,
                   big >> k & 1u ? &FONT_L : &FONT_M, pon ? U_VALON : RGB(176, 176, 182));
    }
}

static void row_body(uint32_t row, int on, const uint8_t *patch, int32_t hl)
{
    row_body_col(row, on, patch, hl, -1);
}

/* a row of knobs in bands of the canvas (UI_BAND), one after the other (the canvas holds the LCD strip at most: pool);
 * hl: the knob turned last, framed (-1 none) */
static void draw_row(uint32_t sec, int on, const uint8_t *patch, int32_t y0, uint32_t slot, int32_t hl)
{
    int32_t b0;
    prof_begin();
    for (b0 = 0; b0 < UI_ROW_H; b0 += UI_BAND) {
        int32_t bh = UI_ROW_H - b0 < UI_BAND ? UI_ROW_H - b0 : UI_BAND;
        cv_begin(232, (uint32_t)bh, U_BG);
        cv_oy = -b0;                     /* (the band's rows of the row: drawing below it and above it clips) */
        cv_y0 = 0;
        cv_y1 = bh;
        row_body(sec, on, patch, hl);
        cv_oy = 0;
        cv_blit(4, (uint32_t)(y0 + b0));
    }
    prof_end(2 + slot);
}

/* one column of a row (a knob turned: its knob and value, nothing else drawn again) */
static void draw_row_col(uint32_t sec, int on, const uint8_t *patch, int32_t y0, int32_t col, int32_t hl)
{
    int32_t b0;
    for (b0 = 0; b0 < UI_ROW_H; b0 += UI_BAND) {
        int32_t bh = UI_ROW_H - b0 < UI_BAND ? UI_ROW_H - b0 : UI_BAND;
        cv_begin(UI_COL_W, (uint32_t)bh, U_BG);
        cv_oy = -b0;
        cv_y0 = 0;
        cv_y1 = bh;
        row_body_col(sec, on, patch, hl, col);
        cv_oy = 0;
        cv_blit((uint32_t)(4 + UI_COL_X(col)), (uint32_t)(y0 + b0));
    }
}

/* the space of the two rows (or a part of it) drawn by body(h) in bands of the canvas, as draw_row does: y0 its top on
 * the screen, h its height */
static UI_LATE void bands(int32_t x, int32_t y, int32_t wd, int32_t h, void (*body)(int32_t))   /* (1.03: one loop
                                          * for draw_body and a sequencer's or the kit's part alone) x, y on the
                                          * screen, wd wide */
{
    int32_t b0;
    for (b0 = 0; b0 < h; b0 += UI_BAND) {
        const int32_t bh = h - b0 < UI_BAND ? h - b0 : UI_BAND;
        cv_begin((uint32_t)wd, (uint32_t)bh, U_BG);
        cv_oy = -b0;
        cv_y0 = 0;
        cv_y1 = bh;
        body(h);
        cv_oy = 0;
        cv_blit((uint32_t)x, (uint32_t)(y + b0));
    }
}
#define draw_body(y0, h, body) bands(4, (y0), 232, (h), (body))
#define part_bands(x, y, wd, h, body) bands(4 + (x), UI_ROW0_Y + (y), (wd), (h), (body))   /* (1.03) a part of a
                                          * sequencer's or the kit's page alone, at x, y in the body, in a canvas of
                                          * its size, in bands (seq_draw, kit_part, pg_part, draw_play_cell) */
static void draw_body_part(int32_t y0, int32_t h, int32_t from, int32_t to, void (*body)(int32_t))   /* only the
                                                     * bands that meet rows from .. to - 1 */
{
    int32_t b0;
    for (b0 = 0; b0 < h; b0 += UI_BAND) {
        int32_t bh = h - b0 < UI_BAND ? h - b0 : UI_BAND;
        if (b0 >= to || b0 + bh <= from)
            continue;
        cv_begin(232, (uint32_t)bh, U_BG);
        cv_oy = -b0;
        cv_y0 = 0;
        cv_y1 = bh;
        body(h);
        cv_oy = 0;
        cv_blit(4, (uint32_t)(y0 + b0));
    }
}
#define UI_BODY_H (UI_ROW1_Y + UI_ROW_H - UI_ROW0_Y)   /* the two rows' space, 171 rows from UI_ROW0_Y */
#define UI_HOME_A 76                     /* the HOME screen: its tiles, then its connections below */

static void u_frame(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t rim, uint16_t bg)
{
    cv_rect(x, y, w, h, rim);
    cv_rect(x + 1, y + 1, w - 2, h - 2, bg);
}
static void u_text_c(int32_t y, const felucca_font_t *f, const char *s, uint16_t c)   /* centred */
{
    cv_text((232 - text_w(f, s)) / 2, y, f, s, c);
}

/* the HOME screen's figures, taken once a draw (a draw's bands all show the same) */
typedef struct {
    uint8_t cpu, voices, maxv, lat, hold, bpm;   /* the tiles */
    uint8_t pk, xr;                      /* the longest block of the last look (%), a dropout lately */
#ifdef HYB_MULTI
    uint8_t vb;                          /* two parts: part B's voices + 1 (0: SINGLE, A's in voices) */
    uint8_t cap;                         /* MAX VOICES under the latency's (the LATENCY tile: "HIGH 14") */
#endif
    int8_t oct, db, batt, tr;            /* (batt: the bar at the bottom; tr: the transpose tile) */
    uint8_t usb, act;                    /* the connections: USB host; act bit PORT_*: MIDI came in lately */
    uint8_t bt_off, bt, bt_host, bt_dev; /* Bluetooth: switched off, plat_ble_state, the computer (2 listening, 1
                                          * linked), plat_ble_device */
    char dev[21];                        /* the device's name */
#ifdef HYB_MULTI
    uint8_t mx_mode, mx_ed, mx_ch;       /* two parts: the mode, the part edited, B's channel (0 none) */
    char mx_a[24], mx_b[24];             /* their patches ("A001 NAME") */
#endif
} u_home_t;
static u_home_t hm;

static void home_tile(int32_t i, const char *label, const char *v, uint16_t c, int32_t bar)   /* bar: % or -1 */
{
    int32_t x = 1 + (i & 3) * 58, y = (i >> 2) * 38;
    const felucca_font_t *f = text_w(&FONT_B, v) > 48 ? &FONT_XS : &FONT_B;
    u_frame(x, y, 56, 35, U_BORDER, U_ROWOFF);
    cv_text(x + 5, y + 2, &FONT_XS, label, U_DIMTXT);
    cv_text(x + 5, y + 16 + (f == &FONT_XS), f, v, c);
    if (bar >= 0) {
        cv_rect(x + 5, y + 31, 46, 2, U_TRACK);
        cv_rect(x + 5, y + 31, (bar > 100 ? 100 : bar) * 46 / 100, 2, u_acc);
    }
}
static UI_SMALL void home_tiles(int32_t h)        /* CPU (the core's total), voices, latency, BPM; HOLD, octave, battery, volume */
{
    char s[12];
    (void)h;
    {   /* the load, and the peak (the longest block) in the label, red after a dropout */
        char lb[12] = "CPU  PK ";
        int32_t x = 1 + 5;
        put_sint(lb + 8, hm.pk, 0);
        if (text_w(&FONT_XS, lb) > 50) {         /* (a peak of three digits: one space, from a pixel further left,
                                                 * inside the tile's 56) */
            lb[3] = 0; text_cat(lb, " PK"); put_sint(lb + 6, hm.pk, 0);
            x = 1 + 3;
        }
        put_sint(s, hm.cpu, 0); text_cat(s, "%");
        home_tile(0, "", s, U_TEXT, hm.cpu);
        cv_text(x, 2, &FONT_XS, lb, hm.xr ? RGB(255, 60, 40) : U_DIMTXT);
    }
    if (!hm.maxv)                        /* (ENGINE without the synth: hm.voices its mode, app.c EN_*) */
        home_tile(1, "ENGINE", hm.voices == 1u ? "DRUMS" : "MIDI", U_TEXT, -1);
    else {
        char t[6];
        put_sint(s, hm.voices, 0); text_cat(s, "/"); put_sint(t, hm.maxv, 0); text_cat(s, t);
#ifdef HYB_MULTI
        if (hm.vb) {                     /* (two parts: "A+B/MAX") */
            put_sint(s, hm.voices, 0); text_cat(s, "+"); put_sint(t, hm.vb - 1, 0); text_cat(s, t);
            text_cat(s, "/"); put_sint(t, hm.maxv, 0); text_cat(s, t);
        }
#endif
        home_tile(1, "VOICES", s, U_TEXT, -1);
    }
#ifdef HYB_MULTI
    s[0] = 0;
    text_cat(s, hm.lat == 3u ? "HIGH " : hm.lat == 2u ? "MED " : "LOW ");
    put_sint(s + text_len(s), hm.cap, 0);
    home_tile(2, "LATENCY", s, U_TEXT, -1);
#else
    home_tile(2, "LATENCY", hm.lat == 3u ? "HIGH 16" : hm.lat == 2u ? "MED 14" : "LOW", U_TEXT, -1);   /* (app.c LAT_*) */
#endif
    put_sint(s, hm.bpm, 0);
    home_tile(3, "BPM", s, U_TEXT, -1);
#ifdef HYB_MULTI
    home_tile(4, "HOLD", hm.hold & 1u ? "ON" : hm.hold ? (ed_part ? "A ON" : "B ON") : "OFF", hm.hold & 1u ? u_acc : U_TEXT,
              -1);                       /* (1.15: the part edited's HOLD off, the other part's on: its letter) */
#else
    home_tile(4, "HOLD", hm.hold ? "ON" : "OFF", hm.hold ? u_acc : U_TEXT, -1);
#endif
    put_sint(s, hm.oct, 1);
    home_tile(5, "OCTAVE", s, U_TEXT, -1);
    put_sint(s, hm.tr, 1);
    home_tile(6, "TRANSP", s, U_TEXT, -1);
    put_sint(s, hm.db, 0); text_cat(s, " dB");
    home_tile(7, "VOLUME", s, U_TEXT, -1);
}
static void home_led(int32_t x, int32_t y, int on)   /* a light, 7 x 7, by a line of text at y */
{
    cv_rect(x, y + 4, 7, 7, on ? u_acc : U_TRACK);
}
static void home_line(int32_t i, const char *label, const char *v, int on)   /* a connection: its light, its state */
{
    int32_t y = 4 + i * 18;
    home_led(4, y, on);
    cv_text(16, y, &FONT_XS, label, U_STATUS);
    cv_text(92, y, &FONT_XS, v, on ? U_TEXT : U_DIMTXT);
}
static void home_conn(int32_t h)         /* MIDI in by port; USB, the computer and the device over Bluetooth */
{
    static const char *const P[NPORTS] = {"USB", "BT", "TRS"};   /* (PORT_*) */
    static const uint8_t ORDER[NPORTS] = {PORT_USB, PORT_TRS, PORT_BLE};
    char nm[21];
    int32_t i, x = 92;
    (void)h;
    cv_rect(0, 0, 232, 1, U_RULE);
    cv_text(16, 4, &FONT_XS, "MIDI IN", U_STATUS);
    for (i = 0; i < NPORTS; i++) {       /* a light a port, lit a moment when a message comes in */
        uint32_t p = ORDER[i];
#if !FM1B_BLE
        if (p == PORT_BLE) continue;     /* (the USB build: no Bluetooth) */
#endif
        home_led(x, 4, hm.act >> p & 1u);
        x = cv_text(x + 11, 4, &FONT_XS, P[p], hm.act >> p & 1u ? U_TEXT : U_DIMTXT) + 12;
    }
#if FM1B_BLE && defined(HYB_MULTI)
    {   /* (the Bluetooth build with two parts: USB and the computer over Bluetooth on one line, the
         * parts below; the device over Bluetooth on GLO > BLUETOOTH) */
        const int bt = !hm.bt_off && hm.bt == BLE_READY;
        char u[28];
        u[0] = 0;
        text_cat(u, hm.usb ? "ON" : "OFF");
        text_cat(u, "   BT ");
        text_cat(u, hm.bt_off ? "OFF" : hm.bt == BLE_STARTING ? "STARTING" : hm.bt == BLE_FAILED ? "FAILED"
                 : !bt ? "OFF" : hm.bt_host == 2u ? "CONNECTED" : hm.bt_host ? "LINKED" : "WAITING");
        home_line(1, "USB", u, hm.usb || (bt && hm.bt_host == 2u));
    }
#else
    home_line(1, "USB", hm.usb ? "CONNECTED" : "NOT CONNECTED", hm.usb);
#endif
#if defined(HYB_MULTI)
    {   /* the two parts: each its patch, lit while it is edited; B's label the mode */
        static const char *const MB[4] = {"PART B", "B LAYER", "B SPLIT", "B CH "};   /* (SY_M_*) */
        char lb[12];
        lb[0] = 0;
        text_cat(lb, MB[hm.mx_mode & 3u]);
        if (hm.mx_mode == 3u) { if (hm.mx_ch > 16u) text_cat(lb, "OMNI"); else if (hm.mx_ch) put_sint(lb + 5, hm.mx_ch, 0); else text_cat(lb, "OFF"); }   /* (1.19.4: B IN OMNI) */
        (void)nm;
        home_line(2, "PART A", hm.mx_a, hm.mx_mode && !hm.mx_ed);
        home_line(3, lb, hm.mx_mode ? hm.mx_b : "OFF", hm.mx_mode && hm.mx_ed);
        if (!hm.mx_mode) cv_text(92 + text_w(&FONT_XS, "OFF") + 8, 4 + 3 * 18, &FONT_XS, hm.mx_b, U_DIMTXT);   /* (B's
                                                     * patch, waiting) */
    }
#elif !FM1B_BLE
    (void)nm;
    home_line(2, "BLUETOOTH", "NOT IN THIS BUILD", 0);   /* (the USB build) */
#else
    home_line(2, "BT HOST", hm.bt_off ? "OFF" : hm.bt == BLE_STARTING ? "STARTING" : hm.bt == BLE_FAILED ? "FAILED"
              : hm.bt != BLE_READY ? "OFF" : hm.bt_host == 2u ? "CONNECTED" : hm.bt_host ? "LINKED" : "WAITING",
              !hm.bt_off && hm.bt == BLE_READY && hm.bt_host == 2u);
    for (i = 0; i < 20 && hm.dev[i]; i++)
        nm[i] = hm.dev[i];
    nm[i] = 0;
    while (i > 0 && text_w(&FONT_XS, nm) > 232 - 92 - 4)   /* (a long name: its end left out) */
        nm[--i] = 0;
    home_line(3, "BT DEVICE", hm.bt_off ? "OFF" : hm.bt_dev == BLE_DEV_READY ? nm
              : hm.bt_dev == BLE_DEV_SCANNING ? "SCANNING" : hm.bt_dev != BLE_DEV_OFF ? "CONNECTING" : "NONE",
              !hm.bt_off && hm.bt_dev == BLE_DEV_READY);
#endif
    {   /* the battery: a bar along the bottom, grey, filled red by the charge (on USB: full, "USB") */
        const int32_t y = h - 18;
        cv_rect(0, y, 232, 16, U_TRACK);
        if (hm.batt > 3)
            cv_rect(0, y, 232, 16, RGB(120, 120, 126));
        else if (hm.batt > 0)
            cv_rect(0, y, 232 * hm.batt / 3, 16, U_TH.batt);
        if (hm.batt > 3 || hm.batt < 0)
            u_text_c(y + 1, &FONT_XS, hm.batt > 3 ? "USB" : "BATTERY --", U_TEXT);
    }
}

/* ---------------------------------------------------------------- lists --- */
/* a list over the two rows' space: LS_N lines of the big font, the cursor's line inverted on the accent, scrolled so
 * the cursor stays in the middle; its text too long for the line runs along it (ls_tick), the others are cut. The
 * lines come from item(i): the text, a prefix (a slot's number) and flags; a value after a tab in the text stands at
 * the line's right end (GLO's "THRU <tab> OFF") */
#define LS_N 5
#define LS_H 34                          /* a line's rows, its gap included (5 x 34 of UI_BODY_H's 171) */
#define LS_TY ((LS_H - 2 - 24) / 2)      /* the text's top in its line (FONT_M, 24 rows) */
enum { LS_EMPTY = 1, LS_DOT = 2, LS_LED = 4, LS_LIT = 8, LS_STAR = 16 };   /* an empty slot (a box); a
                                          * patch with edits; a light at the line's end (0.92: the matrix's slots), lit; a
                                          * favorite (1.18C33: a star at the line's end). (1.19.1 .. 1.19.2: LS_DIM, a
                                          * line dimmed: GLO > MIDI > B CHANNEL in SINGLE; 1.19.3 none) */
typedef void (*ls_item_fn)(uint32_t i, char *t, char *pre, uint32_t *fl);
typedef void (*ls_foot_fn)(char *l, char *r);   /* a list's foot strip: the bank letters before / after ("" none) */
static ls_foot_fn ls_foot;               /* (set by the list's owner after ls_set; the list then has one line less) */
static struct {
    ls_item_fn item;
    uint16_t n, cur;
    int16_t mq;                          /* the cursor's text run on by this (pixels) */
    uint8_t mq_hold;                     /* ticks still to wait at an end */
    uint32_t mq_t;
} ls;

static uint32_t ls_top(void)
{
    const uint32_t r = ls_foot ? LS_N - 1u : LS_N;
    uint32_t top = ls.cur > r / 2u ? ls.cur - r / 2u : 0u;
    if (ls.n <= r) return 0u;
    return top > ls.n - r ? ls.n - r : top;
}
static void ls_set(ls_item_fn item, uint32_t n, uint32_t cur)   /* the list up now (its run starts again on a change) */
{
    if (item != ls.item || cur != ls.cur) {
        ls.mq = 0;
        ls.mq_hold = 6;
    }
    ls.item = item;
    ls_foot = 0;
    ls.n = (uint16_t)n;
    ls.cur = (uint16_t)cur;
}
#define LS_VAL_W 120                     /* a value's room at most (a longer one is cut) */
static char *ls_val(char *t)             /* the value after a tab in t, cut off it and to its room ("" none) */
{
    char *v = t;
    uint32_t n;
    while (*v && *v != '\t')
        v++;
    if (!*v)
        return v;
    *v++ = 0;
    for (n = text_len(v); n > 0 && text_w(&FONT_M, v) > LS_VAL_W; n--)
        v[n - 1u] = 0;
    return v;
}
static void ls_win(const char *pre, uint32_t fl, const char *v, int32_t *x0, int32_t *x1)   /* the text's room on a line */
{
    *x0 = pre[0] ? 40 : 10;
    *x1 = fl & LS_LED ? 224 : (fl & LS_DOT ? 210 : 224) - (fl & LS_STAR ? 18 : 0) - (v[0] ? text_w(&FONT_M, v) + 12 : 0);   /* (a light's
                                          * line: its value placed by ls_led) */
}
/* (0.92) a line's light at its right end, the LFO page's look (led_draw: a disc of 9 pixels, U_TH.led lit, U_TRACK
 * dark; on the cursor's row the text's colour lit, its colour darkened unlit), and its value before it in the
 * largest font that leaves 10 pixels to the line's text (FONT_M, FONT_B, FONT_S, cut) */
#define LS_LED_X 214
static UI_LATE void ls_led(const char *t, char *v, uint32_t fl, int32_t y, int on, uint16_t bg)
{
    const int32_t room = LS_LED_X - 6 - (10 + text_w(&FONT_M, t) + 10);
    const uint16_t c = fl & LS_LIT ? (on ? U_ONCUR : U_TH.led) : (on ? mix565(U_CUR, U_ONCUR, 80u) : U_TRACK);
    const int32_t ly = y + (LS_H - 2 - 9) / 2;
    int32_t i, j;
    if (v[0]) {
        const felucca_font_t *f = text_w(&FONT_M, v) <= room ? &FONT_M : text_w(&FONT_B, v) <= room ? &FONT_B : &FONT_S;
        uint32_t n = text_len(v);
        while (n > 0u && text_w(f, v) > room) v[--n] = 0;
        cv_text(LS_LED_X - 6 - text_w(f, v), y + (LS_H - 2 - f->h) / 2, f, v, on ? U_CURVAL : u_acc);
    }
    for (j = 0; j < 9; j++)
        for (i = 0; i < 9; i++) {
            const float dx = (float)i - 4.f, dy = (float)j - 4.f;
            const uint32_t a = cover(4.3f - fm_sqrtf(dx * dx + dy * dy));
            if (a) cv_blend(LS_LED_X + i, ly + j, c, a);
        }
    (void)bg;
}
static UI_LATE void ls_star(int32_t x, int32_t y, uint16_t c)   /* (1.18C33) a favorite's star, 15 x 15 at x, y: a
                                          * row a word, a pixel's coverage in 2 bits (0, 1/3, 2/3, all; a five-point
                                          * star of radius 7 sampled 4 x 4 a pixel) */
{
    static const uint32_t ST[15] = {0u, 0x00004000u, 0x0000C000u, 0x0001D000u, 0x0002E000u, 0x0003F000u, 0x1BFFFFF9u,
                                    0x07FFFFE0u, 0x00BFFF80u, 0x002FFE00u, 0x002FFE00u, 0x003FBF00u, 0x007D1F40u,
                                    0x00A00280u, 0u};
    int32_t i, j;
    for (j = 0; j < 15; j++)
        for (i = 0; i < 15; i++)
            cv_blend(x + i, y + j, c, (ST[j] >> (2 * i) & 3u) * 86u);
}
static void ls_line(uint32_t i, int32_t y, int32_t run)
{
    char t[40], pre[6], *v;
    uint32_t fl = 0;
    int32_t x0, x1;
    const int on = i == ls.cur;
    const uint16_t bg = on ? U_CUR : U_ROWOFF, tc = on ? U_ONCUR : U_TEXT;
    t[0] = pre[0] = 0;
    ls.item(i, t, pre, &fl);
    v = ls_val(t);
    ls_win(pre, fl, v, &x0, &x1);
    u_frame(0, y, 232, LS_H - 2, on ? U_CUR : U_BORDER, bg);
    if (fl & LS_EMPTY) {                 /* (an empty slot: its box, after its text if any: SAVE's "C012") */
        const int32_t xb = t[0] ? cv_text(x0, y + LS_TY, &FONT_M, t, on ? tc : U_DIMTXT) + 10 : x0;
        u_frame(xb, y + LS_TY + 7, 60, 10, on ? U_ONCUR : U_BORDER, bg);
    } else {
        cv_text(x0 - run, y + LS_TY, &FONT_M, t, tc);
        cv_rect(1, y + 1, x0 - 3, LS_H - 4, bg);     /* (the text cut at its room) */
        cv_rect(x1 + 2, y + 1, 229 - x1, LS_H - 4, bg);
    }
    if (pre[0]) cv_text(10, y + LS_TY, &FONT_M, pre, on ? tc : U_STATUS);
    if (fl & LS_LED) ls_led(t, v, fl, y, on, bg);
    else if (v[0]) cv_text(224 - text_w(&FONT_M, v), y + LS_TY, &FONT_M, v, on ? U_CURVAL : u_acc);
    if (fl & LS_DOT) cv_rect(218, y + LS_TY + 8, 7, 7, on ? U_ONCUR : u_acc);
    if (fl & LS_STAR) ls_star(fl & LS_DOT ? 196 : 210, y + (LS_H - 2 - 15) / 2, on ? U_ONCUR : u_acc);
}
static UI_LATE void ls_ask(const char *q, const char *h)   /* (0.96) the question over the list's lines 3 to 5 (the
                                          * cursor's, INIT, on the first): the name editor's look (nm_body). 1.18C33:
                                          * RESET PATCH's, a favorite's removal (RANDOMIZE asks nothing: REC acts) */
{
    u_frame(0, 2 * LS_H, 232, UI_BODY_H - 2 * LS_H, u_acc, mix565(U_BG, u_acc, 40u));
    u_text_c(2 * LS_H + 14, &FONT_L, q, U_TEXT);
    u_text_c(2 * LS_H + 62, &FONT_B, h, u_acc);
}
static void ls_arrow(int32_t x, int32_t y, int32_t d)   /* an arrow of 16 x 8 pixels, d < 0 left */
{
    int32_t k;
    cv_rect(x, y, 16, 2, u_acc);
    for (k = 0; k < 4; k++) cv_rect(d < 0 ? x + k : x + 15 - k, y - k, 1, 2 + 2 * k, u_acc);
}
static UI_LATE void ls_foot_draw(void)   /* the strip under the lines: "A <- OCT-" | "OCT+ -> C" (a side with no letter: blank) */
{
    char l[4], r[4];
    const int32_t y = (int32_t)(LS_N - 1u) * LS_H, ty = y + (LS_H - 1 - FONT_B.h) / 2, ay = y + LS_H / 2 - 2;
    int32_t x;
    l[0] = r[0] = 0;
    ls_foot(l, r);
    u_frame(0, y, 232, LS_H - 1, u_acc, mix565(U_BG, u_acc, 40u));
    cv_rect(116, y + 4, 1, LS_H - 9, u_acc);
    if (l[0]) {
        x = cv_text(8, ty, &FONT_B, l, U_TEXT) + 6;
        ls_arrow(x, ay, -1);
        cv_text(x + 22, ty, &FONT_B, "OCT-", U_TEXT);
    }
    if (r[0]) {
        x = cv_text(124, ty, &FONT_B, "OCT+", U_TEXT) + 6;
        ls_arrow(x, ay, 1);
        cv_text(x + 22, ty, &FONT_B, r, U_TEXT);
    }
}
static void ls_body(int32_t h)
{
    uint32_t i, top = ls_top();
    (void)h;
    if (ls_foot) ls_foot_draw();
    for (i = top; i < ls.n && i < top + (ls_foot ? LS_N - 1u : LS_N); i++)
        ls_line(i, (int32_t)(i - top) * LS_H, i == ls.cur ? ls.mq : 0);
    if (ui.rp_ask == 1u && ui.plist_until) ls_ask("RESET PATCH ?", "REC  RESET      HOME  BACK");   /* (0.96: the
                                          * patch list's question, app.c rp_*) */
    else if (fv.ask && ui.plist_until && !fv.ed) ls_ask("REMOVE FAV ?", "REC  REMOVE    HOME  BACK");   /* (1.18C33) */
}
static void ls_cur_body(int32_t h) { (void)h; ls_line(ls.cur, 0, ls.mq); }
/* the cursor's text, too long for its line, runs along it: a few pixels every 150 ms, a pause at each end; only its
 * line drawn again (y0: the list's top on the screen) */
static void ls_tick(uint32_t now, int32_t y0)
{
    char t[40], pre[6], *v;
    uint32_t fl = 0;
    int32_t x0, x1, over;
    if (!ls.item || now - ls.mq_t < 150u)
        return;
    ls.mq_t = now;
    t[0] = pre[0] = 0;
    ls.item(ls.cur, t, pre, &fl);
    v = ls_val(t);
    ls_win(pre, fl, v, &x0, &x1);
    over = text_w(&FONT_M, t) - (x1 - x0);
    if (over <= 0 || (fl & LS_EMPTY))
        return;
    if (ls.mq_hold) {
        if (--ls.mq_hold) return;
        if (!ls.mq) return;              /* (the start's pause over: it runs from the next tick) */
        ls.mq = 0;                       /* (the end's pause over: back to the start) */
        ls.mq_hold = 6;
    } else if ((ls.mq += 4) >= over) {
        ls.mq = (int16_t)over;
        ls.mq_hold = 6;
    }
    draw_body(y0 + (int32_t)(ls.cur - ls_top()) * LS_H, LS_H - 2, ls_cur_body);
}

/* an entry in a row's place (EDIT, BACK): lit (REC acts on it) or below the lit row, what comes next */
static struct { const char *w; uint8_t on; } ue;
static void entry_body(int32_t h)
{
    u_frame(0, 0, 232, h, ue.on ? u_acc : U_BORDER, ue.on ? mix565(U_BG, u_acc, 28u) : U_ROWOFF);
    if (ue.on) {
        u_text_c(8, &FONT_L, ue.w, U_TEXT);
        u_text_c(48, &FONT_B, ue.w[0] == 'B' ? "REC  BACK" : "REC  OPEN", u_acc);
    } else {
        char s[16];
        s[0] = 0;
        text_cat(s, "NEXT  ");
        text_cat(s, ue.w);
        u_text_c((h - FONT_M.h) / 2, &FONT_M, s, U_DIMTXT);
    }
}
static void draw_entry(const char *w, int on, int32_t y0)
{
    ue.w = w;
    ue.on = (uint8_t)on;
    draw_body(y0, UI_ROW_H, entry_body);
}

/* (0.92) the matrix page's slot list (MATRIX > EDIT): each slot's line ends in a light, lit while the slot is in use,
 * and its destination's short name before it. In use: a source (not OFF), a destination (not OFF; a shimmer target
 * counts) and an amount off the centre (64 = 0). A destination the reference leaves alone ("--") still counts: the
 * slot is set. The short names, 0 .. 127 (1.00, the engine's mapping): 0 .. 71 the
 * panel's list's; 72 .. 81 the velocity amounts (B 47-50, 54-57, 60, 61), 88 the oscillators' init phase, 90 the
 * ring modulator, 91 the noise colour, 92 the effects' colour, 93 / 94 the boost and its tune, 95 the distortion's
 * intensity, 96 the input ring modulator (B 99), 97-99 osc 3's volume, semitone, detune, 102-107 the phaser's, 109
 * the delay's shape, 113 the dry level, 117-119 the EQ's mid band; "--" none in the reference (59-61, 82-87, 89, 100,
 * 101, 114-116, 120-127); an empty one repeats another (MX_DUP: 108 the delay's rate, 110 the colour, 111 its time,
 * 112 its feedback). From 72 on the knob shows the short name too (value_text) */
static const char MX_SHORT[] = "OFF\0VOL\0VOL 2\0PAN\0TRANSP\0PORTA\0OSC1 SHP\0OSC1 PW\0OSC1 WAV\0PITCH 1\0OSC1 KF\0"
    "OSC2 SHP\0OSC2 PW\0OSC2 WAV\0PITCH 2\0DETUNE\0FM\0FE>PITCH\0FE>FM\0OSC2 KF\0OSC BAL\0SUB\0OSC VOL\0NOISE\0"
    "CUT 1\0CUT 2\0RES 1\0RES 2\0FENV 1\0FENV 2\0KEYF 1\0KEYF 2\0F BAL\0FE ATT\0FE DEC\0FE SUS\0FE SLP\0FE REL\0"
    "AE ATT\0AE DEC\0AE SUS\0AE SLP\0AE REL\0L1 RATE\0L1 SYM\0L1>OSC1\0L1>OSC2\0L1>PW\0L1>RES\0L1>GAIN\0"
    "L2 RATE\0L2 SYM\0L2>SHP\0L2>FM\0L2>CUT1\0L2>CUT2\0L2>PAN\0L3 RATE\0L3 AMT\0--\0--\0--\0"
    "CHO MIX\0CHO RATE\0CHO DPT\0CHO DLY\0CHO FB\0FX SEND\0DLY TIME\0DLY FB\0DLY RATE\0DLY DPT\0"
    "V>SHP 1\0V>SHP 2\0V>PW\0V>FM\0V>FENV 1\0V>FENV 2\0V>RES 1\0V>RES 2\0V>AMP\0V>PAN\0"   /* 72-81 */
    "--\0--\0--\0--\0--\0--\0OSC PH\0--\0RING\0N COLOR\0FX COL\0BOOST\0BST TUNE\0DIST INT\0IN RING\0"   /* 82-96 */
    "O3 VOL\0O3 SEMI\0O3 DET\0--\0--\0PH MIX\0PH RATE\0PH DPT\0PH FREQ\0PH FB\0PH SPRD\0"   /* 97-107 */
    "\0DLY SHP\0\0\0\0DRY\0--\0--\0--\0MID GAIN\0MID FREQ\0MID Q\0--\0--\0--\0--\0--\0--\0--\0--";   /* 108-127 */
static const uint8_t MX_DUP[][2] = {{108, 70}, {110, 92}, {111, 68}, {112, 69}};   /* (the same byte as another) */
static UI_LATE void mx_short(uint32_t d, char *s)   /* destination code d's short name into s */
{
    const char *p = MX_SHORT;
    uint32_t k;
    for (k = 0; k < sizeof MX_DUP / sizeof MX_DUP[0]; k++)
        if (MX_DUP[k][0] == d) d = MX_DUP[k][1];
    if (d >= MXD_N) { put_sint(s, (int32_t)d, 0); return; }   /* (a byte past the list: its number) */
    while (d--) p += text_len(p) + 1u;
    text_cat(s, p);
}
static UI_LATE int mx_slot(uint32_t i, char *nm)   /* matrix row i (0..8): in use (1) or not; nm: its destination */
{
    const u_row_t *R = &U_ROW[U_NROW - MX_N + i];
    const uint32_t src = shadow[R->p[0].byte], dst = shadow[R->p[1].byte], amt = shadow[R->p[2].byte];
    nm[0] = 0;
    if (!src || !dst || amt == 64u) return 0;
#ifdef HYB_SHIMMER
    if (dst >= SHX_VD) { text_cat(nm, dst - SHX_VD < SHX_NT ? SHX_TN[dst - SHX_VD] : "SHIMMER"); return 1; }
#endif
    mx_short(dst, nm);
    return 1;
}
static UI_LATE uint32_t mx_sig(void)     /* the slots' lights and names as they are drawn: a change redraws the list */
{
    uint32_t i, h = 2166136261u;
    for (i = 0; i < MX_N; i++) {
        const u_row_t *R = &U_ROW[U_NROW - MX_N + i];
        const uint32_t src = shadow[R->p[0].byte], dst = shadow[R->p[1].byte], amt = shadow[R->p[2].byte];
        h = (h ^ (src && dst && amt != 64u ? dst + 1u : 0u)) * 16777619u;
    }
    return h;
}

/* below a matrix assign: its source and destination in full, the source's sharing */
static uint8_t mx_row;
static void mx_body(int32_t h)
{
    const u_row_t *R = &U_ROW[mx_row];
    uint32_t a = mx_row - (U_NROW - MX_N);
    char s[24];
    u_frame(0, 0, 232, h, U_BORDER, U_ROWOFF);
    if (MX_SHARED[a]) cv_text(8, 3, &FONT_B, MX_SHARED[a], u_acc);
    else cv_text(8, 3, &FONT_B, "SOURCE OWN", U_STATUS);
    value_text(&R->p[0], shadow[R->p[0].byte], s);
    cv_text(8, 22, &FONT_M, s, U_TEXT);
    s[0] = '>'; s[1] = ' ';
    value_text(&R->p[1], shadow[R->p[1].byte], s + 2);
    cv_text(8, 47, &FONT_M, s, U_TEXT);
}

/* the quick-access list's EDIT entry (REC opens the list of its slots) */
static void edit_entry_body(int32_t h)
{
    u_frame(0, 0, 232, h, U_BORDER, U_ROWOFF);
    u_text_c(30, &FONT_L, "EDIT", U_TEXT);
    u_text_c(74, &FONT_B, "QUICK ACCESS LIST", U_STATUS);
    u_text_c(104, &FONT_B, "REC  OPEN", u_acc);
}
static struct { uint8_t cur, choosing; int8_t ch; } ul;

/* SAVE's name editor (app.c sv_*): the slot over it, the ten letters large in their boxes (the cursor's lit), the
 * controls below; the question when REC overwrites a filled slot, else what is in the slot */
static struct { char tag[6], nm[10], old[24]; uint8_t cur, filled, ask; } nv;
#define NV_W 23                          /* a letter's box (10 x 23 = 230 of 232) */
static UI_LATE void nm_body(int32_t h)
{
    int32_t k;
    (void)h;
    k = cv_text(4, 4, &FONT_B, "SAVE TO ", U_STATUS);
    cv_text(k, 4, &FONT_B, nv.tag, u_acc);
    cv_text(232 - 4 - text_w(&FONT_B, "NAME"), 4, &FONT_B, "NAME", U_STATUS);
    for (k = 0; k < 10; k++) {
        const int on = k == nv.cur;
        const int32_t x = 1 + k * NV_W, y = 26;
        char c[2] = {nv.nm[k], 0};
        u_frame(x, y, NV_W - 1, 46, on ? U_CUR : U_BORDER, on ? U_CUR : U_ROWOFF);
        if (c[0] != ' ') cv_text(x + (NV_W - 1 - text_w(&FONT_L, c)) / 2, y + 7, &FONT_L, c, on ? U_ONCUR : U_TEXT);
        else cv_rect(x + 7, y + 36, NV_W - 15, 2, on ? U_ONCUR : U_TRACK);   /* (a space: a mark) */
    }
    k = cv_text(4, 80, &FONT_B, "SELECT ", u_acc);
    k = cv_text(k, 80, &FONT_B, "LETTER   ", U_STATUS);
    k = cv_text(k, 80, &FONT_B, "K1 ", u_acc);
    k = cv_text(k, 80, &FONT_B, "MOVE   ", U_STATUS);
    k = cv_text(k, 80, &FONT_B, "K2 ", u_acc);
    cv_text(k, 80, &FONT_B, "CASE", U_STATUS);
    k = cv_text(4, 100, &FONT_B, "REC ", u_acc);
    k = cv_text(k, 100, &FONT_B, "SAVE   ", U_STATUS);
    k = cv_text(k, 100, &FONT_B, "HOME ", u_acc);
    cv_text(k, 100, &FONT_B, "BACK", U_STATUS);
    if (nv.ask) {
        u_frame(0, 124, 232, 44, u_acc, mix565(U_BG, u_acc, 40u));
        u_text_c(134, &FONT_M, "REC again: overwrite", U_TEXT);
    } else if (nv.filled) {
        u_frame(0, 124, 232, 44, U_BORDER, U_ROWOFF);
        cv_text(8, 128, &FONT_B, "IN THE SLOT NOW", U_STATUS);
        cv_text(8, 146, &FONT_B, nv.old, U_DIMTXT);
    }
}

/* ------------------------------------------------------------ PLAY mode --- */
/* PLAY mode's screen (app.c play_*): its seven controls, K1..K4 on top as a row of knobs (the parameter's name, or its
 * short label where the name is too wide; the value), SELECT, PRESET and ALGO below (the control, the parameter's
 * name, a small knob and the value; SMART where the control takes the smart choice). The control turned last is
 * framed. Drawn whole, or one control alone (a turn: its box, nothing else) */
#define PC_N 7u                          /* the controls: K1..K4, SELECT, PRESET, ALGO */
#define PV_GAP 3                         /* the rows between the two rows of boxes */
static const char *const PC_NAME[PC_N] = {"K1", "K2", "K3", "K4", "SELECT", "PRESET", "ALGO"};
static struct {
    const u_param_t *p[PC_N];            /* each control's parameter now (app.c pv_fill) */
    uint8_t smart[PC_N];                 /* it takes the smart choice */
    int8_t hl;                           /* the control framed (turned last), -1 none */
    int16_t ox;                          /* the canvas's left in the body (one box alone: its left) */
} pv = {{0}, {0}, -1, 0};
static UI_LATE void pv_box(uint32_t c, int32_t *x, int32_t *y, int32_t *w)   /* control c's box in the body (its height UI_ROW_H) */
{
    if (c < 4u) { *x = (int32_t)c * 58; *y = 0; *w = 58; }
    else { *x = (int32_t)(c - 4u) * 77; *y = UI_ROW_H + PV_GAP; *w = c == 6u ? 78 : 77; }
}
static UI_LATE void pv_cut(char *s, const felucca_font_t *f, int32_t w)   /* s cut to w pixels */
{
    uint32_t n = text_len(s);
    while (n && text_w(f, s) > w) s[--n] = 0;
}
static UI_LATE const felucca_font_t *pv_font(const char *s, int32_t w, int big)   /* the biggest font s fits w in */
{
    if (big && text_w(&FONT_L, s) <= w) return &FONT_L;
    if (text_w(&FONT_M, s) <= w) return &FONT_M;
    return text_w(&FONT_B, s) <= w ? &FONT_B : &FONT_XS;
}
static UI_LATE void pv_name(const u_param_t *p, char *s, int32_t w)   /* its name, or its label when the name is too wide */
{
    s[0] = 0;
    p_name(p, s);
    if (text_w(&FONT_XS, s) > w) { s[0] = 0; text_cat(s, p_label(p)); }
    pv_cut(s, &FONT_XS, w);
}
static UI_LATE void pv_cell(uint32_t c)
{
    const u_param_t *p = pv.p[c];
    const int32_t H = UI_ROW_H;
    const uint16_t lt = U_VALON;
    int32_t x, y, w, cx, d;
    uint32_t v, mx;
    char s[24];
    const felucca_font_t *f;
    pv_box(c, &x, &y, &w);
    x -= pv.ox;
    w -= 2;                              /* (the gap to the next box) */
    u_frame(x, y, w, H, (int32_t)c == pv.hl ? U_LCDTXT : U_BORDER, mix565(U_BG, u_acc, 28u));
    if ((int32_t)c == pv.hl) u_frame(x + 1, y + 1, w - 2, H - 2, U_LCDTXT, mix565(U_BG, u_acc, 28u));
    if (!p) return;
    v = shadow[p_byte(p)];
    mx = p_max(p);
    if (v > mx) v = mx;
    if (c < 4u) {                        /* K1..K4: as a row's knob */
        cx = x + w / 2;
        if (pv.smart[c]) cv_text(x + 4, y + 2, &FONT_XS, "S", u_acc);
        knob(cx - UI_KNOB / 2, y + 3, (float)v / (float)mx, p->kind == V_BI || p->kind == V_SEMI, 1, UI_KNOB);
        pv_name(p, s, w - 4);
        cv_text(cx - text_w(&FONT_XS, s) / 2, y + UI_KNOB + 2, &FONT_XS, s, RGB(242, 242, 243));
        value_text(p, v, s);
        if (p->kind == V_SHAPE && text_w(&FONT_XS, s) > w - 4) put_sint(s, (int32_t)v, 0);   /* (too long: the number) */
        f = pv_font(s, w - 4, 1);
        pv_cut(s, f, w - 4);
        cv_text(cx - text_w(f, s) / 2, y + H - 2 - f->h + (f == &FONT_L ? 2 : f == &FONT_M ? -2 : -6), f, s, lt);
        return;
    }
    cv_text(x + 5, y + 2, &FONT_XS, PC_NAME[c], U_DIMTXT);   /* SELECT, PRESET, ALGO */
    if (pv.smart[c]) cv_text(x + w - 5 - text_w(&FONT_XS, "S"), y + 2, &FONT_XS, "S", u_acc);   /* (SMART: an S, as
                                                     * on top; the word does not fit beside SELECT) */
    pv_name(p, s, w - 10);
    cv_text(x + 5, y + 18, &FONT_XS, s, RGB(242, 242, 243));
    d = 28;
    knob(x + 4, y + 42, (float)v / (float)mx, p->kind == V_BI || p->kind == V_SEMI, 1, d);
    value_text(p, v, s);
    if (p->kind == V_SHAPE && text_w(&FONT_XS, s) > w - d - 10) put_sint(s, (int32_t)v, 0);
    f = pv_font(s, w - d - 10, 0);
    pv_cut(s, f, w - d - 10);
    cx = x + d + 6 + (w - d - 10) / 2;
    cv_text(cx - text_w(f, s) / 2, y + 42 + (d - f->h) / 2 + (f == &FONT_M ? 1 : 0), f, s, lt);
}
static UI_LATE void pv_body(int32_t h)           /* the whole screen */
{
    uint32_t c;
    (void)h;
    for (c = 0; c < PC_N; c++)
        pv_cell(c);
}
static UI_LATE void draw_play(void) { pv.ox = 0; draw_body(UI_ROW0_Y, UI_BODY_H, pv_body); }
#define PV_BATT_Y (UI_ROW0_Y + UI_BODY_H)   /* PLAY mode's battery: a thin bar in the 3 rows under the controls */
static UI_LATE void pv_batt(int b)       /* b: plat_battery's (HOME's bar, thin: grey, filled red by the charge, on USB
                                          * full and grey); -9: away (PLAY mode left) */
{
    lcd_fill(4, PV_BATT_Y, 232, 240 - PV_BATT_Y, U_BG);
    if (b == -9) return;
    lcd_fill(4, PV_BATT_Y + 1, 232, 2, b > 3 ? RGB(120, 120, 126) : U_TRACK);
    if (b > 0 && b <= 3) lcd_fill(4, PV_BATT_Y + 1, (uint32_t)(232 * b / 3), 2, U_TH.batt);
}

/* ------------------------------------------------------ the sequencer --- */
/* the SEQ page (app.c sq_*): at the top the progress row (a field per block of 16 steps the
 * pattern's LENGTH has, each of four quarters, filled as the step playing moves on: the quarter playing bright, the
 * blocks played dim; the block the white keys show framed; pg_*), below it two blocks of knobs: the step held (or the
 * last one held), EVENT, LENGTH, ALIGN, VEL (K1..K4) and OFFSET (SELECT), lit while a step key is held; the sequencer,
 * LENGTH, TIME DIV, PATTERN, CLEAR (K1..K4) and BASE NOTE (SELECT), lit otherwise. The steps are not drawn (the white
 * keys' lights show them, and the step playing). Each part is drawn again only when what it shows changed: a progress
 * field (its quarter, not each step), a knob's column. 0.94: the blocks without the tags up their left side (17
 * pixels; their columns over the whole width, larger knobs); the step held in the strip ("STEP 12", sq_lcd). 0.94's
 * info box right of the progress row (the tempo, STEP n: too small to read) is gone in 0.96, the progress fields over
 * the whole width again (the tempo is in the status line) */
#define SQ_PG 4u                         /* the progress row's fields: the parts before the blocks' */
#define SQ_PG_H 26                       /* the progress row's height (the drums' page: DR_PG_H) */
#define SQ_PG_W 232                      /* the progress fields' width (0.94-0.95: 189, an info box right of them) */
#define SQ_B1_Y 29                       /* the blocks' tops, their height, the knobs' size */
#define SQ_B2_Y 101
#define SQ_B_H 70
#define SQ_COL_X(k) ((int32_t)(k) * 232 / 5)   /* a block's column k of 5 (0.94: over the whole width, 46-47 wide) */
#define SQ_NPART (SQ_PG + 12u)           /* the progress fields, a part unused (block 1's tag up to 0.93, the info box
                                          * in 0.94-0.95) and block 1's 5 columns, a part unused (block 2's tag up to
                                          * 0.93) and block 2's 5 columns */
#define DR_NPART (SQ_PG + DR_NI + 11u)   /* the drums' page: the progress fields, the instruments, a part unused (as
                                          * the arp sequencer's) and block 1's 4 columns, a part unused and block 2's
                                          * 5 columns */
static const uint8_t SQ_KEY[SQ_N] = PLAT_WHITE_KEYS;   /* step i's key (of the block shown) */
static const uint8_t DR_KEY[DR_NI] = PLAT_BLACK_KEYS;  /* instrument i's key */
static const char *const DR_NAME[DR_NI] = {"BD", "SD", "LT", "MT", "HT", "RS", "CP", "CH", "OH", "CY", "CB"};
static struct {
    uint8_t key;                         /* the step key held + 1 (its white key's index), 0 none */
    uint8_t sel;                         /* the step shown (0 .. 63): the one held, or the last held */
    uint8_t edited;                      /* the step held was changed (its release toggles nothing) */
    uint8_t clr;                         /* CLEAR turned: REC clears (until clr_t + SQ_CLR_MS) */
    uint8_t ins;                         /* the drums' page: the instrument shown */
    uint8_t rec, rec_run;                /* the drums' page: RECORD on (REC); the drums ran when last looked at */
    uint8_t arec, arec_run, arec_mi;     /* the arp sequencer's page: RECORD on (sq_arm); it ran when last looked at;
                                          * the one it records (0 A, 1 B) */
    uint8_t menu, menu_v;                /* the EDIT list: its line + 1 (0: closed), the page it is up on (se_*) */
    uint8_t cp, cp_ask;                  /* EDIT > COPY TO's chooser: its line + 1 (0: closed); OVERWRITE asked (cp_*) */
    uint8_t sp_ask;                      /* SAVE on a sequencer's page: "SAVE PATTERN 3?" asked (until sp_t + SQ_CLR_MS) */
    uint8_t sp_wait;                     /* (after 1.00) SAVE asked while the beat ran: done at its stop (0 none,
                                          * 0xFF all the patterns with edits, else a pattern's key + 1; sp_go) */
    uint8_t sv_w;                        /* (after 1.00) the kit's (1), GLO's settings' (2), the patch's (4, 1.02) SAVE asked while the beat
                                          * ran: done at its stop (sp_idle) */
    uint8_t blk[2];                      /* the block of 16 steps the white keys show: the arp sequencer's, the drums' */
    uint8_t tt;                          /* the parameter turned last (a column, SQ_TT_TEMPO; 0 none): in the strip */
    uint8_t what;                        /* the part being drawn */
    uint8_t last;                        /* SEQ's last page shown (0: none yet, its way starts; 1 the arp
                                          * sequencer's, sq_mi's; 2 the drums'): SEQ off the pages opens it (0.77) */
    int8_t dz;                           /* ALIGN, OFFSET: the detents turned at 0 (sq_dz), of column dzw */
    uint8_t dzw;
    int16_t ox, oy;                      /* (1.03) ox: the width of the part drawn alone (seq_draw, seq_part_body);
                                          * oy unused (up to 1.02 the canvas's place in the body, always 0) */
    uint32_t lcd;                        /* the strip as shown (sq_frame, dr_frame) */
    uint32_t down_t, clr_t;              /* the step key went down; CLEAR turned (the edits: app.c's journal) */
    uint32_t tt_t;                       /* the parameter turned then */
    uint32_t sp_t;                       /* SAVE asked then */
    uint32_t sp_b;                       /* the buttons down since SAVE asked (a new one takes it back: sp_idle) */
    uint32_t cp_used;                    /* COPY TO: the patterns saved once (bit n: pattern n; cp_open) */
    uint16_t svpw[2];                    /* (1.02, the patch's SAVE) REC in the name editor while the beat runs (sv_w
                                          * bit 2): the slot written at its stop (app.c sp_idle, again through
                                          * sv_write), the patch edited then */
    uint8_t chn, cht[SQ_CHV];            /* (1.03) CHORD ENTRY: the notes chosen, as the chords hold them (offsets
                                          * from the base + 64, lowest first, 0 past the last; app.c sq_ch_key) */
    uint8_t ce;                          /* (1.03) CHORD ENTRY on for the step shown (sqv.key kept: SQ_CE): the
                                          * keyboard's octave before it + 3 (OCT- / OCT+ move it meanwhile); 0 off */
    uint8_t lin;                         /* (1.18C31) the step key let go after a change: its view (STEP n, the
                                          * roll, its columns) kept until lin_t + SQ_TT_MS (app.c sq_idle), CHORD
                                          * ENTRY on: kept until REC / HOME; 0: held (or none) */
    uint8_t aud;                         /* (1.18C44) the step held's audition (app.c sq_aud_idle): 0 not yet,
                                          * SQ_AUD_TRY tried (nothing heard), SQ_AUD_ON heard (its release toggles
                                          * nothing); in lin's padding */
    uint16_t lin_t, cek;                 /* (1.18C31) let go then; EVENT's last turn that opened CHORD ENTRY or was
                                          * swallowed after it (| 1; 0 none: SQ_CE_REST, app.c sq_knob) */
#ifdef HYB_MULTI
    uint8_t blk_o;                       /* the other arp sequencer's block shown (sq_show swaps it with blk[0]) */
    uint8_t first;                       /* the arp sequencer SEQ showed first (the part edited's): SEQ again the other */
#endif
} sqv PLAT_POOL;
#define SQ_CE (sqv.ce && sqv.key)        /* (1.03) CHORD ENTRY on (the step key's place kept as if held; a page or
                                          * step change that clears sqv.key ends it: app.c sq_idle) */
#define SQ_CEE 0x80u                     /* (1.07) sqv.ce's top bit: every note toggled out (app.c sq_ch_key), the
                                          * choice empty (no notes shown, the step silent, REC a rest), not "none
                                          * chosen yet" (the step as stored); the octave in its low bits */
#define SQ_CET 0x40u                     /* (1.18C31) sqv.ce's bit 6: a note toggled in this CHORD ENTRY (app.c
                                          * sq_ch_key); before it EVENT turned leaves it with that event, the step's
                                          * own chord chosen or not (sq_knob) */
#define SQ_CECH (sqv.chn || (sqv.ce & SQ_CEE))   /* (1.07) CHORD ENTRY's choice stands (notes chosen or all out) */
#define SQ_HELD (sqv.key && (!sqv.lin || SQ_CE))   /* (1.18C31) a step key held (or CHORD ENTRY's place kept),
                                          * not a step's view kept a while after it was let go (sqv.lin) */
static UI_LATE void pv_one(int32_t h)    /* (a band of) the box drawn alone (sqv.what: the part being drawn, as on the
                                          * sequencers' pages): its rows from its top (part_bands) */
{
    int32_t x, y, w;
    (void)h;
    pv_box(sqv.what, &x, &y, &w);
    cv_oy -= y;
    pv_cell(sqv.what);
}
static UI_LATE void draw_play_cell(uint32_t c)   /* one control's box alone, in a canvas of its width */
{
    int32_t x, y, w;
    pv_box(c, &x, &y, &w);
    pv.ox = (int16_t)x;
    sqv.what = (uint8_t)c;
    part_bands(x, y, w, UI_ROW_H, pv_one);
    pv.ox = 0;
}
#define PL_T uint8_t                     /* (PLAY mode's list: a row x 4 + knob, PL_ENC) */
/* (1.18C48) EDIT > OUTPUT (FX's last EDIT row) holds LEVEL's DRY, VOLUME and PAN again: it never adds to the list
 * (each byte once, LEVEL's block first), so it is skipped and not counted: 1.18C47's second EDIT > SHIMMER row made
 * 65 counted rows, MATRIX SLOT 6's entries 256 .. 258, which 8 bits wrapped onto the first row's (SLOT 6 could not be
 * assigned; host test play_mx6.txt) */
#define ROW_OUT (U_NBLK + 11u)           /* (FX's twelve EDIT rows: DLY/REV x3, PHASER x2, CHORUS x2, DIST, EQ x2, INPUT, OUTPUT) */
#ifdef HYB_SHIMMER
/* (1.04) the rows counted without the shimmer's (FX > SHIMMER, EDIT > SHIMMER's two (1.18C47): the extension's,
 * never in the list), so 8 bits hold all 68 rows' entries (0.89-1.03 took 16 bits: 260 B more of the pool) */
#define ROW_SHTL 37u                     /* EDIT > SHIMMER's rows: U_NBLK and FX's twelve EDIT rows before them */
#define PL_ENC(r, k) ((PL_T)(((r) - ((r) > ROW_SHIM) - ((r) > ROW_OUT) - ((r) > ROW_SHTL) - ((r) > ROW_SHTL + 1u)) * 4u + (k)))
#define PL_NX 4u                         /* (the rows never in it) */
static uint32_t pl_row(uint32_t e)       /* an entry's row */
{
    uint32_t r = e >> 2;
    if (r >= ROW_SHIM) r++;
    if (r >= ROW_OUT) r++;
    if (r >= ROW_SHTL) r++;
    if (r >= ROW_SHTL + 1u) r++;
    return r;
}
#else
#define PL_ENC(r, k) ((PL_T)(((r) - ((r) > ROW_OUT)) * 4u + (k)))
#define PL_NX 1u
#define pl_row(e) (((uint32_t)(e) >> 2) + (((uint32_t)(e) >> 2) >= ROW_OUT))
#endif
_Static_assert((U_NROW - 1u - PL_NX) * 4u + 3u <= 255u, "PLAY mode's list: every row's entry in 8 bits");
static union {                           /* (RAM is short: two buffers never in use at once share it) */
    uint32_t part[DR_NPART + 2u];        /* the sequencers' pages: the parts as drawn (sq_sig, dr_sig: one page at a
                                          * time), 0 not drawn; past them the roll's signature and time (rl_draw) */
    PL_T pl[(U_NROW - 1u - PL_NX) * 4u];   /* PLAY mode: its list of parameters (app.c pl_tab, built again when needed:
                                          * pl_n = 0 when a sequencer's page opens; each byte once: FX's LEVEL, 0.84,
                                          * adds none, its four bytes are DLY/REV's and OUTPUT's) */
} sq_ush PLAT_POOL;                     /* (0.86: in the pool, the RAM's .bss full; 256 B with the matrix's
                                          * nine rows) */
#define SQ_PART sq_ush.part
static const char *const SQ_DIVT[SQ_NDIV] = {"0.25x", "0.5x", "1x", "2x", "4x"};
static const char *const SQ_LB[12] = {"", "EVENT", "LENGTH", "ALIGN", "VEL", "OFFSET", "", "LENGTH", "DIV",
                                      "PATTERN", "CLEAR", "BASE"};   /* the columns' labels (43 pixels a column:
                                          * DIV for TIME DIV, BASE for BASE NOTE); the strip's title too */
static const char *const DR_LB[11] = {"", "ACCENT", "LENGTH", "ALIGN", "STEP", "", "LENGTH", "DIV", "PATTERN",
                                      "CLEAR", "LEVEL"};   /* (the full names: the title while turning) */
static const char *const SQ_SH[12] = {"", "EVT", "LEN", "POS", "VEL", "OFS", "", "LEN", "DIV", "PTN", "CLR", "KEY"};
static const char *const DR_SH[11] = {"", "ACC", "LEN", "POS", "STP", "", "LEN", "DIV", "PTN", "CLR", "LVL"};
                                          /* the columns' labels, 3 letters (1.18C24: the full ones above are the
                                           * title's; LEVEL has no column any more) */

/* the progress row: field w of a pattern of LENGTH lc (16 << lc steps, 1 << lc fields side by side), the block b
 * the keys show, the step playing st (run: 1 while it runs); its signature (2 | ...; 1: no field there) changes only
 * when its quarter does */
static UI_LATE uint32_t pg_sig(uint32_t w, uint32_t lc, uint32_t b, uint32_t run, uint32_t st)
{
    const uint32_t n = 1u << lc, pb = st >> 4;
    uint32_t f = 0;
    if (w >= n) return 1u;
    if (run) f = w < pb ? 5u : w == pb ? 1u + (st >> 2 & 3u) : 0u;   /* (5: played; 1..4: the quarter playing + 1) */
    return 2u | lc << 2 | (w == b ? 16u : 0u) | f << 5;
}
static UI_LATE void pg_place(uint32_t w, uint32_t lc, int32_t *x, int32_t *wd)   /* field w's left and width (with
                                          * the gap after it) */
{
    const uint32_t n = 1u << lc;
    *x = (int32_t)(w * (uint32_t)SQ_PG_W / n);
    *wd = (int32_t)((w + 1u) * (uint32_t)SQ_PG_W / n) - *x;
}
static UI_LATE void pg_field(uint32_t w, uint32_t g, int32_t h)   /* field w (signature g) in a canvas of its place */
{
    const uint32_t lc = g >> 2 & 3u, n = 1u << lc, f = g >> 5 & 7u, shown = (g & 16u) != 0;
    const felucca_font_t *fn = h >= 24 ? &FONT_XS : &FONT_T;
    const int32_t sh = h >= 24 ? 7 : 4, sy = h - 3 - sh;
    int32_t x, wd, bw, j, sx, sw;
    char s[8];
    pg_place(w, lc, &x, &wd);
    bw = w + 1u < n ? wd - 3 : wd;       /* (the box; a gap of 3 to the next) */
    u_frame(0, 0, bw, h, shown ? u_acc : U_BORDER, U_ROWOFF);
    if (shown) u_frame(1, 1, bw - 2, h - 2, u_acc, U_ROWOFF);
    put_sint(s, (int32_t)(w * SQ_N + 1u), 0);
    text_cat(s, "-");
    put_sint(s + text_len(s), (int32_t)((w + 1u) * SQ_N), 0);
    cv_text((bw - text_w(fn, s)) / 2, h >= 24 ? 1 : 2, fn, s, shown ? U_TEXT : U_DIMTXT);
    sw = (bw - 8 - 3 * 2) / 4;           /* (four quarters, 2 apart, 4 in from the sides) */
    sx = (bw - (4 * sw + 3 * 2)) / 2;
    for (j = 0; j < 4; j++) {
        const uint16_t c = f == 5u ? mix565(U_BG, u_acc, 96u)   /* (a block played: dim) */
                         : f && (uint32_t)j + 1u < f ? u_acc     /* (its quarters played) */
                         : f && (uint32_t)j + 1u == f ? mix565(u_acc, C_WHITE, 150u)   /* (the one playing) */
                         : U_TRACK;
        cv_rect(sx + j * (sw + 2), sy, sw, sh, c);
    }
}
static UI_LATE void pg_body(int32_t h) { pg_field(sqv.what, SQ_PART[sqv.what], h); }
static UI_LATE void pg_part(uint32_t w, int32_t h)   /* field w alone (its signature in SQ_PART), in bands */
{
    int32_t x, wd;
    pg_place(w, SQ_PART[w] >> 2 & 3u, &x, &wd);
    sqv.what = (uint8_t)w;
    part_bands(x, 0, wd, h, pg_body);
}

static UI_LATE void sq_ev_text(uint32_t ev, char *s)   /* CHORD, NOTE, FING n, F POLY (1.02: FINGER POLY, the
                                          * column's room; POLY up to 1.01) */
{
    s[0] = 0;
    if (ev == SQ_NOTE) text_cat(s, "NOTE");
    else if (ev == SQ_CHORD) text_cat(s, "CHORD");
    else if (ev == SQ_POLY) text_cat(s, "F POLY");
    else {
        text_cat(s, "FING ");
        put_sint(s + 5, (int32_t)(ev - SQ_FING + 1u), 0);
    }
}
static UI_LATE void note_text(uint32_t v, char *s)   /* a note's name after s (C4 = 60) */
{
    static const char N[12][3] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const uint32_t k = text_len(s);
    text_cat(s, N[v % 12u]);
    put_sint(s + k + (N[v % 12u][1] ? 2 : 1), (int32_t)(v / 12u) - 1, 0);
}
static UI_LATE void put_align(char *s, int32_t v)   /* an ALIGN: 0, +n/8, -n/8 */
{
    if (v) { put_sint(s, v, 1); text_cat(s, "/8"); } else text_cat(s, "0");
}
static UI_LATE uint32_t sq_live(void)   /* KEY LATCH (app.c sq_latch): the notes the arp sequencer shown has latched,
                                          * running, no input note held: the lowest + 1 (| 0x100: a chord); 0 none */
{
    uint32_t w, n = 0;
    if (!sq.run || (sqp.pad & SQ_NLAT) || sq.ldn || sq.lclr) return 0;
    for (w = 0; w < 4u; w++)
        if (sq.keys[w] | sq.midi[w]) return 0;
    for (w = 0; w < 128u; w++)
        if (sq.lat[w >> 5] >> (w & 31u) & 1u) {
            if (n) return n | 0x100u;
            n = w + 1u;
        }
    return n;
}
static UI_LATE uint32_t sq_cv(uint32_t w, char *s, uint32_t *mx, uint32_t *bi)   /* column w's value (its knob's
                                          * position of *mx, *bi bipolar) and its text (s, empty first) */
{
    const sq_step_t *t = &sqp.st[sqv.sel];
    uint32_t v = 0;
    *mx = 1;
    *bi = 0;
    s[0] = 0;
    if (w == 1u) { v = SQ_EVU(t->ev & 15u); *mx = SQ_NEV - 1u; sq_ev_text(t->ev & 15u, s); }   /* (the list's place,
                                          * 1.02) */
    else if (w == 2u) {
        const uint32_t n = SQ_LEN(t->la & 31u) / 8u;   /* (in steps: "40/16"; whole bars past one "2 BARS") */
        v = t->la & 31u; *mx = SQ_LMAXP(sqp.lc) > v ? SQ_LMAXP(sqp.lc) : v;
        if (v < 3u) { text_cat(s, "1/"); put_sint(s + 2, (int32_t)(64u >> v), 0); }
        else if (n > SQ_N && !(n % SQ_N)) { put_sint(s, (int32_t)(n / SQ_N), 0); text_cat(s, " BARS"); }
        else { put_sint(s, (int32_t)n, 0); text_cat(s, "/16"); }
    } else if (w == 3u) { v = (uint32_t)(SQ_AL(t) + 7); *mx = 14u; *bi = 1; put_align(s, SQ_AL(t)); }
    else if (w == 4u) { v = t->vel; *mx = 127u; put_sint(s, (int32_t)v, 0); }
    else if (w == 5u) {                  /* (a CHORD step's off its chord: "--", 1.02; 1.18C31: OFFSET NOTE's alone,
                                          * FING 1-8 and FING POLY "--" too, the bar at 0) */
        v = (uint32_t)(t->off + 24); *mx = 48u; *bi = 1;
        if (!SQ_OFS(t->ev & 15u)) { v = 24u; text_cat(s, "--"); } else put_sint(s, t->off, 1);
    }
    else if (w == 7u) { v = sqp.lc; *mx = SQ_NLC - 1u; put_sint(s, (int32_t)SQ_STEPS(v), 0); }
    else if (w == 8u) { v = sqp.div; *mx = SQ_NDIV - 1u; *bi = 1; text_cat(s, SQ_DIVT[v]); }
    else if (w == 9u) {                  /* (NONE: no pattern, PLAY leaves the sequencer out) */
        v = (uint32_t)(SQ_QPAT(UN_SQI, SQ_PATSH) + 1); *mx = SQ_PATS;   /* (0.96: the one waiting for the line) */
        if (v) put_sint(s, (int32_t)v, 0); else text_cat(s, "NONE");
    } else if (w == 10u) text_cat(s, sqv.clr ? "REC?" : "--");
    else {                               /* (the base note: OFF, C1 .. C7; KEY LATCH: the note latched, "E3*", a
                                          * chord its lowest, "E3+", the knob at the setting) */
        const uint32_t l = sq_live();
        v = SQ_BASEV ? SQ_BASEV - SQ_BASE_LO + 1u : 0u; *mx = SQ_BASE_HI - SQ_BASE_LO + 1u;
        if (l) { note_text((l & 0xFFu) - 1u, s); text_cat(s, l >> 8 ? "+" : "*"); }
        else if (SQ_BASEV) note_text(SQ_BASEV, s); else text_cat(s, "OFF");
    }
    return v;
}
static UI_LATE uint32_t sq_sig(uint32_t w)       /* what part w shows (never 0) */
{
    const uint32_t k = sqv.key ? 1u : 0u;
    if (w < SQ_PG) return pg_sig(w, sqp.lc, sqv.blk[0], sq.run, sq.step);
    w -= SQ_PG;
    if (w == 0u) return 1u;              /* (unused: nothing drawn; the info box in 0.94-0.95) */
    if (w <= 5u) {
        const sq_step_t *t = &sqp.st[sqv.sel];
        const uint32_t v = w == 1u ? t->ev & 15u : w == 2u ? t->la & 31u : w == 3u ? (uint32_t)(SQ_AL(t) + 7)
                         : w == 4u ? t->vel : (uint32_t)(t->off + 24) | (t->ev & 15u) << 6;
        return 1u | k << 1 | (uint32_t)sqv.sel << 2 | v << 8;
    }
    if (w == 6u) return 1u;              /* (unused: nothing drawn) */
    return 1u | k << 1 | (w == 7u ? sqp.lc : w == 8u ? sqp.div : w == 9u ? (SQ_PADV & SQ_DISARM ? 255u : SQ_PATV)
                                                                              | sq_qt[UN_SQI] << 8
                          : w == 10u ? sqv.clr : SQ_BASEV | sq_live() << 8) << 2;
}
static UI_LATE void sq_colbox(int32_t wd, int32_t H, int on, int ed, uint32_t v, uint32_t mx, int bi,
                              const char *lb, const char *s, int hot)   /* a column of a sequencer's block, its place in
                                          * the canvas: a thin strip at its top (the value's fraction, from the centre
                                          * for a bipolar one; hot, CLEAR asking or a pattern waiting: the strip and
                                          * the value in the accent, else the strip dim), its 3-letter label and its
                                          * value large; ed bit 0 the block's first column (its left side), bit 1 its
                                          * last (its right). The label in FONT_M, the value in the largest of FONT_L
                                          * (tall blocks), FONT_M, FONT_B, FONT_S, FONT_XS it fits in */
{
    const int32_t x = 0, y = 0, sx = 5 + (ed & 1), sw = wd - 10 - (ed & 1) - (ed >> 1 & 1), cx = sx + sw / 2;
    const uint16_t bd = on ? u_acc : U_BORDER, ac = on || hot ? u_acc : RGB(120, 120, 126);
    const felucca_font_t *lf = text_w(&FONT_M, lb) <= wd - 4 ? &FONT_M : &FONT_B;
    const felucca_font_t *f = H >= SQ_B_H && text_w(&FONT_L, s) <= wd - 4 ? &FONT_L
                            : text_w(&FONT_M, s) <= wd - 4 ? &FONT_M : text_w(&FONT_B, s) <= wd - 4 ? &FONT_B
                            : text_w(&FONT_S, s) <= wd - 4 ? &FONT_S : &FONT_XS;
    int32_t a = (int32_t)(v * (uint32_t)sw / mx), b = 0;   /* (the fill: from the left, or from the centre) */
    cv_rect(x, y, wd, H, on ? mix565(U_BG, u_acc, 28u) : U_ROWOFF);
    cv_rect(x, y, wd, 1, bd);
    cv_rect(x, y + H - 1, wd, 1, bd);
    if (ed & 1) cv_rect(x, y, 1, H, bd);
    if (ed & 2) cv_rect(x + wd - 1, y, 1, H, bd);
    cv_rect(sx, y + 5, sw, 3, U_TRACK);
    if (bi) { b = sw / 2; if (a < b) { int32_t t = a; a = b; b = t; } }
    else if (a > sw) a = sw;
    if (a > b) cv_rect(sx + b, y + 5, a - b, 3, ac);
    if (bi) cv_rect(sx + sw / 2, y + 3, 1, 7, on ? C_WHITE : RGB(142, 142, 148));   /* (the centre's tick) */
    cv_text(cx - text_w(lf, lb) / 2, y + 9, lf, lb, on ? RGB(242, 242, 243) : RGB(150, 150, 156));
    cv_text(cx - text_w(f, s) / 2, y + H - 3 - f->h + (f == &FONT_L ? 4 : f == &FONT_M ? 2 : 0), f, s,
            hot ? u_acc : on ? U_VALON : RGB(176, 176, 182));
}
/* the piano roll (0.78): a step held and turned (sqv.edited), the whole body shows 8 steps
 * around it (3 before, 4 after, wrapping at the pattern's LENGTH; earlier steps long enough reach in), a bar per step
 * on: x its place + ALIGN, its width LENGTH (cut at the right with an arrow), y its pitch (the base note, KEY LATCH's
 * note or C4 with BASE NOTE OFF, + OFFSET; POLY and FING at the base, marked "P", "F2"), 25 semitones (the view
 * centred on the other steps', following the step held when it leaves it), the octaves' C ruled; under them the
 * steps' numbers and their velocities; the playing position a thin line. The step held: the accent, a light rim
 * (off: hollow). Its signature and time in SQ_PART's room past the drums' parts (sq_ush). Drawn again whole when what
 * it shows changes, at most every RL_MS (more under load: gui_gap). The drums' page has one too (0.79, dl_*): the
 * grid, the bars, the lane and the drawing shared (rl_grid, rl_bar, rl_lane, rl_draw) */
#define RL_X0 23                         /* the steps' left, a step's width, the pitch rows' top and height */
#define RL_CW 26
#define RL_PY 2
#define RL_RH 5
#define RL_GH (RL_PY + 25 * RL_RH)       /* the grid's height (the steps' numbers under it) */
#define RL_MS 40u
#define RL_SIG SQ_PART[DR_NPART]
#define RL_T SQ_PART[DR_NPART + 1u]
static uint32_t fnv(const void *p, uint32_t n);
static uint32_t gui_gap(uint32_t base);
static UI_LATE uint32_t rl_play(uint32_t st, uint32_t run, uint32_t n)   /* the playing position's column (8: not in
                                          * the window; n a power of 2: the unsigned's wrap) */
{
    const uint32_t c = (st + 3u - sqv.sel) % n;
    return run && c < 8u ? c : 8u;
}
static UI_LATE void rl_grid(uint32_t n, uint32_t pl)   /* the steps' lines, their numbers, the playing position */
{
    int32_t i;
    char s[4];
    for (i = 0; i <= 8; i++) {
        cv_rect(RL_X0 + i * RL_CW, 0, 1, RL_GH, U_RULE);
        if (i == 8) break;
        s[0] = 0;
        put_sint(s, (int32_t)((sqv.sel + (uint32_t)i - 3u) % n) + 1, 0);
        cv_text(RL_X0 + i * RL_CW + (RL_CW - text_w(&FONT_XS, s)) / 2 + 1, 129, &FONT_XS, s, i == 3 ? U_TEXT : U_DIMTXT);
    }
    if (pl < 8u) cv_rect(RL_X0 + (int32_t)pl * RL_CW, 0, 1, RL_GH, U_TEXT);
}
static UI_LATE int32_t rl_bar(int32_t x, int32_t w, int32_t y, int32_t h, uint16_t c, int held)   /* a bar from x, w
                                          * wide (cut at the window: an arrow), h high; held: 1 the step held, 2 held
                                          * while off (hollow); its left as drawn (-1: none) */
{
    int32_t x1 = x + (w < 3 ? 3 : w);
    if (x1 <= RL_X0) return -1;
    if (x < RL_X0) x = RL_X0;
    if (x1 > RL_X0 + 8 * RL_CW) {        /* (past the window: an arrow; up to its edge none, 0.79) */
        x1 = 228;
        cv_rect(228, y - 1, 1, h + 2, c);
        cv_rect(229, y, 1, h, c);
        cv_rect(230, y + 1, 1, h - 2, c);
        cv_rect(231, y + 2, 1, h - 4, c);
    }
    if (!held) cv_rect(x, y, x1 - x, h, c);
    else u_frame(x, y, x1 - x, h, mix565(u_acc, C_WHITE, 170u), held == 1 ? c : U_BG);
    return x;
}
static UI_LATE void rl_lane(int32_t i, int32_t vh, uint16_t c)   /* column i's bar in the lane under the numbers */
{
    cv_rect(RL_X0 + i * RL_CW + 6, 170 - vh, RL_CW - 11, vh ? vh : 1, c);
}
static UI_LATE int32_t rl_base(void)     /* the base the steps play from */
{
    const uint32_t l = sq_live();
    return l ? (int32_t)(l & 0xFFu) - 1 : SQ_BASEV ? (int32_t)SQ_BASEV : 60;
}
static UI_LATE int32_t rl_pitch(const sq_step_t *t) { return rl_base() + (SQ_OFS(t->ev & 15u) ? t->off : 0); }   /* (FING
                                          * n: its OFFSET on the base, 1.18C31) */
static UI_LATE uint32_t rl_notes(const sq_step_t *t, int32_t *p)   /* (1.02) the pitches step t shows, lowest first
                                          * (p[0] its lowest): a CHORD step's each (in CHORD ENTRY the notes chosen,
                                          * sqv.cht, 1.03; all toggled out: none, 0, 1.07), the others' one */
{
    int8_t o[SQ_CHV];
    uint32_t i, n;
    if ((t->ev & 15u) != SQ_CHORD) { p[0] = rl_pitch(t); return 1u; }
    n = sq_chord(&sqp, t, o);
    if (t == &sqp.st[sqv.sel] && SQ_CECH)   /* (1.07: all toggled out, none: 0) */
        for (n = 0; n < sqv.chn; n++) o[n] = (int8_t)(sqv.cht[n] - 64u);
    for (i = 0; i < n; i++) p[i] = rl_base() + o[i];
    return n;
}
static UI_LATE void rl_body(int32_t h)
{
    const uint32_t n = SQ_STEPS(sqp.lc);
    int32_t hp, hx, i, j, mn = 999, mx = -999, lo, y, p[SQ_CHV];
    const uint16_t dim = mix565(RGB(60, 60, 64), u_acc, 128u);
    char s[8];
    (void)h;
    i = (int32_t)rl_notes(&sqp.st[sqv.sel], p);
    if (!i) p[i++] = rl_base();          /* (CHORD ENTRY, all toggled out: the view on the base, no bar, 1.07) */
    hx = p[i - 1];
    hp = p[0];
    for (i = -3; i <= 4; i++) {          /* the view: centred on the other steps' pitches, the one held within */
        const sq_step_t *t = &sqp.st[(sqv.sel + (uint32_t)i) % n];
        const uint32_t k = rl_notes(t, p);
        if (!i || !(t->ev & SQ_ON)) continue;
        if (p[0] < mn) mn = p[0];
        if (p[k - 1u] > mx) mx = p[k - 1u];
    }
    lo = (mn > mx ? (hp + hx) / 2 : (mn + mx) / 2) - 12;
    if (hx > lo + 24) lo = hx - 24;
    if (hp < lo) lo = hp;                /* (a chord wider than the view: its lowest) */
    if (lo < 0) lo = 0;
    rl_grid(n, rl_play(sq.step, sq.run, n));
    for (i = 0; i < 25; i++) {           /* the octaves' C: ruled, named */
        const int32_t p = lo + 24 - i;
        if (p % 12) continue;
        y = RL_PY + i * RL_RH + RL_RH / 2;
        cv_rect(RL_X0, y, 232 - RL_X0, 1, U_KNOBRIM);
        s[0] = 0;
        note_text((uint32_t)p, s);
        cv_text(0, y - 7, &FONT_XS, s, U_DIMTXT);
    }
    for (i = -15; i <= 4; i++) {         /* the bars (earlier steps too: a long one reaches in), the velocities */
        const sq_step_t *t = &sqp.st[(sqv.sel + (uint32_t)i) % n];
        const uint32_t ev = t->ev & 15u, on = t->ev & SQ_ON, k = rl_notes(t, p);
        const uint16_t c = i ? dim : u_acc;
        int32_t x = -1;
        if (!on && i) continue;
        for (j = (int32_t)k; j--; ) {    /* (a CHORD step: a bar a note, at its pitch; 1.02) */
            const int32_t q = p[j] < lo ? lo : p[j] > lo + 24 ? lo + 24 : p[j];
            y = RL_PY + (lo + 24 - q) * RL_RH;
            x = rl_bar(RL_X0 + (i + 3) * RL_CW + SQ_AL(t) * RL_CW / 8, (int32_t)SQ_LEN(t->la & 31u) * RL_CW / 8, y,
                       RL_RH, c, i ? 0 : on ? 1 : 2);
        }
        if (x < 0) continue;
        if (ev != SQ_NOTE && ev != SQ_CHORD) {   /* (FING POLY "P", FING n "F2") */
            s[0] = ev == SQ_POLY ? 'P' : 'F';
            s[1] = (char)('1' + ev - SQ_FING);
            s[ev == SQ_POLY ? 1 : 2] = 0;
            cv_text(x + 1, y - 11, &FONT_T, s, i ? U_DIMTXT : U_TEXT);
        }
        if (i >= -3) rl_lane(i + 3, t->vel * 26 / 127, c);
    }
}
static UI_LATE uint32_t rl_sig(void)
{
    const uint32_t n = SQ_STEPS(sqp.lc);
    return fnv(&sqp, sizeof sqp) ^ fnv(sqv.cht, sqv.chn)   /* (the steps, the chords; the notes gathered, 1.02) */
           ^ ((uint32_t)rl_base() << 1 | (uint32_t)sqv.sel << 8 | (uint32_t)sqp.lc << 14
                                         | rl_play(sq.step, sq.run, n) << 16);
}
static UI_LATE int sq_tt_on(void);
/* a sequencer's page (all: the whole of it): a step held and turned (the arp sequencer's: a CHORD step, 1.18C31; any
 * step while LENGTH, ALIGN or VEL was turned last, 1.18C33, OFFSET too, 1.18C35), its roll (body; drawn whole when its signature
 * sig changes, at most every RL_MS): 1; else 0 and the page's parts to draw, all of them when asked or after the
 * roll (the page cleared, their signatures 0) */
static UI_LATE int rl_draw(int *all, uint32_t (*sig)(void), void (*body)(int32_t))
{
    uint32_t i, now, g;
    if (!sqv.key || !sqv.edited
        || (ui.view == V_SEQ && (sqp.st[sqv.sel].ev & 15u) != SQ_CHORD
            && !(sqv.tt - 2u < 4u && (sqv.lin || sq_tt_on())))) {   /* (1.18C31: the arp sequencer's roll for a CHORD
                                          * step alone, CHORD ENTRY's too; NOTE, FING 1-8, FING POLY: the page, its
                                          * step columns showing the step's values, as before 0.79. 1.18C33: LENGTH,
                                          * ALIGN, VEL turned last: the roll on every event, until SQ_TT_MS after the
                                          * last turn while held, SQ_TT_MS after the key was let go, sqv.lin; 1.18C35:
                                          * OFFSET too (sqv.tt 5; it acts on NOTE and FING n alone, sq_knob)) */
        if (RL_SIG) *all = 1;            /* (the roll closed: the page again) */
        if (*all) {
#ifdef APP_HOST_CMD
            sq_fulls++;                  /* (the host: the page drawn whole, "fullis") */
#endif
            lcd_fill(4, UI_ROW0_Y, 232, UI_BODY_H, U_BG);
            for (i = 0; i < DR_NPART + 2u; i++) SQ_PART[i] = 0;
        }
        return 0;
    }
    now = plat_ms();
    g = sig() | 1u;
    if (!*all && (g == RL_SIG || (RL_SIG && now - RL_T < gui_gap(RL_MS)))) return 1;
    draw_body(UI_ROW0_Y, UI_BODY_H, body);
    RL_SIG = g;
    RL_T = now;
    return 1;
}

/* the drums' page (app.c dr_*): the progress row as the arp sequencer's (slimmer), below it the
 * 11 instruments in a row (lit: chosen; grey: with steps within the pattern's length; what the step sounding strikes
 * is on the black keys' lights, and the steps on the white keys', not on the screen), below them the blocks, large:
 * the step held (ACCENT, LENGTH, ALIGN, STEP), the drum sequencer (LENGTH, TIME DIV, PATTERN, CLEAR; LEVEL on SELECT).
 * Drawn as the arp sequencer's page: a part again only when its signature changed */
#define DR_PG_H 22                       /* the progress row's height */
#define DR_IY 24                         /* the instruments' row: its top, a cell's width and height */
#define DR_IW 21
#define DR_IH 20
#define DR_B1_Y 46                       /* the blocks' tops (DR_B_H high) */
#define DR_B2_Y 109
#define DR_B_H 62
static const char *const DR_LENT[DR_NLEN] = {"15%", "25%", "40%", "60%", "100%", "140%", "200%", "300%"};
static UI_LATE uint32_t dr_has(uint32_t i);
static UI_LATE uint32_t dr_cv(uint32_t w, char *s, uint32_t *mx, uint32_t *bi)   /* column w's value and text */
{
    static const char *const ACT[DR_NACC] = {"OFF", "ACC", "ACC+"};
    const uint32_t b = DR_ST(sqv.ins, sqv.sel);
    uint32_t v = 0;
    *mx = 1;
    *bi = 0;
    s[0] = 0;
    if (w == 1u) { v = b & 3u; *mx = DR_NACC - 1u; text_cat(s, ACT[v > 2u ? 2u : v]); }
    else if (w == 2u) { v = b >> 2 & 7u; *mx = DR_NLEN - 1u; text_cat(s, DR_LENT[v]); }
    else if (w == 3u) { v = (uint32_t)(DR_AL(sqv.ins, sqv.sel) + 7); *mx = 14u; *bi = 1; put_align(s, (int32_t)v - 7); }
    else if (w == 4u) { v = DR_ON(sqv.ins, sqv.sel); text_cat(s, v ? "ON" : "OFF"); }
    else if (w == 6u) { v = drp.lc; *mx = SQ_NLC - 1u; put_sint(s, (int32_t)SQ_STEPS(v), 0); }
    else if (w == 7u) { v = drp.div; *mx = SQ_NDIV - 1u; *bi = 1; text_cat(s, SQ_DIVT[v]); }
    else if (w == 8u) {                  /* (NONE: no pattern) */
        v = (uint32_t)(SQ_QPAT(2u, DR_PATSH) + 1); *mx = DR_PATS;   /* (0.96: the one waiting for the line) */
        if (v) put_sint(s, (int32_t)v, 0); else text_cat(s, "NONE");
    } else if (w == 9u) text_cat(s, sqv.clr ? "REC?" : "--");
    else { v = set.dr_lev; *mx = DR_LMAX; put_sint(s, (int32_t)v, 0); }   /* (above 100: the drive, dr_level) */
    return v;
}
static UI_LATE uint32_t dr_sig(uint32_t w)       /* what part w shows (never 0) */
{
    const uint32_t k = sqv.key ? 1u : 0u, b = DR_ST(sqv.ins, sqv.sel), on = DR_ON(sqv.ins, sqv.sel);
    if (w < SQ_PG) return pg_sig(w, drp.lc, sqv.blk[1], dr.run, dr.step);
    w -= SQ_PG;
    if (w < DR_NI)
        return 1u | (dr_has(w) ? 2u : 0u) | (sqv.ins == w ? 4u : 0u);
    w -= DR_NI;
    if (w == 0u) return 1u;              /* (unused: nothing drawn; the info box in 0.94-0.95) */
    if (w <= 4u)
        return 1u | k << 1 | (uint32_t)sqv.sel << 2
               | (w == 1u ? b & 3u : w == 2u ? b >> 2 & 7u : w == 3u ? (uint32_t)(DR_AL(sqv.ins, sqv.sel) + 7) : on) << 8;
    if (w == 5u) return 1u;              /* (unused: nothing drawn) */
    if (w == 10u) return 1u | (uint32_t)set.dr_lev << 2;   /* (1.18C24: LEVEL has no column, its knob works; 1.19.7:
                                          * its value in the signature, so the strip's title follows each detent
                                          * (sq_tt_sig); up to 1.19.6 it kept the first detent's value) */
    return 1u | k << 1 | (w == 6u ? drp.lc : w == 7u ? drp.div : w == 8u ? (set.sq_pad & DR_DISARM ? 255u : set.dr_pat)
                                                                              | sq_qt[2] << 8
                          : sqv.clr) << 2;
}
static UI_LATE void dr_cell(uint32_t i)          /* an instrument's cell */
{
    const uint32_t g = dr_sig(SQ_PG + i);
    const int has = (g & 2u) != 0, sel = (g & 4u) != 0;
    const uint16_t bg = sel ? u_acc : has ? U_TRACK : U_ROWOFF, tx = sel ? U_ONACC : has ? U_TEXT : U_DIMTXT;
    const felucca_font_t *f = text_w(&FONT_B, DR_NAME[i]) <= DR_IW - 2 ? &FONT_B : &FONT_T;
    cv_rect(0, 0, DR_IW - 1, DR_IH, bg);
    cv_text((DR_IW - 1 - text_w(f, DR_NAME[i])) / 2, (DR_IH - f->h) / 2, f, DR_NAME[i], tx);
}
/* the drums' roll (0.79; rl_*): a step held and turned, the body shows the 8 steps around it
 * as the arp sequencer's roll does, a row per instrument (BD at the bottom, as a drum grid's notes go), each hit a bar
 * from its place + ALIGN (0.80: early too, the step after the window's reaching in)), as long as its LENGTH (15 % .. 300 % of a step: the decay's scale), as bright as its
 * ACCENT; the chosen instrument's row lit, its hit at the step held in the accent with a light rim (off: hollow);
 * under the numbers the chosen instrument's accents (a bar per hit: OFF a third high, ACC two, ACC+ full) */
#define DL_RH 11                         /* an instrument's row */
#define DL_RY (RL_GH - (int32_t)DR_NI * DL_RH)
static UI_LATE void dl_body(int32_t h)
{
    const uint32_t n = SQ_STEPS(drp.lc);
    uint32_t r;
    int32_t i, y;
    (void)h;
    cv_rect(RL_X0, DL_RY + (int32_t)(DR_NI - 1u - sqv.ins) * DL_RH, 232 - RL_X0, DL_RH, U_TRACK);
    rl_grid(n, rl_play(dr.step, dr.run, n));
    for (r = 0; r < DR_NI; r++) {
        y = DL_RY + (int32_t)(DR_NI - 1u - r) * DL_RH;
        cv_text(2, y + (DL_RH - FONT_XS.h) / 2, &FONT_XS, DR_NAME[r], r == sqv.ins ? U_TEXT : U_DIMTXT);
        for (i = -6; i <= 5; i++) {      /* (300 % from 7/8: three steps before the window reach in; the one after it
                                          * early) */
            const uint32_t s = (sqv.sel + (uint32_t)i) % n, b = DR_ST(r, s), on = DR_ON(r, s), hd = !i && r == sqv.ins;
            const int32_t a = DR_AL(r, s);
            const uint16_t c = hd ? u_acc : mix565(RGB(60, 60, 64), u_acc, 70u + 60u * (b & 3u));
            if ((!on && !hd) || (i == 5 && a >= 0)) continue;
            rl_bar(RL_X0 + (i + 3) * RL_CW + a * RL_CW / 8, (int32_t)(DR_LMUL[b >> 2 & 7u] * RL_CW),
                   y + 2, DL_RH - 4, c, hd ? (on ? 1 : 2) : 0);
            if (r == sqv.ins && i >= -3 && i <= 4) rl_lane(i + 3, (int32_t)((b & 3u) + 1u) * 26 / 3, c);
        }
    }
}
static UI_LATE uint32_t dl_sig(void)
{
    const uint32_t n = SQ_STEPS(drp.lc);
    return fnv(&drp, sizeof drp) ^ ((uint32_t)sqv.sel << 8 | (uint32_t)sqv.ins << 16 | rl_play(dr.step, dr.run, n) << 20);
}
static UI_LATE void seq_part_body(int32_t hh)   /* a sequencer's page's part sqv.what (the drums' on V_DRUM): a knob's
                                          * column (the drums': an instrument's cell too; 1.03: one for both pages) */
{
    const uint32_t d = ui.view == V_DRUM, w = sqv.what - SQ_PG - (d ? DR_NI : 0u), e = !d;
    char s[12];
    uint32_t mx, bi, v;
    int on;
    (void)hh;
    if (d && w >= 0u - DR_NI) { dr_cell(w + DR_NI); return; }   /* (the drums' instruments) */
    v = d ? dr_cv(w, s, &mx, &bi) : sq_cv(w, s, &mx, &bi);
    on = w <= 4u + e ? sqv.key != 0 : !sqv.key;
    if (e && w == 5u && !SQ_OFS(sqp.st[sqv.sel].ev & 15u)) on = 0;   /* (OFFSET: NOTE's and FING n's, 1.18C31) */
    sq_colbox(sqv.ox, d ? DR_B_H : SQ_B_H, on, (w == 1u || w == 6u + e) | (w == (d ? 9u : 11u)) << 1,
              v, mx, (int)bi, d ? DR_SH[w] : SQ_SH[w], s,
              (w == 9u + e && sqv.clr) || (w == 8u + e && sq_qt[d ? 2u : UN_SQI]));   /* (0.96: a pattern waiting,
                                          * the accent) */
}
static UI_LATE void seq_draw(int all)            /* a sequencer's page (the drums' on V_DRUM): all of it, or the parts
                                          * that changed, each alone in a canvas of its size, in bands (1.03: one for
                                          * both pages, sq_draw and dr_draw up to 1.02) */
{
    const uint32_t d = ui.view == V_DRUM, u = SQ_PG + (d ? DR_NI : 0u);
    uint32_t i, g;
    int32_t x, y, wd, h;
    if (rl_draw(&all, d ? dl_sig : rl_sig, d ? dl_body : rl_body)) return;   /* (a step held and turned: the roll) */
    for (i = 0; i < (d ? DR_NPART : SQ_NPART); i++) {
        if ((g = d ? dr_sig(i) : sq_sig(i)) == SQ_PART[i]) continue;
        SQ_PART[i] = g;
        if (i < SQ_PG) { if (g != 1u) pg_part(i, d ? DR_PG_H : SQ_PG_H); continue; }
        if (i == u || i == u + 6u - d || (d && i == u + 10u)) continue;   /* (unused: the info box, 0.94-0.95; block 2's tag, up to 0.93) */
#ifdef APP_HOST_CMD
        if (i >= u) sq_ncol++;           /* (the host: a knob's column, hostcmd.c fullis) */
#endif
        if (i < u) { x = (int32_t)(i - SQ_PG) * DR_IW; y = DR_IY; wd = DR_IW - 1; h = DR_IH; }   /* (the drums'
                                          * instruments) */
        else {                           /* (the blocks: the arp sequencer's 5 + 5 columns; the drums' 4 of 58 over
                                          * the width (0.94; 53 beside the tag before), then 5) */
            const uint32_t w = i - u, b = w <= 5u - d;
            h = d ? DR_B_H : SQ_B_H;
            y = b ? (d ? DR_B1_Y : SQ_B1_Y) : d ? DR_B2_Y : SQ_B2_Y;
            x = SQ_COL_X(w - (b ? 1u : 7u - d));
            wd = SQ_COL_X(w - (b ? 0u : 6u - d)) - x;
            if (d) { x = (int32_t)(w - (b ? 1u : 6u)) * 58; wd = 58; }   /* (1.18C24: block 2 four of 58 too) */
        }
        sqv.what = (uint8_t)i;
        sqv.ox = (int16_t)wd;            /* (its width for seq_part_body; the canvas is the part's) */
        part_bands(x, y, wd, h, seq_part_body);
    }
}

/* the drum kit's page (app.c kit_*): three blocks of four knobs in the menus' look (a block's
 * tag up its left side, the knob, its label, its value large), the one K1..K4 act on lit: the instrument's SOUND
 * (LEVEL, TUNE, DECAY, ATTACK or CLICK) and VOICE (TONE or SWEEP, MODEL, PAN, SEND), the drums' BUS (LEVEL, DRIVE,
 * COMP) with the instrument's RESET on K4 (0.88; up to 0.87 VOICE's K4, BUS's empty). Two rows: blocks kv.top and kv.top + 1. A part (a tag, a column) is drawn again only when what it
 * shows changed (kit_sig; the signatures in SQ_PART: one page at a time) */
#define KIT_NPART 10u                    /* the first row's tag and four columns, the second row's */
#define KIT_TAG_W 2                      /* the row's left part: the frame's left side and a pixel (0.87: the tag
                                          * up the left side gone; its room the columns', as the menus' rows) */
static UI_LATE uint32_t kit_blk(uint32_t w) { return kv.top + (w >= 5u); }   /* the block part w shows */
static UI_LATE uint32_t kit_val(uint32_t w)   /* column w's value (w 1..4 the first row, 6..9 the second) */
{
    const dr_kp_t *q = &drk.k[sqv.ins];
    const uint32_t b = kit_blk(w), c = w % 5u;
    if (b == KB_SOUND) return c == 1u ? q->lev : c == 2u ? q->tune : c == 3u ? q->dec : q->att;
    if (b == KB_VOICE) return c == 1u ? q->tone : c == 2u ? q->m909 : c == 3u ? (uint32_t)(q->pan + DR_KPAN) : q->snd;
    return c == 1u ? set.dr_lev : c == 2u ? (drk.drv & 15u) : c == 3u ? (uint32_t)(drk.drv >> 4) : kv.ask;
}
static UI_LATE uint32_t kit_sig(uint32_t w)      /* what part w shows (never 0) */
{
    const uint32_t b = kit_blk(w), on = b == kv.blk, g = 1u | on << 1 | b << 2 | (uint32_t)sqv.ins << 4;
    return w % 5u == 0u ? g : g | kit_val(w) << 8;
}
static UI_LATE void kit_place(uint32_t w, int32_t *x, int32_t *y, int32_t *wd)   /* part w's place (UI_ROW_H high) */
{
    const uint32_t c = w % 5u;
    *y = w < 5u ? 0 : UI_ROW1_Y - UI_ROW0_Y;
    *x = c ? UI_COL_X(c - 1u) : 0;
    *wd = !c ? KIT_TAG_W : c == 4u ? 232 - *x : UI_COL_W;
}
static UI_LATE void kit_tag(uint32_t w)          /* a row's left part: the frame's side (the block: its knobs'
                                                 * labels; the instrument: the strip) */
{
    const int on = kit_blk(w) == kv.blk;
    const int32_t H = UI_ROW_H;
    const uint16_t bg = on ? mix565(U_BG, u_acc, 28u) : U_ROWOFF;
    u_frame(0, 0, KIT_TAG_W + 1, H, on ? u_acc : U_BORDER, bg);
}
static UI_LATE void kit_col(uint32_t w)          /* a knob's column: its knob, label and value */
{
    const uint32_t ins = sqv.ins, sw = DR_SWEPT(ins), v = kit_val(w), b = kit_blk(w), c = w % 5u;
    const int32_t H = UI_ROW_H;
    int32_t x, y, wd, cx;
    uint32_t mx = DR_KN;
    int on = b == kv.blk, bi = 1;
    const char *lb;
    const felucca_font_t *f;
    uint16_t bg, bd, vc;
    char s[12];
    kit_place(w, &x, &y, &wd);
    bg = on ? mix565(U_BG, u_acc, 28u) : U_ROWOFF;
    bd = on ? u_acc : U_BORDER;
    cv_rect(0, 0, wd, H, bg);
    cv_rect(0, 0, wd, 1, bd);
    cv_rect(0, H - 1, wd, 1, bd);
    if (c == 4u) cv_rect(wd - 1, 0, 1, H, bd);
    s[0] = 0;
    if (b == KB_BUS && c < 4u) {         /* (the drums' bus: LEVEL as the drum sequencer's, DRIVE, COMP; K4 the
                                          * instrument's RESET, 0.88) */
        lb = c == 1u ? "LEVEL" : c == 2u ? "DRIVE" : "COMP";
        mx = c == 1u ? DR_LMAX : c == 2u ? DV_N - 1u : DC_N - 1u;
        bi = 0;
        kit_bus_text(c - 1u, s);
    } else if ((b == KB_SOUND && c == 1u) || (b == KB_VOICE && c == 4u)) {   /* (LEVEL; SEND 0.88: into part A's
                                          * delay / reverb) */
        lb = b == KB_SOUND ? "LEVEL" : "SEND";
        mx = 127u;
        bi = 0;
        put_sint(s, (int32_t)v, 0);
    }
    else if (c == 2u - (b == KB_VOICE)) {   /* (TUNE, TONE: -12 .. +12) */
        lb = b == KB_SOUND ? "TUNE" : sw ? "SWEEP" : "TONE";
        if (v != DR_K0) put_sint(s, (int32_t)v - (int32_t)DR_K0, 1); else text_cat(s, "0");
    } else if (b == KB_SOUND && c == 3u) {   /* (DECAY: the factor in %) */
        lb = "DECAY";
        put_sint(s, (int32_t)(100.f * DR_SEMI[v] * DR_SEMI[v] + 0.5f), 0);
        text_cat(s, "%");
    } else if (b == KB_SOUND) {          /* (ATTACK: 2 ms a step; CLICK: 4 % a step) */
        lb = sw ? "CLICK" : "ATTACK";
        mx = DR_KATT;
        bi = 0;
        put_sint(s, (int32_t)(v * (sw ? 4u : 2u)), 0);
        if (!sw && v) text_cat(s, "ms");
    } else if (c == 2u) {                /* (MODEL: BD SD and the toms; the hats with their sample's table: SYN the
                                          * synthesized, SMP the sample, 0.98; the others have one voice) */
        lb = "MODEL";
        mx = 1u;
        bi = 0;
        if (ins <= DR_HT) text_cat(s, v ? "909" : "808");
        else if (KIT_MODEL(ins)) text_cat(s, v ? "SMP" : "SYN");
        else { text_cat(s, "--"); on = 0; }
    } else if (c == 3u) {                /* (PAN: C, L n, R n) */
        lb = "PAN";
        mx = 2u * DR_KPAN;
        text_cat(s, v == DR_KPAN ? "C" : v < DR_KPAN ? "L" : "R");
        if (v != DR_KPAN) put_sint(s + 1, v < DR_KPAN ? DR_KPAN - (int32_t)v : (int32_t)v - DR_KPAN, 0);
    } else {                             /* (RESET: turned, REC? for SQ_CLR_MS; REC resets) */
        lb = "RESET";
        mx = 1u;
        bi = 0;
        text_cat(s, v ? "REC?" : "--");
    }
    cx = UI_COL_C;
    f = text_w(&FONT_L, s) <= UI_VAL_W ? &FONT_L : &FONT_M;
    vc = b == KB_BUS && c == 4u && v ? u_acc : on ? U_VALON : RGB(176, 176, 182);
    u_col_knob(cx, H, (float)(v > mx ? mx : v) / (float)mx, bi, on, lb, s, f, vc);
    (void)y;
}
static UI_LATE void kit_part_body(int32_t h)
{
    (void)h;
    if (sqv.what % 5u == 0u) kit_tag(sqv.what); else kit_col(sqv.what);
}
static UI_LATE void kit_part(uint32_t w)         /* one part alone, in a canvas of its size, in bands */
{
    int32_t x, y, wd;
    kit_place(w, &x, &y, &wd);
    sqv.what = (uint8_t)w;
    part_bands(x, y, wd, UI_ROW_H, kit_part_body);
}
static UI_LATE void kit_draw(int all)            /* the page: all of it, or the parts that changed */
{
    uint32_t i, g;
    if (all) {
        lcd_fill(4, UI_ROW0_Y, 232, UI_BODY_H, U_BG);
        for (i = 0; i < KIT_NPART; i++) SQ_PART[i] = 0;
    }
    for (i = 0; i < KIT_NPART; i++)
        if ((g = kit_sig(i)) != SQ_PART[i]) { SQ_PART[i] = g; kit_part(i); }
}
#ifdef HYB_MULTI
/* (1.18C10) the pattern launcher's page (app.c ln_*): LN_SHOW rows in the big font, the row
 * selected framed on top and the next ones under it (the table ends at row 16, no wrap); per row its number, SYNTH 1's,
 * SYNTH 2's and the drums' pattern (the number in the part's colour on the mode's neutral cell, none "-"; SYNTH 2 off
 * with one synth: dim) and the mode (the row playing: dark green; the one waiting: a green rim); under them each part's
 * volume as a knob (muted or off: dim). A part key held frames its cell in the row selected and its knob. Each cell drawn alone, only
 * when its signature changes (ln_sg, 1.18C11), the whole page on entry */
#define LN_SHOW 5u                       /* the rows shown */
#define LN_BUDGET 4u                     /* the cells drawn a frame at most (each its own blit; the scan runs in the TIMER5 interrupt, so the draw never blocks it) */
#define LN_RH 28                         /* a row's height (LN_SHOW of them, then the knobs) */
#define LN_KY (LN_RH * (int32_t)LN_SHOW + 1)   /* the knobs' strip */
#define LN_X0 28                         /* the first pattern column; each LN_CW wide, then the mode's */
#define LN_CW 50
#define LN_XM (LN_X0 + 3 * LN_CW)
static const char *const LN_MNAME[LN_NMODE] = {"NO", "REP", "1x", "2x", "4x", "8x"};
#define LN_NSG (LN_SHOW * 5u + 5u)       /* the cells, the knobs, then the tempo and the battery */
static uint32_t ln_sg[LN_NSG] PLAT_POOL;   /* what each cell (5 a row: number, 3 patterns, mode), then the three knobs, the tempo and the battery, shows (never 0 once drawn) */
static uint8_t ln_dc, ln_dw;             /* the cell being drawn: its column (0 number, 1..3 patterns, 4 mode), its row */
#ifdef APP_HOST_CMD
static uint32_t ln_nd;                   /* (host tests: the bands drawn, "lncnt") */
#endif
static UI_LATE void ln_cx(uint32_t c, int32_t *x, int32_t *w)   /* column c's left edge and width */
{
    *x = c == 0u ? 0 : c < 4u ? LN_X0 + (int32_t)(c - 1u) * LN_CW : LN_XM;
    *w = c == 0u ? LN_X0 : c < 4u ? LN_CW : 232 - LN_XM;
}
static UI_LATE uint32_t ln_sigc(uint32_t w, uint32_t c)   /* what cell c of the row w shows (1: nothing) */
{
    const uint32_t r = ln.sel + w, f = 2u | (w ? 0u : 4u);   /* (the table ends at row 16: no wrap) */
    if (r >= LN_ROWS) return 1u;
    if (c == 0u) return f | r << 4;
    if (c < 4u) return f | (uint32_t)set.ln[r][c - 1u] << 4 | (uint32_t)(!w && (ln.keys >> (c - 1u) & 1u)) << 12
                       | (uint32_t)(c == 2u && SQ_BMO) << 13 | (uint32_t)(ln_mute >> (c - 1u) & 1u) << 14;
    return f | (uint32_t)set.ln[r][3] << 4 | (uint32_t)(ln.play == r + 1u) << 8
           | (uint32_t)(ln.next == r + 1u && ln.next != ln.play) << 9;
}
static UI_LATE void ln_rim(int32_t x, int32_t y, int32_t w, int32_t h, int32_t t, uint16_t c)   /* a frame t thick */
{
    cv_rect(x, y, w, t, c);
    cv_rect(x, y + h - t, w, t, c);
    cv_rect(x, y, t, h, c);
    cv_rect(x + w - t, y, t, h, c);
}
static UI_LATE void ln_big(int32_t x, int32_t w, const char *s, uint16_t c)   /* s centred in a cell, the big font
                                          * where it fits */
{
    const felucca_font_t *f = text_w(&FONT_L, s) <= w - 4 ? &FONT_L : &FONT_M;
    cv_text(x + (w - text_w(f, s)) / 2, f == &FONT_L ? LN_RH - 2 - 32 + 2 : LN_RH - 2 - 24 - 2, f, s, c);
}
static UI_LATE void ln_cell(int32_t h)   /* the cell ln_dw, ln_dc of the table (canvas: the cell alone; the band callback gets the height, the width is the column's) */
{
    int32_t wd, x0;
    (void)h;
    ln_cx(ln_dc, &x0, &wd);
    (void)x0;
    const uint32_t r = ln.sel + ln_dw, c = ln_dc;
    const int32_t H = LN_RH - 2;
    char s[8];
    if (r >= LN_ROWS) return;
    s[0] = 0;
    if (c == 0u) {
        put_sint(s, (int32_t)r + 1, 0);
        cv_text((wd - text_w(&FONT_M, s)) / 2, H - 26, &FONT_M, s, ln_dw ? U_DIMTXT : U_TEXT);
    } else if (c < 4u) {                 /* (the part's colour filled, none grey) */
        const uint32_t q = c - 1u, v = set.ln[r][q], mo = q == 1u && SQ_BMO, mu = ln_mute >> q & 1u;
        cv_rect(2, 1, wd - 4, H, !v && !mu ? RGB(44, 44, 49) : mu ? RGB(178, 178, 184)
                                 : mo ? U_THEMES[U_TH_GREY].lcdrim : U_THEMES[q].acc);   /* (1.18C21: muted: light grey;
                                          * 1.19.3: SEQ B MIDI only in SINGLE, a mid grey) */
        if (v) put_sint(s, (int32_t)v, 0); else text_cat(s, "-");
        ln_big(2, wd - 4, s, mu ? RGB(36, 36, 40) : !v ? U_DIMTXT : mo ? U_THEMES[U_TH_GREY].valon : U_THEMES[q].onacc);
    } else {
        const uint32_t m = set.ln[r][3] < LN_NMODE ? set.ln[r][3] : 0u;
        cv_rect(2, 1, wd - 4, H, ln.play == r + 1u ? RGB(110, 190, 255) : RGB(58, 58, 64));   /* (the row playing: light blue, black text) */
        if (ln.next == r + 1u && ln.next != ln.play) ln_rim(2, 1, wd - 4, H, 2, RGB(60, 220, 90));
        ln_big(2, wd - 4, LN_MNAME[m], ln.play == r + 1u ? RGB(0, 0, 0) : m == LN_NO ? U_DIMTXT : U_TEXT);
    }
    if (!ln_dw) {                        /* (the row selected: framed, its edges in the cells) */
        cv_rect(0, 0, wd, 2, U_TEXT);
        cv_rect(0, LN_RH - 2, wd, 2, U_TEXT);
        if (c == 0u) cv_rect(0, 0, 2, LN_RH, U_TEXT);
        if (c == 4u) cv_rect(wd - 2, 0, 2, LN_RH, U_TEXT);
    }
    if (!ln_dw && c > 0u && c < 4u && (ln.keys >> (c - 1u) & 1u)) ln_rim(0, 0, wd, LN_RH, 2, U_TEXT);   /* (its part key held: that part's cell of the row selected only) */
}
static UI_LATE void ln_knob1(int32_t h)  /* part ln_dc's volume as a knob (muted or off: dim; its key held: framed) */
{
    const uint32_t c = ln_dc, v = c == 2u ? set.dr_lev : pv_vol[c], mx = c == 2u ? DR_LMAX : 127u;
    const u_theme_t *t = u_th;
    if (LN_EP == c) cv_rect(0, 0, LN_CW, h, U_THEMES[c].lcdglow);   /* (1.18C21: the part edited: its colour half lit behind) */
    u_th = &U_THEMES[c];                 /* (its arc in the part's colour) */
    if (c == 1u && SQ_BMO)               /* (1.19.3: SEQ B MIDI only in SINGLE: "MIDI" for part B's volume) */
        cv_text((LN_CW - text_w(&FONT_M, "MIDI")) / 2, (h - FONT_M.h) / 2, &FONT_M, "MIDI",
                ln_mute >> 1 & 1u ? U_DIMTXT : U_THEMES[U_TH_GREY].valon);
    else knob((LN_CW - 26) / 2, 2, (float)v / (float)mx, 0, !(ln_mute >> c & 1u), 26);
    u_th = t;
    if (ln.keys >> c & 1u) ln_rim(0, 0, LN_CW, h, 2, U_TEXT);
}
static UI_LATE uint32_t ln_bpm(void)     /* the tempo as the transport runs it: an external MIDI clock's, else the one set (HOME's tile: tp_run) */
{
    return S.xclk_on && S.bpm_now > 1.f ? (uint32_t)(S.bpm_now + 0.5f) : (uint32_t)(tp_run ? tp_run - 1u : shadow[TP_BYTE]) + 63u;
}
static UI_LATE void ln_tempo(int32_t h)  /* the tempo, left of the knobs under the row numbers: the biggest font that fits the column */
{
    const felucca_font_t *f = &FONT_L, *const F[4] = {&FONT_M, &FONT_B, &FONT_S, &FONT_XS};
    char s[8];
    uint32_t i;
    s[0] = 0;
    put_sint(s, (int32_t)ln_bpm(), 0);
    for (i = 0; i < 4u && text_w(f, s) > LN_X0 - 2; i++) f = F[i];
    cv_text((LN_X0 - text_w(f, s)) / 2, (h - f->h) / 2, f, s, U_DIMTXT);
}
/* the battery under the mode column: an outline with a tip at its right, filled from the left by the charge (green over
 * 40 %, yellow 15-40 %, red under 15 %), on USB full with a white bolt; not read yet: the outline only. plat_battery's
 * levels 0..3 are bars of the divider's reading: 10, 35, 65 and 100 % */
static UI_LATE void ln_batt(int32_t h)
{
    static const uint8_t BOLT[9][2] = {{5, 3}, {4, 3}, {3, 3}, {1, 6}, {3, 6}, {4, 3}, {3, 3}, {2, 3}, {1, 3}};   /* (x, width) a row */
    const int b = (int)(ln_sg[LN_SHOW * 5u + 4u] & 0xFFu) - 2;   /* (the signature's level: -1 .. 4) */
    const int32_t bx = 3, by = 5, bw = 46, bh = h - 10, iw = bw - 6;
    const uint16_t grey = RGB(190, 190, 196), col = b >= 2 ? RGB(70, 200, 95) : b == 1 ? RGB(235, 180, 30) : RGB(230, 60, 45);
    const int32_t w = b < 0 ? 0 : b > 3 ? iw : iw * (b == 0 ? 10 : b == 1 ? 35 : b == 2 ? 65 : 100) / 100;
    cv_rect(bx, by, bw, 2, grey);
    cv_rect(bx, by + bh - 2, bw, 2, grey);
    cv_rect(bx, by, 2, bh, grey);
    cv_rect(bx + bw - 2, by, 2, bh, grey);
    cv_rect(bx + bw, by + 6, 3, bh - 12, grey);
    if (w > 0) cv_rect(bx + 3, by + 3, w, bh - 6, col);
    if (b > 3) {                         /* (on USB: a bolt, black round it, white) */
        const int32_t x0 = bx + 3 + (iw - 8) / 2, y0 = by + (bh - 9) / 2;
        uint32_t k, r;
        for (k = 0; k < 5u; k++)         /* (k 0..3: the black copies a pixel off, 4: the white one) */
            for (r = 0; r < 9u; r++) {
                const int32_t dx = k == 0u ? -1 : k == 1u ? 1 : 0, dy = k == 2u ? -1 : k == 3u ? 1 : 0;
                cv_rect(x0 + BOLT[r][0] + dx, y0 + (int32_t)r + dy, BOLT[r][1], 1, k == 4u ? RGB(255, 255, 255) : RGB(0, 0, 0));
            }
    }
}
static UI_LATE void ln_draw(int all)     /* the page: all of it, or the cells that changed (1.18C11: each alone) */
{
    uint32_t w, c, g, nb = LN_BUDGET;    /* (1.18C20: at most LN_BUDGET cells a frame; the rest follows in the next frames, drawn
                                          * from what the table is then (the signatures), so a row moved again meanwhile
                                          * draws only its latest target) */
    if (all) {
        lcd_fill(4, UI_ROW0_Y, 232, UI_BODY_H, U_BG);
        for (w = 0; w < LN_NSG; w++) ln_sg[w] = 0;
    }
    for (w = 0; w < LN_SHOW; w++) {
        for (c = 0; c < 5u; c++) {
            int32_t x, wd;
            g = ln_sigc(w, c);
            if (g == ln_sg[w * 5u + c]) continue;
            if (!nb) return;
            nb--;
            ln_sg[w * 5u + c] = g;
            ln_dw = (uint8_t)w;
            ln_dc = (uint8_t)c;
            ln_cx(c, &x, &wd);
#ifdef APP_HOST_CMD
            ln_nd++;
#endif
            part_bands(x, (int32_t)w * LN_RH, wd, LN_RH, ln_cell);
        }
    }
    for (c = 0; c < 3u; c++) {           /* (the knobs under the columns) */
        const uint32_t v = c == 2u ? set.dr_lev : pv_vol[c];
        g = 1u << 29 | v | (uint32_t)(ln_mute >> c & 1u) << 8 | (uint32_t)(ln.keys >> c & 1u) << 9
            | (uint32_t)(c == 1u && SQ_BMO) << 10 | (uint32_t)(LN_EP == c) << 11;
        if (g == ln_sg[LN_SHOW * 5u + c]) continue;
        if (!nb) return;
        nb--;
        ln_sg[LN_SHOW * 5u + c] = g;
        ln_dc = (uint8_t)c;
#ifdef APP_HOST_CMD
        ln_nd++;
#endif
        part_bands(LN_X0 + (int32_t)c * LN_CW, LN_KY, LN_CW, UI_BODY_H - LN_KY, ln_knob1);
    }
    g = 1u << 29 | ln_bpm();             /* (the tempo and the battery: each when its value changes; the battery's level is read
                                          * here, a few loads, no more) */
    if (g != ln_sg[LN_SHOW * 5u + 3u] && nb) {
        nb--;
        ln_sg[LN_SHOW * 5u + 3u] = g;
#ifdef APP_HOST_CMD
        ln_nd++;
#endif
        part_bands(0, LN_KY, LN_X0, UI_BODY_H - LN_KY, ln_tempo);
    }
    g = 1u << 29 | (uint32_t)(plat_battery() + 2);
    if (g != ln_sg[LN_SHOW * 5u + 4u] && nb) {
        nb--;
        ln_sg[LN_SHOW * 5u + 4u] = g;
#ifdef APP_HOST_CMD
        ln_nd++;
#endif
        part_bands(LN_XM, LN_KY, 232 - LN_XM, UI_BODY_H - LN_KY, ln_batt);
    }
}
/* (1.18C27) the launcher's sets' page (app.c ls_*): a head line (the way back to the rows), LS_SHOW
 * slots (the one on K1 framed; filled: part A's and part B's patch, tag and name in the parts' colours, the rows
 * used; empty: dim), the strip over the knobs: K1's slot, the hint (REC and the question), K4's action. Each part
 * drawn alone when its signature changes (ls_sg) */
#define LS_SHOW 4u
#define LS_HH 24                         /* the head line's height */
#define LS_RH 28
#define LS_KY (LS_HH + LS_RH * (int32_t)LS_SHOW + 2)
static uint32_t ls_sg[LS_SHOW + 2u] PLAT_POOL;   /* the head, the slots' rows, the strip (never 0 once drawn) */
static uint8_t ls_dw PLAT_POOL;          /* the row being drawn (its slot) */
static UI_LATE uint32_t lsd_top(void)     /* the first slot shown */
{
    const uint32_t d = LS_SLOTS - 1u - lset.slot;   /* (1.18C28: drawn upside down: SET 1 at the bottom, the numbers rise upwards) */
    return d < 1u ? 0u : d - 1u > LS_SLOTS - LS_SHOW ? LS_SLOTS - LS_SHOW : d - 1u;
}
static UI_LATE void ls_head(int32_t h)
{
    (void)h;
    cv_text(4, 4, &FONT_XS, "LAUNCHER SETS", U_TEXT);
    cv_text(230 - text_w(&FONT_T, "SELECT DOWN: ROWS"), 6, &FONT_T, "SELECT DOWN: ROWS", U_DIMTXT);
    cv_rect(0, LS_HH - 1, 232, 1, RGB(58, 58, 64));
}
static UI_LATE void ls_slot(int32_t h)   /* slot ls_dw's row */
{
    const uint32_t k = ls_dw, on = k == lset.slot, full = lset.full >> k & 1u;
    char s[24];
    ls_rec_t r;
    (void)h;
    s[0] = 0;
    put_sint(s, (int32_t)k + 1, 0);
    cv_text((LN_X0 - text_w(&FONT_M, s)) / 2, LS_RH - 2 - 26, &FONT_M, s, on ? U_TEXT : U_DIMTXT);
    if (full && ls_read(k, &r)) {
        uint32_t q, i, n = 0;
        for (q = 0; q < 2u; q++) {
            const uint32_t i = (uint32_t)r.pt[q][0] | (uint32_t)r.pt[q][1] << 8;
            uint32_t j, m;
            s[0] = q ? 'B' : 'A'; s[1] = ' ';
            patch_tag(s + 2, i < PATCH_N ? i : PATCH_INIT);
            s[6] = ' ';
            for (j = 0, m = 7; j < 10u; j++) { const char c = r.nm[q][j]; s[m++] = c >= 32 && c < 127 ? c : ' '; }
            while (m > 7 && s[m - 1] == ' ') m--;
            s[m] = 0;
            cv_text(LN_X0 + 4, 2 + (int32_t)q * 12, &FONT_T, s, U_THEMES[q].acc);
        }
        for (i = 0; i < LN_ROWS; i++) n += r.ln[i][3] != 0u || (r.ln[i][0] | r.ln[i][1] | r.ln[i][2]);
        s[0] = 0;
        put_sint(s, (int32_t)n, 0);
        text_cat(s, n == 1u ? " ROW" : " ROWS");
        cv_text(228 - text_w(&FONT_T, s), 8, &FONT_T, s, U_DIMTXT);
    } else
        cv_text(LN_X0 + 4, 6, &FONT_XS, "EMPTY", U_DIMTXT);
    if (on) ln_rim(0, 0, 232, LS_RH, 2, lset.ask && lset.ask != LS_CLEAR + 1u ? RGB(255, 60, 40) : U_TEXT);   /* (the slot
                                          * asked about: red) */
}
static UI_LATE void ls_strip(int32_t h)  /* the knobs' strip: K1 the slot, the hint, K4 the action */
{
    const char *a = LS_ACT[lset.act < LS_NACT ? lset.act : 0];
    char s[8];
    (void)h;
    cv_text(LN_X0 + (LN_CW - text_w(&FONT_T, "SET")) / 2, 2, &FONT_T, "SET", U_DIMTXT);
    s[0] = 0;
    put_sint(s, lset.slot + 1, 0);
    cv_text(LN_X0 + (LN_CW - text_w(&FONT_S, s)) / 2, 14, &FONT_S, s, U_TEXT);
    if (lset.ask) {
        cv_text(LN_X0 + LN_CW + 2, 4, &FONT_XS, lset.ask == LS_SAVE + 1u ? "OVERWRITE?" : lset.ask == LS_DEL + 1u ? "DELETE?" : "CLEAR ROWS?",
                RGB(255, 60, 40));
        cv_text(LN_X0 + LN_CW + 2, 18, &FONT_T, "REC: YES", U_TEXT);
    } else
        cv_text(LN_X0 + LN_CW + 2, 10, &FONT_T, "REC: DO IT", U_DIMTXT);
    cv_rect(LN_XM + 2, 1, 232 - LN_XM - 4, h - 2 < 30 ? h - 2 : 30, RGB(58, 58, 64));
    cv_text(LN_XM + (232 - LN_XM - text_w(&FONT_S, a)) / 2, 8, &FONT_S, a, U_TEXT);
}
static UI_LATE void ls_draw(int all)     /* the sets' page: all of it, or the parts that changed */
{
    const uint32_t t = lsd_top(), base = (uint32_t)(lset.gen & 0x7Fu) << 24 | (uint32_t)lset.ask << 20 | 1u << 31;
    uint32_t w, g, nb = 2u;
    if (all) {
        lcd_fill(4, UI_ROW0_Y, 232, UI_BODY_H, U_BG);
        for (w = 0; w < LS_SHOW + 2u; w++) ls_sg[w] = 0;
    }
    g = 1u << 31 | lset.on;
    if (g != ls_sg[0]) { ls_sg[0] = g; nb--; part_bands(0, 0, 232, LS_HH, ls_head); }
    for (w = 0; w < LS_SHOW; w++) {
        const uint32_t k = LS_SLOTS - 1u - (t + w);
        g = base | k << 8 | (uint32_t)(k == lset.slot) << 7 | (lset.full >> k & 1u) << 6;
        if (g == ls_sg[1u + w]) continue;
        if (!nb) return;
        nb--;
        ls_sg[1u + w] = g;
        ls_dw = (uint8_t)k;
        part_bands(0, LS_HH + (int32_t)w * LS_RH, 232, LS_RH, ls_slot);
    }
    g = base | (uint32_t)lset.slot << 8 | lset.act;
    if (g == ls_sg[LS_SHOW + 1u] || !nb) return;
    ls_sg[LS_SHOW + 1u] = g;
    part_bands(0, LS_KY, 232, UI_BODY_H - LS_KY, ls_strip);
}
#endif

static void draw_status(uint32_t voices, uint32_t maxv, uint32_t cpu, int32_t db, uint32_t bpm)
{
    char s[24], t[12];
    int32_t x;
    prof_begin();
    cv_begin(232, 16, U_BG);    /* (16 rows: the canvas's band, app.h) */
    cv_rect(0, 0, 232, 1, U_RULE);
    x = cv_text(2, 1, &FONT_XS, "VCE ", U_STATUS);
    put_num(t, voices, 2); t[2] = '/'; put_num(t + 3, maxv, 2); t[5] = 0;
    cv_text(x, 1, &FONT_XS, t, U_TEXT);
    x = cv_text(62, 1, &FONT_XS, plat_cpu2_on() ? "CPU2 " : "CPU1 ", U_STATUS);   /* (the cores rendering) */
    cv_rect(x, 7, 16, 5, RGB(38, 38, 42));
    cv_rect(x, 7, (int32_t)(cpu > 100u ? 16u : cpu * 16u / 100u), 5, u_acc);
    put_sint(s, (int32_t)cpu, 0); text_cat(s, "%");
    cv_text(x + 19, 1, &FONT_XS, s, U_TEXT);
    put_sint(s, db, 0); text_cat(s, " dB");
    cv_text(170 - text_w(&FONT_XS, s), 1, &FONT_XS, s, U_TEXT);
    put_sint(s, (int32_t)bpm, 0); text_cat(s, " BPM");
    cv_text(230 - text_w(&FONT_XS, s), 1, &FONT_XS, s, U_STATUS);
    cv_blit(4, 220);
    prof_end(4);
}

/* HOME held: the block's time by part, % of the block (128 frames): E events, C the voices' control, V the voices on
 * this core, W the wait for the second core, A its voices added (one core: the voices), F the effects, P the
 * platform and the rest of the interrupt (the load less the engine's parts) */
/* the chip's microbenchmark (bench.c): cycles an op, over the rows; cyc: from the cycle counter, else TIMER4 */
static void draw_bench(const uint32_t *r, int cyc)
{
    static const char *const L[BENCH_N] = {"MAC FLASH", "MAC RAM", "ADD", "MUL", "DIV", "CVT", "LD RAM",
                                           "LD FL 4K", "LD FL 64K", "INT", "LD ROW", "ST ROW", "F SCALE",
                                           "LD POOL", "LD ROW POOL", "MAC DENORM", "ADD DENORM",
                                           "LD COLD", "CODE COLD", "CODE WARM", "MAC X4 IND", "BRANCH"};
    char s[16];
    int32_t i, c;
    for (c = 0; c < 2; c++) {            /* the upper canvas: the title and six; the lower: the other nine */
        int32_t i0 = c ? 6 : 0, i1 = c ? BENCH_N : 6;
        cv_begin(232, 71, U_BG);
        if (!c) cv_text(2, 1, &FONT_XS, cyc ? "CYCLES / OP  (COUNTER)" : "CYCLES / OP  (TIMER, 360 MHZ)", U_STATUS);
        for (i = i0; i < i1; i++) {
            int32_t j = i - i0, x = (j & 1) ? 118 : 2, y = c ? 1 + 8 * (j >> 1) : 18 + 17 * (j >> 1);
            char f[4] = {'.', 0, 0, 0};
            f[1] = (char)('0' + r[i] / 10u % 10u);
            f[2] = (char)('0' + r[i] % 10u);
            put_sint(s, (int32_t)(r[i] / 100u), 0);
            text_cat(s, f);
            x = cv_text(x, y, &FONT_XS, L[i], U_STATUS);
            cv_text(x + 4, y, &FONT_XS, s, U_TEXT);
        }
        cv_blit(4, c ? UI_ROW1_Y : UI_ROW0_Y);
    }
}

static void draw_prof(const float *t, uint32_t cpu, uint32_t n0, uint32_t nv)   /* t: the engine's parts (sy_prof) and the first core's
                                                     * voice stages (sy_prof_v), in ticks */
{
    static const char *const L[9] = {"E", "C", "O", "L", "M", "V", "W", "A", "F"};
    static const uint8_t IX[9] = {0, 1, 7, 8, 9, 2, 3, 4, 5};   /* O L M: the voices' oscillators, filters, amp;
                                                     * V: the first core's voices in all */
    const float pc = 100.f / (128.f * 1000000.f / 44100.f * (float)PLAT_TICKS_PER_US);
    char s[8];
    float sum = 0.f;
    int32_t x = 2, i;
    prof_begin();
    cv_begin(232, 16, U_BG);    /* (16 rows: the canvas's band, app.h) */
    cv_rect(0, 0, 232, 1, U_RULE);
    for (i = 0; i < 9; i++) {
        float p = t[IX[i]] * pc;
        if (i < 2 || i > 4) sum += p + (i == 8 ? t[6] * pc : 0.f);
        x = cv_text(x, 1, &FONT_XS, L[i], i >= 2 && i <= 4 ? U_DIMTXT : U_STATUS);
        put_sint(s, (int32_t)(p + 0.5f), 0);
        x = cv_text(x + 1, 1, &FONT_XS, s, U_TEXT) + 4;
    }
    x = cv_text(x, 1, &FONT_XS, "T", U_STATUS);  /* the core's total (the rest, platform and UI, is T minus the parts) */
    put_sint(s, (int32_t)cpu, 0);
    x = cv_text(x + 1, 1, &FONT_XS, s, U_TEXT) + 4;
    {   /* the split: this core's voices of all */
        char u[8];
        put_sint(s, (int32_t)n0, 0); text_cat(s, "/"); put_sint(u, (int32_t)nv, 0); text_cat(s, u);
    }
    cv_text(x, 1, &FONT_XS, s, U_DIMTXT);
    cv_blit(4, 220);
    prof_end(4);
}
