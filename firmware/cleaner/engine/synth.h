/* SPDX-License-Identifier: GPL-3.0-only */
/* The synthesizer engine: one part, a 256-byte patch, MIDI in, stereo out.
 * Portable C99, single-precision float, no allocations, no libm: the same sources build the host renderer (the
 * lab's interface), the FM-1 firmware and a later plugin.
 *
 *   static sy_synth s;                     (large: keep it static, or in the FM-1's pool)
 *   sy_init(&s, 44100.f);
 *   sy_load_patch(&s, bytes);
 *   sy_midi(&s, 0x90, 60, 100);
 *   sy_render(&s, left, right, in_l, in_r);  (SY_BLOCK frames; in_l / in_r may be 0)
 *
 * Everything that depends on time is derived from the sample rate given to sy_init, so the engine runs at the
 * reference's 46,875 Hz in the lab and at the codec's rate on the FM-1. */
#ifndef SYNTH_H
#define SYNTH_H
#include <stdint.h>
/* the reverb of modes 2-4: the own one (sy_cln_rv.c), on a ring (its word HYB_RV_BITS) */
#include "sy_cln_rv.h"

#define SY_BLOCK 128                    /* frames per control block (the reference's block) */
#ifndef CLN_OSC
#define CLN_OSC 1                       /* CLEANER: osc 1 and 2 the own band-limited oscillator (sy_cln_osc.inc) */
#endif
#ifndef SY_MAX_VOICES
#define SY_MAX_VOICES 40                /* voice slots (the reference plays 36-40); the FM-1 build sets fewer */
#endif
#define SY_FXN 27                       /* (the effects' bytes the matrix moves: A 105-119, 12 of page B; sy_fx.c SY_FXB) */
#ifndef SY_DELAY_FRAMES
#define SY_DELAY_FRAMES 65536           /* delay line per channel (frames, int16): 1.4 s at 46.9 kHz */
#endif
#define SY_EV_PARAM 0xF4              /* an internal event: set patch byte d1 to d2 at the block's start */
#define SY_EVQ 64                       /* queued MIDI events (the 2-block latency) */
#define SY_WAVE_N 256                   /* samples per wave table */
#define SY_WAVE_LEVELS 7                /* band-limited levels: 64, 32, 16, 8, 4, 2, 1 harmonics */

/* option flags (sy_synth.opt) */
#define SY_OPT_LATENCY 1u               /* faithful: note on -> sound after 2 blocks, the input route after 3 */
#define SY_OPT_CLEAN_PITCH 2u           /* clean: the pitch interpolated per sample instead of held per block */
#define SY_OPT_SOFTCLIP 4u              /* clean: a soft limiter instead of the hard clip at full scale */
#if defined(HYB_MULTI) && !defined(SY_UNI_CAP)
#define SY_UNI_CAP                      /* (the unison caps: synth.c uni_cap) */
#endif
#define SY_OPT_FXPIPE 8u                /* with SY_OPT_LATENCY and a firmware built with SY_FX_PIPE: the effects a block
                                         * later on the second core, the notes and the input route a block earlier: the
                                         * same latency, the first core freed of the effects */

typedef struct { float s1, s2; } sy_svf_t;

typedef struct {
    uint8_t stage;                      /* 0 idle, 1 attack, 2 decay, 3 sustain, 4 release */
    uint8_t fol;                        /* (0.92) sustain: the level follows a moved sustain level (env_step) */
    uint16_t sq;                        /* (0.92) sustain: the sustain level it last saw, x 65535 */
    float lvl;                          /* level at the end of the last block */
    float prev;                         /* level at its start */
    float knee;                         /* where in the block the attack reached full level (0..1; 1 = not) */
    float an, al0, alen;                /* amp attack: frames since its start (at the block's start), its start level,
                                         * the full ramp's frames */
} sy_env_t;

typedef struct {
    float ph;                           /* 0..1 */
    float out;                          /* value of this block, -1..1 (env mode: 0..2) */
    float sh_from, sh_to;               /* sample & hold / glide */
    uint32_t rng;
    uint8_t done;                       /* envelope mode: the cycle is over */
} sy_lfo_t;

typedef struct {
    float tab[SY_WAVE_LEVELS][SY_WAVE_N + 1];
    int16_t id;                         /* the wave in it, -1 none */
    int16_t stale;                      /* 1: its wave's bank record changed, to build again (sy_waves_stale) */
} sy_wtab_t;

typedef struct {
    uint8_t on;                         /* allocated (sounding or releasing) */
    uint8_t gate;                       /* key held (or held by the hold mode) */
    uint8_t key, vel;                   /* the MIDI key and velocity */
    uint8_t uni_i, uni_n;               /* unison: index, count */
    uint8_t fresh;                      /* started in this block */
#ifdef SY_UNI_CAP
    uint8_t uni_p;                      /* the unison count its note asked for (the level's reference: synth.c uni_level) */
#endif
    uint32_t age;
    float glide;                        /* the note now (portamento), semitones */
    float target;                       /* the note it glides to */
    float t_on;                         /* seconds since note on (LFO 3 fade) */
    uint32_t ph1, ph2, ph3, phs;        /* oscillator phases (osc 1, 2, 3, sub) */
    float lp_osc, lp_x, lp_sub;         /* the oscillators' top roll-off; the sub's smoothing */
    float click, click_sg, click_lp;    /* the oscillators' start transient (its position, sign), its DC removal */
    float dip1, dip2;                   /* the high keys' dip filter (transposed direct form II) */
    float p6[4];                        /* CLEANER: the plain oscillators' last two samples, osc 1 and 2 (sy_voice.c pl6) */
    float n_lp, n_hp, n_x;              /* noise colour */
    float nf_lp, nf_x;
    uint8_t lfo_pend;                   /* the free LFOs' start phase is decided at the first block */
#ifdef SY_UNI_CAP
    uint8_t ufade, ufb;                 /* a unison voice taken from its note, fading out (synth.c uni_fade); the blocks
                                         * left of its level's ramp */
#endif
    uint32_t lfo_step;                  /* (the tuning step's count: the private LFO start cycle) */
    float sync_hold;                    /* (sy_voice.c: samples osc 2's saw part still holds after a sync reset) */
    uint32_t sync_lag;                  /* (and its lag, in phase) */
    float lp_osc2, lp_x2;              /* split routing: osc 2's own top roll-off */           /* the same for the noise FM */
    uint32_t rng;
    sy_svf_t f1, f2a, f2b;
    float lad[4];                       /* analog modes */
    float sat_hold, sat_lp, sat_dc;     /* saturation stage state */
    float sat_cnt;
    sy_env_t aenv, fenv;
    sy_lfo_t lfo[3];
    float gl, gr;                       /* amp x pan gains at the end of the last block */
    float lad_a;                        /* the analog loop's coefficient at the end of the last block */
    uint32_t lad_rng;                   /* mode 7's noise */
    float afl, afr;                     /* the amp x pan without the envelope at the end of the last block (the attack's level) */
    float g1, h1, g2, h2;               /* filter coefficients at the end of the last block */
    float k1, k2, pg1, pg2;             /* resonance (2R) and passband gains (pg1, pg2: the last block's, sy_voice.c
                                         * vr_filt; k1, k2 unused) */
    float inc1f, inc2f;                 /* clean pitch: the increments of the last block */
    uint8_t started;                    /* the interpolation endpoints are valid */
    uint8_t used;                       /* the voice has sounded (free-running phases continue from its last) */
    uint8_t iph;                        /* the osc init phase of its note (B 35 + the matrix's 88 at its start;
                                         * in the padding: no RAM) */
    uint8_t pr;                         /* CLEANER (FILT KNOBS SMOOTH, synth.c flt_glide): this block the filters'
                                         * passband gains move from pg1, pg2 (the last block's) across the block (in
                                         * the padding: no RAM) */
    uint32_t rph1, rph2, rph3, rphs;   /* the phases at the release (synth.c note_on hand-over) */
    uint32_t hph[4][4];                 /* the phases at the starts of the last blocks */
    uint8_t solo;                       /* started with no other key held (synth.c note_on) */
    float du;                           /* the note's tuning step, -1 .. 1 (synth.c note_on) */
    float pwq1, pwq2;                   /* CLEANER: osc 1's, 2's duty less 0.5 at the last block (synth.c PW_PH) */
    float tk_c, tk_em;                  /* the oscillators' clock (its fraction past the last tick), the emphasis' last
                                         * input (sy_cln_osc.inc) */
    float tk_D1;                        /* osc 1's last restart's jump (0: none since a start) */
    float tk_xh[6];                     /* the last six ticks' naive mix (a start's or new weights' step from them) */
    float tk_g1, tk_g2, tk_w1, tk_w2, tk_s1, tk_s2, tk_c1, tk_c2, tk_f1, tk_f2;   /* (the weights it had, the widths) */
    uint32_t tk_d1, tk_d2;
    uint32_t tk_pe1, tk_pe2;            /* the phases the last block left (another: a start) */
#ifdef SY_UNI_CAP
    float ug, ugt;                      /* its note's unison level correction (synth.c uni_level): at the last block's
                                         * end, and where the ramp goes (0: the voice fades out) */
#endif
} sy_voice_t;
/* (a voice stolen within a note-on keeps for the phase hand-over (ptab_keep) only its phases, all it reads: 80 bytes
 * an entry instead of a whole voice's ~1 KB, 15 KB less memory) */
typedef struct { uint32_t hph[4][4], ph1, ph2, ph3, phs; } sy_steal_t;
#define SY_STEAL_PUT(d, v) do { sy_steal_t *d_ = (d); const sy_voice_t *v_ = (v); int i_, j_; \
    for (i_ = 0; i_ < 4; i_++) for (j_ = 0; j_ < 4; j_++) d_->hph[i_][j_] = v_->hph[i_][j_]; \
    d_->ph1 = v_->ph1; d_->ph2 = v_->ph2; d_->ph3 = v_->ph3; d_->phs = v_->phs; } while (0)

typedef struct {
    uint32_t at;                        /* the block it is due in */
    uint8_t st, d1, d2;
} sy_event_t;

/* a voice's block buffers, one set per core that renders voices (here, not on the stack: the audio interrupt's stack
 * is small and the second core's smaller) */
typedef struct {
    float xo[SY_BLOCK], xo2[SY_BLOCK], xn[SY_BLOCK], yl[SY_BLOCK], yr[SY_BLOCK];
    float o1b[SY_BLOCK], o2b[SY_BLOCK], o3b[SY_BLOCK], xs[SY_BLOCK], xt[SY_BLOCK], wrapf[SY_BLOCK];
    uint32_t ph2_0;                     /* osc 2's phase at the block's start in the tick path (sy_voice.c sub_ring) */
    const float *o2s;                   /* (P3_O2S hook) osc 2's samples of this block in the tick path when its own
                                         * oscillator (cln_osc) left them, else 0: sub_ring then renders them itself
                                         * (sy_voice.c; vr_osc clears it before cln_osc) */
} sy_vwork_t;
#ifndef SY_CORES
#define SY_CORES 1                      /* the cores that render voices (a set of buffers each) */
#endif
typedef struct {
    sy_vwork_t vw[SY_CORES];            /* the voices' block buffers, per core (sy_voice.c) */
    float vl[SY_BLOCK], vr[SY_BLOCK], in[SY_BLOCK];           /* the block's own (sy_render, sy_render_s16): the
                                         * voices' sum, the input, */
    float l[SY_BLOCK], r[SY_BLOCK], il[SY_BLOCK], ir[SY_BLOCK];   /* the output and the input in float */
} sy_work_t;
extern sy_work_t sy_work;               /* (one engine per program; where it lives: SY_WORK_ATTR, synth.c) */

typedef struct { float ph, r, t, n; } sy_dw_t;   /* (a detune wander: sy_synth's dw) */

typedef struct sy_synth {
    uint8_t n_on;                       /* the voices on at this block's start (synth.c VC6_GAIN) */
    uint8_t v6;                         /* CLEANER: the 6-voice state this block (synth.c v6_on, V6_LOAD) */
    uint8_t part_snd;                   /* a voice is on or an input route is used (the phaser's LFO runs only
                                         * then, sy_fx.c) */
    uint8_t keep_prev;                  /* the arp's releases hand over the phases of an earlier block */
    uint8_t arp_marked;
    uint32_t arp_mark;                  /* the block whose phases the arp's release hands over */
    float fs, ifs, bdt;                 /* rate, 1/rate, seconds per block */
    uint32_t opt;
    float osc_clean;                    /* osc 1 and 2: 0 ORIGINAL (the default: as measured) .. 1 CLEAN (steps at their own
                                         * time, less aliasing; sy_cln_osc.inc) */
    int max_voices;                     /* voice limit (<= SY_MAX_VOICES); a governor may lower it */
    uint8_t patch[256];
    float pv[256];                      /* parameter values as the engine sees them (controller smoothing) */
    uint8_t cc[128];                    /* controller values (matrix sources) */
    float ccs[128], cc_inc[128];        /* the same smoothed (B 25 odd), and their glide's step per block */
    uint8_t cc_tgt[128], cc_seen[128];  /* the glide's target; the controller has moved since the patch load */
    float sm_inc[256];                  /* the parameters' glide: step per block, target, moved since the load */
    uint8_t sm_tgt[256], sm_seen[256];
    uint8_t sm_dirty;                   /* a parameter or a controller changed: the smoothing runs until all settle */
    uint8_t flt_sm;                     /* CLEANER (GLO > AUDIO > FILT KNOBS): 0 ORIGINAL (the default: a changed
                                         * cutoff or resonance steps at the block's start), 1 SMOOTH (its base value
                                         * glides: synth.c flt_glide) */
    uint8_t fs_act;                     /* (flt_glide runs: a base value of the filters' may still glide) */
    uint16_t fs_t[4];                   /* (flt_glide: the block (low 16 bits) of cutoff 1, 2, resonance 1, 2's last new
                                         * target) */
    float bend, bend_t;                 /* -1..1: the bend (smoothed) and its target */
    uint8_t bend_seen;                  /* the bend has moved since the patch load */
    float pressure;
    uint32_t block;                     /* blocks rendered */
    uint32_t ages;
    uint32_t age_mark;                  /* the voices' age count at this block's start (note-offs after the ons) */
    uint8_t kcount[128];                /* mono: a key's note-ons not yet matched by note-offs */
    uint32_t notes;                     /* note-ons since load (the tuning steps' counter) */
    uint32_t prng[4], prng_pend;        /* (the phase hand-over's random generator: prng[0], synth.c PHASE_TAB) */
    struct { int32_t st; uint32_t p1, p2, p3, ps; } ptab[16];   /* (its table of phases kept at release) */
    int32_t pcnt;                       /* (its stamp counter) */
    int steal_n;
    int loaded;                         /* a patch has been loaded */                        /* held voices taken during a note-on (kept after its voices start) */
    sy_event_t evq[SY_EVQ];
    uint32_t ev_w, ev_r;
    uint8_t held[128];                  /* keys held (mono modes: the order) */
    uint8_t stack[16];                  /* mono: held keys, last on top */
    uint8_t nstack;
    uint8_t sustain_hold;               /* (1.19.2) the sustain pedal (CC 64) down: synth.c sustain */
    float last_note;                    /* portamento start */
    sy_lfo_t glfo[3];                   /* LFOs in mono mode */
    float clock_ph;                     /* arpeggiator */
    int arp_i, arp_dir, arp_note, arp_on, arp_chord;
    int arp_pk, arp_pv, arp_pc, arp_po;                   /* an arp note waiting for its onset (blocks) */
    float arp_clk, arp_wait;            /* the part's free tick clock (1/48 beat, its fraction); the first step's wait */
    float arp_t, arp_end;               /* pattern 0: seconds in the step; patterns: ticks, the gate's end */
    int arp_k;                          /* patterns: the next note */
    float arp_lt;                       /* seconds since the arp (re)started (the clocked LFOs' phase) */
    int xclk_on;                        /* MIDI clock: running (since a start or the first tick) */
    uint32_t xclk_ticks;                /* ticks since the start */
    float xclk_frames, xclk_per;        /* frames since the last tick, frames per tick (smoothed) */
    float xclk_arp;                     /* the arpeggiator's own frames per tick (gliding in after a start) */
    uint32_t xclk_n;                    /* ticks measured since the start */
    float bpm_now;                      /* this block's tempo (the clock's or the patch's) */
    uint8_t arp_vel[128];               /* the keys the arpeggiator holds (velocity) */
    uint8_t arp_ord[16], arp_n;         /* the same in the order played */
    uint8_t arp_chord_k[128];           /* the chord mode's sounding keys */
#ifndef HYB_MULTI
    sy_voice_t v[SY_MAX_VOICES];
    sy_steal_t steal[16];
#else                                   /* two parts (HYB_MULTI): the voices a slice of the shared pool, sy_multi_t */
    sy_voice_t *v;                      /* the part's voices: sh->v + v0, nv of them (max_voices <= nv) */
    sy_steal_t *steal;                  /* (shared: used within one note-on) */
    int nv, v0;
    uint8_t pidx;                       /* 0 part A (it owns the delay / reverb), 1 part B */
    float *sat_lut;                     /* the saturation curve of types 1-4 (A: sy_sat_lut) */
    int arp_held_m;                     /* (synth.c's arp_held, cc_dirty, pv_gl: one per part) */
    uint8_t cc_dirty_m;
    uint32_t pv_gl_m[8];
    int16_t send;                       /* part B: its send to A's delay / reverb, -1 its patch's (113) */
    uint8_t mx_on;                      /* part A: B sends this block (sy_fx.c) */
    float mx_wb;                        /* part A: B's send (the send law's wet) */
    float *mx_bl, *mx_br;               /* B's signal into the space (after its chorus, sy_fx.c) */
    float rt_g;                         /* the player's part volume (the app's SHIFT + ALGO, VOL A / B CC): a gain on
                                         * part_g, 1 as initialised (x 1: the output to the bit) */
    uint8_t mx_bz;                      /* part B at rest: its send into A's space is held at 0 (sy_fx_quiet) */
    uint8_t fx_rmx;                     /* part A at rest: B's send as the rest began (mx_on, mx_wb; sy_fx_rest_begin) */
    float fx_rwb;
#endif
    uint8_t mx_ovr;                     /* (part A: the space takes another signal too, B's or the drums': its dry 0,
                                         * its wet mx_w; sy_fx.c sy_fx_render) */
    int16_t dry_off;                    /* the matrix's offset on the dry level (B 58, destination 113), as rm_off */
    float mx_w;
    /* a firmware's drum machine sending into part A's delay / reverb (the FM-1 app's DRUM KIT SEND; the
     * mixing as B's send). dr_w 0 (as initialised): nothing of it runs */
    volatile float dr_w;                /* the drums' send: the send law's wet of the largest instrument's SEND */
    const float *dr_b;                  /* their signal, L (SY_BLOCK frames) then R, in the space's units, each
                                         * instrument at its own wet (the space takes it at 1 / its output gain) */
    volatile uint8_t dr_on;             /* dr_b holds a signal for this block (else it is not read: zeros) */
    volatile uint8_t fx_wake;           /* a patch byte set or a patch loaded since the render looked (part B's rest,
                                         * synth.c render2; the effects' rest, sy_fx_rest_begin: they run again) */
    uint8_t fx_rest;                    /* the effects at rest: their blocks skipped (sy_fx.c sy_fx_rest_*) */
    uint8_t fx_rc, fx_rz;               /* (this block checked; the last one's output all 0, its states kept) */
    int16_t fx_rn;                      /* (blocks in a row that changed nothing; below 0 a wait after a look) */
    sy_voice_t stat;                    /* input mode 2: the input through the filters, no note */
    float fxm[SY_FXN];                  /* the matrix's offsets on the part's effects (A 105-119 at [0..14], the page B
                                         * effect bytes of sy_fx.c SY_FXB at [15..26]): those of
                                         * the voice started last, held when it ends, 0 after a patch load (synth.c
                                         * voice_block; sy_fx.c FXP) */
    uint8_t fx_mod;                     /* (one of fxm not 0) */
    uint8_t fx_tail, fx_vel, fx_key;    /* that voice has ended: fx_tail runs on its LFOs (fx_lfo), key, velocity */
    sy_lfo_t fx_lfo[3];
    float fxv[SY_FXN];                  /* the effects' parameters this block: pv + fxm, A 105-119 0..128, page B 0..127
                                         * (sy_fx.c fx_vals) */
#ifdef HYB_SHIMMER
    uint8_t shx_rt[12];                 /* (0.89, FX > SHIMMER) the shimmer's targets of the matrix's
                                         * nine destinations, LFO 1's and 2's assigns and LFO 3's (0 none, 1 AMOUNT,
                                         * 2 MODULATION (1.18C45; TONE before); 3, 4 kept): the app's (the patch's
                                         * extension; the patch's own destination byte off there) */
    float shm[3];                       /* their offsets (AMOUNT, MODULATION) as the effects' (fxm): the voice started
                                         * last, held, 0 after a patch load; [2] (1.18C45) LFO 3 on SHIM MOD: its
                                         * output x its amount / 127 (-1..1), the pitch's modulator; sy_fx.c hands
                                         * them to the reverb */
#endif
    int channel;                        /* MIDI channel 0..15, -1 = omni */
    uint8_t sat_type_built;
    uint8_t pv_jump;                    /* a controller set a value this block: no interpolation */
    int16_t rm_off;                     /* the matrix's offset on the input ring modulator (B 99, destination 96): the
                                         * voice started last's, as fxm, in 1/256 steps (in the padding: no RAM) */
    float sat_drive_built;
    sy_wtab_t wt[3];                    /* waves of osc 1, 2, 3 */
    /* effects */
#ifndef HYB_MULTI
    float ch_buf[2][1024 * 2];          /* chorus (up to 48 kHz) */
#else
    float (*ch_buf)[1024 * 2];          /* (sy_multi_init: a part's own, where the firmware puts it) */
#endif
    uint32_t ch_w;
    float ch_ph, ch_u;
    int ch_hold;                        /* frames before the chorus LFO starts */
    float ch_sh_from, ch_sh_to;
    uint32_t ch_rng;
    float ch_fb[2];
#ifndef HYB_MULTI
    union {                             /* (the delay's memory is the reverb's ring too) */
        struct {
            int16_t dl_buf[SY_DELAY_FRAMES];    /* the delay's loop (both outputs tap it) */
            float rv_buf[4096];                 /* (the earlier network's lines: unused, the union's size kept) */
        };
        float rvx_ring[32768 / 32 * HYB_RV_BITS];   /* the reverb's ring (modes 2..4) */
    };
#else
    int16_t *dl_buf;                    /* (part A's: sy_shared_t's, B never runs a delay or reverb) */
    float *rv_buf, *rvx_ring;
#endif
    uint32_t dl_w;
    float dl_ph, dl_lp[2], dl_hp[2], dl_fbv[2];
    uint8_t fx_mode_last;
    float part_g, part_g_prev, dry_g;                     /* patch x part volume after the distortion */
    float ds_hp[2], ds_lp[2], ds_hold[2], ds_cnt, ds_ph[2];   /* the distortion: filter states, the rate reducer's hold */
    float rv_state[16];                 /* reverb: filter states */
    int32_t rv_pos[8], rv_len[8], rv_off[8];
    float ph_ap[2][12], ph_lfo, ph_fb[2];
    float eq_z[2][4][2];                /* EQ biquads: low shelf, mid, high shelf, boost */
    float eq_c[4][5];
    uint8_t eq_key[10];
    uint8_t eq_on;
    float in_dl[1][SY_BLOCK * 4];       /* the input route's latency (3 blocks) */
    float vol_prev;
#ifdef SY_UNI_CAP                       /* (HYB_MULTI sets it; a firmware may: 69 bytes) */
    uint8_t uni_max, uni_mono;          /* the unison voices a note takes at most (poly, mono patches); 0 the patch's */
    uint8_t uni_auto;                   /* the poly cap steps down as the part's chord grows (synth.c uni_cap) */
    uint8_t uni_steal;                  /* stealing: an extra unison voice of a held note before a whole note
                                         * (synth.c uni_victim), faded out while the note's other voices take its level */
    uint8_t upn;                        /* the notes with unison voices waiting for a voice that fades (uni_pend) */
    struct {
        uint8_t key, vel, n, np, alone, wait;   /* the note; its unison count, the patch's; struck alone; blocks waited */
        uint16_t mask;                  /* its unison slots still to start */
        uint32_t n0;                    /* its tuning step */
        float from;                     /* its glide's start */
    } upend[4];
#endif
#ifndef HYB_MULTI
    float tables[24][128];              /* per-value curves (sy_tables.c) */
    float cmap[257];                    /* cutoff map: log2(Hz) for steps -64..192 */
#else
    float (*tables)[128];               /* (sy_shared_t's: the same for both parts) */
    float *cmap;
#endif
    /* (0.91, at the end: the fields before keep their offsets, the RAM code its size) the effects' parameters A 105-119
     * the block before (fxv's): the chorus and the delay ramp from these to fxv over a block in which one of theirs
     * moved (sy_fx.c fx_vals, fx_rmp) */
    float fxp[15];
    uint8_t fx_snap, fx_rmp;            /* (fxp = fxv at the next block: a patch load, a reset; this block's ramps:
                                         * 1 the chorus, 2 the delay) */
    /* (at the end too) CLEANER: the detune's wanders (synth.c DET_WANDER): the part's first voice's and the one all its
     * other voices share; each a triangle's phase (0..1), rate and target rate (Hz), the seconds to the next target */
    sy_dw_t dw[2];
    float dw_w[2];                      /* (their values this block, of the nominal amount) */
    uint8_t ped_k[16];                  /* (1.19.2, at the end too) the arpeggiator's keys let go under the sustain
                                         * pedal (a bit each; synth.c sustain) */
} sy_synth;

void sy_init(sy_synth *s, float fs);
void sy_load_patch(sy_synth *s, const uint8_t *patch);
void sy_set_param(sy_synth *s, int index, int value);       /* a patch byte, 0..255 */
void sy_set_param_queued(sy_synth *s, int index, int value);   /* the same through the event queue (safe while an
                                                                * interrupt renders) */
void sy_midi(sy_synth *s, uint8_t status, uint8_t d1, uint8_t d2);
void sy_render(sy_synth *s, float *out_l, float *out_r, const float *in_l, const float *in_r);
void sy_all_off(sy_synth *s);
int sy_voices_active(const sy_synth *s);
int sy_note_voices(const sy_synth *s);   /* (after 1.18C34) the voices a note struck alone takes now (the patch's unison,
                                         * the caps: UNISON MAX / MONO UNI MAX), for a firmware's governor floor */
int sy_shed(sy_synth *s, int n, int nb);   /* (the overload guard's: n voices faded out over nb blocks, the released
                                                 * ones first, the quietest first; the count taken) */
extern volatile uint8_t sy_bud;           /* (synth.c, 1.18C49) a firmware's budget acts: no voice over the cap */
extern volatile uint8_t sy_start_budget;   /* (synth.c START_SPREAD) the voice starts a block takes before its further
                                            * note-ons wait for the next block (both parts together; 0: no limit) */
/* user waves: the app's bank replaces factory waves 2-63. sy_user_wave(id) gives id's record or 0
 * (the factory wave); a record is SY_UWAVE_LEN bytes: a name of SY_UW_NAMES characters, a level trim (int8, 0.1 dB:
 * the wave is set to the factory waves' level, its peak at full scale, then trimmed), a flags byte (0), then 64
 * harmonics of 4 bytes each, little end: the amplitude as an IEEE half float, the phase in 1 / 65536 of a cycle
 * (y = sum a_n sin 2 pi (n t + phase_n)). It is read only while a wave table is built (a patch load, a wave change,
 * sy_waves_rebuild) */
#define SY_UW_NAMES 10
#define SY_UW_TRIM 10
#define SY_UW_HARM 12
#define SY_UWAVE_LEN (SY_UW_HARM + 64 * 4)
extern const uint8_t *(*sy_user_wave)(int id);
void sy_waves_harm(int id, float *amp, float *phs);   /* factory wave id's (2..63) harmonics: amp[1..64] at the level
                                         * its table holds, phs[1..64] in cycles */
void sy_waves_stale(sy_synth *s, int id);   /* the oscillator tables holding wave id (its bank record changed) to build
                                         * again; they play on as they are until then */
int sy_waves_rebuild(sy_synth *s, int id);   /* one of those built again: 1, or 0 when none is left. Between blocks, as
                                         * a patch load (a patch load or a wave change builds them too) */
/* integer interface (no float in the caller): the rate in Hz; a block of frames as L | R << 16, int16 each */
void sy_init_hz(sy_synth *s, uint32_t hz);
void sy_render_s16(sy_synth *s, uint32_t *frames, const uint32_t *in);
int sy_render_i24(sy_synth *s, int32_t *lr, float g);    /* interleaved int32, clipped then x g (a 24-bit output);
                                                           * 1: the block is zeros (the effects at rest), known */
/* the second core (SY_CORES 2): run(fn, arg) hands it a job (1 = taken, 0 = do it yourself), wait() waits for it
 * (0 = done, else it was given up and the job dropped); the voices are then shared between the cores, each block */
void sy_set_cpu2(int (*run)(void (*fn)(uint32_t), uint32_t arg), int (*wait)(void));

#ifdef HYB_MULTI
/* Two parts (HYB_MULTI, the FM-1's USB firmware): parts A and B, each a sy_synth with its own
 * patch, control (LFOs, matrix, arpeggiator, glide, smoothing), wave tables, saturation curve and effects (distortion,
 * chorus, phaser, EQ); the voices one pool of SY_MAX_VOICES, a slice each; the delay / reverb one, part A's (its
 * patch sets it), B's signal sent into it. SINGLE renders part A alone with all the voices, as without HYB_MULTI. */
typedef struct {                        /* the parts' shared memory */
#ifndef HYB_RV_LEND
    sy_voice_t v[SY_MAX_VOICES];        /* the voice pool: A's slice first, then B's */
    sy_steal_t steal[16];
#endif
    union {                             /* part A's delay or reverb (as the single engine's; HYB_RV_LEND: first, so the
                                         * memory below it can make the reverb's ring 128 KB) */
        struct {
            int16_t dl_buf[SY_DELAY_FRAMES];
        } d;
        float rvx_ring[32768 / 32 * HYB_RV_BITS];   /* the reverb's ring (modes 2..4) */
    } sp;
#ifdef HYB_RV_LEND
    sy_voice_t v[SY_MAX_VOICES];
    sy_steal_t steal[16];
#endif
    float tables[24][128];
    float cmap[257];
} sy_shared_t;
enum { SY_M_SINGLE, SY_M_LAYER, SY_M_SPLIT, SY_M_CHANNELS, SY_M_N };
typedef struct {
    sy_synth *p[2];                     /* part A, part B */
    sy_shared_t *sh;
    /* the setup (written by the caller between blocks, or from another context: taken at a block's start) */
    volatile uint8_t mode;              /* SY_M_*: SINGLE part A alone; LAYER both on every key; SPLIT B below the
                                         * split key, A from it up; CHANNELS the keys A, B on its channel only */
    volatile uint8_t split;             /* the split key (B below it) */
    volatile int8_t ch_b;               /* part B's MIDI channel 0..15 (not SINGLE: its messages go to B alone), -1 none */
    volatile uint8_t nv_a;              /* part A's voices of the pool (not SINGLE), 1 .. SY_MAX_VOICES - 1 */
    volatile int16_t send_b;            /* B's send into A's delay / reverb, 0..127, -1 B's patch's (byte 113) */
    volatile int16_t max_voices;        /* the voices in all (a governor's, under the player's cap), split between the
                                         * parts by nv_a (FIXED) or shared (DYNAMIC) */
    volatile uint8_t dyn;               /* DYNAMIC: one pool up to max_voices, a part takes what the other leaves (each
                                         * keeps its share when both want more: the one over its share is stolen from) */
    volatile uint8_t uni_max, uni_mono, uni_auto, uni_steal; /* both parts' unison caps and stealing (UNISON FIRST) (poly patches, mono patches), 0 none: the effective
                                         * unison count at a note's start, the patch unchanged (also SINGLE) */
    /* the engine's */
    uint8_t mode_now, nv_now, dyn_now;  /* the setup rendered */
    int16_t cap_now;                    /* the voices in all now (max_voices, clamped) */
    uint8_t kp[128];                    /* the parts a key's note-on went to (bit 0 A, bit 1 B): its note-off goes there */
} sy_multi_t;
/* sh, the parts, B's chorus buffer and B's saturation curve (SAT_N + 1 floats) where the caller keeps them; A's chorus
 * buffer too (the single engine's is in sy_synth). Both parts initialised; the setup SINGLE */
void sy_multi_init(sy_multi_t *m, sy_synth *a, sy_synth *b, sy_shared_t *sh, float (*ch_a)[2048], float (*ch_b)[2048],
                   float *sat_b, float fs);
void sy_multi_midi(sy_multi_t *m, uint8_t status, uint8_t d1, uint8_t d2);   /* a MIDI message by the setup (part A's
                                         * channel; part B's channel: B alone); a part's own: sy_midi on it */
void sy_multi_play(sy_multi_t *m, uint8_t status, uint8_t d1, uint8_t d2);   /* the same from the panel's keys (no
                                         * channel: B's channel not looked at) */
void sy_multi_render(sy_multi_t *m, float *out_l, float *out_r, const float *in_l, const float *in_r);   /* (the input
                                         * goes to part A only) */
int sy_multi_render_i24(sy_multi_t *m, int32_t *lr, float g);
int sy_multi_voices_active(const sy_multi_t *m);
#endif

#endif
