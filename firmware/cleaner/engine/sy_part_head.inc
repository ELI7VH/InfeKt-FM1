/* SPDX-License-Identifier: GPL-3.0-only */
/* (synth.c: a part's block before its voices, in sy_render and, two parts, part_head) its events (note-ons first),
 * the arpeggiator, the smoothing, the bend, the part's LFOs, the input and its route's latency: sets in, the input the
 * voices take. In scope: s, in_l, in_r, in_mode, insel (sy_work.in), in, i */
#if START_SPREAD
#ifdef HYB_MULTI
    if (!s->pidx) spr_used = 0;         /* (a block's starts: both parts', A's head first) */
#else
    spr_used = 0;
#endif
    {
    const uint32_t a0 = s->ages;        /* (uni_pend's starts count too) */
#endif
#ifdef SY_UNI_CAP
    if (s->upn) uni_pend(s);            /* (unison voices waiting for a voice that faded: synth.c uni_pend) */
#endif
    {   /* a block's note-ons are taken before its note-offs, whatever their order (read: an off and an on in
         * one block start the new voice first; the off then ends the voices that were there before) */
        sy_event_t offs[32];
        uint8_t pre[32];                /* CLEANER (1.18C34 fix): offs[q] came before a note-on of its key (off_pre), */
        uint32_t mk[32];                /* the voices' age count where it was queued (off_lim) */
        int no = 0, q;
        s->age_mark = s->ages;
        while (s->ev_r != s->ev_w && (int32_t)(s->evq[s->ev_r % SY_EVQ].at - s->block) <= 0) {
            const sy_event_t *e = &s->evq[s->ev_r % SY_EVQ];
            int ty = e->st & 0xF0;
#if START_SPREAD
            if (ty == 0x90 && e->st < 0xF0 && e->d2 && sy_start_budget && spr_used + (s->ages - a0) > 0u &&
                (int32_t)(s->block - e->at) < START_LATE_MAX && s->ev_w - s->ev_r <= SY_EVQ / 2u &&
                spr_used + (s->ages - a0) >= SY_FAR(spr_lim)(s)) {
                SY_FAR(spr_rt)(s);      /* (this note-on and the rest wait for the next block: START_SPREAD) */
                break;
            }
#endif
            if (EV_ONS_FIRST && e->st < 0xF0 && (ty == 0x80 || (ty == 0x90 && !e->d2)) && no < 32) { pre[no] = 0; mk[no] = s->ages; offs[no++] = *e; }
            else {
                if (ty == 0x90 && e->d2 && e->st < 0xF0)   /* (a note-on: the offs of its key queued before it are the old note's) */
                    for (q = 0; q < no; q++)
                        if (offs[q].d1 == e->d1 && ((offs[q].st ^ e->st) & 0x0F) == 0) pre[q] = 1;
                apply_event(s, e);
            }
            s->ev_r++;
        }
        for (q = 0; q < no; q++) { off_pre = pre[q]; off_lim = mk[q]; apply_event(s, &offs[q]); }
        off_pre = 0;
    }
#if START_SPREAD
    spr_used += s->ages - a0;
    }
#endif
    {   /* the part's tick clock runs from load */
        float c = s->arp_clk + s->bdt * arp_bpm(s) * (48.f / 60.f);
        s->arp_clk = c - (float)sy_floori(c);
    }
    if (s->patch[B_ARP]) { arp_step(s); s->arp_lt += s->bdt; }
#ifdef PTAB_DEBUG
    { int q, nv = 0; for (q = 0; q < s->max_voices; q++) nv += s->v[q].on ? 1 : 0; fprintf(stderr, "vc %u %d\n", s->block, nv); }
#endif

    /* controller smoothing (B 25 odd) */
    if (s->sm_dirty) {                  /* (only while something changed and has not settled: sy_set_param, a CC, a load) */
        int left = 0;
        if (s->patch[B_SMOOTH] & 1u) {
            int w;                      /* (the entries that may glide only: pv_gl; each glides on its own) */
            for (w = 0; w < 8; w++) {
                uint32_t bits = pv_gl[w], keep = 0u;
                for (i = w << 5; bits; i++, bits >>= 1)
                    if ((bits & 1u) && s->pv[i] != (float)s->patch[i]) {
                        smooth_step(s, i);
                        if (s->pv[i] != (float)s->patch[i]) { left = 1; keep |= 1u << (i & 31); }
                    }
                pv_gl[w] = keep;
            }
            if (cc_dirty) {             /* (ccs and cc differ only after a CC under the smoothing: no scan otherwise) */
                int cl = 0;
                for (i = 0; i < 128; i++)   /* the matrix's controller sources glide too (read with CC 1 on the pan) */
                    if (s->ccs[i] != (float)s->cc[i]) {
                        s->ccs[i] = glide(s->ccs[i], (float)s->cc[i], &s->cc_inc[i], &s->cc_tgt[i], s->cc[i], s->bdt);
                        cl |= s->ccs[i] != (float)s->cc[i];
                    }
                cc_dirty = (uint8_t)cl;
                left |= cl;
            }

        } else {
            float fk[4];                /* (FILT KNOBS SMOOTH: the filters' base values gliding, flt_glide) */
            if (s->fs_act) for (i = 0; i < 4; i++) fk[i] = s->pv[CUT1 + i];
            for (i = 0; i < 256; i++) s->pv[i] = (float)s->patch[i];
            for (i = 0; i < 128; i++) s->ccs[i] = (float)s->cc[i];
            for (i = 0; i < 8; i++) pv_gl[i] = 0u;
            if (s->fs_act) for (i = 0; i < 4; i++) { s->pv[CUT1 + i] = fk[i]; PV_GL(CUT1 + i); }   /* (pv_gl: the smooth
                                         * mode's glide goes on from there when it is turned on) */
        }
        s->sm_dirty = (uint8_t)left;
    }
    if (s->fs_act) s->fs_act = (uint8_t)flt_glide(s);   /* (CLEANER: FILT KNOBS SMOOTH, synth.c; the smooth mode's
                                         * own glide takes over when it is on) */
    if ((s->patch[B_SMOOTH] & 1u) && s->bend != s->bend_t) {
        /* the bend glides at a fixed speed (read on the pitch: the centre to either end in 2000 frames) */
        float st = s->bdt * (REF_FS / 2000.f);
        s->bend += sy_clampf(s->bend_t - s->bend, -st, st);
    } else s->bend = s->bend_t;
    /* the part's LFOs (mono mode, and the free-running phase new voices start from) */
    {
        static const int RATE[3] = {L1_RATE, L2_RATE, B_L3_RATE}, CLK[3] = {B_CLK_L1, B_CLK_L2, B_CLK_L3};
        for (i = 0; i < 3; i++) {
            lfo_advance(&s->glfo[i], SY_FAR(lfo_hz)(s, s->pv[RATE[i]], s->patch[CLK[i]], 0, 60.f), s->bdt, 0);
        }
    }

    /* the input: select, the input ring modulator (B 99), the route's latency */
    {
        int sel = s->patch[IN_SEL], rm = s->patch[PB + 99];
        float rmw = (rm == 0 || rm == 127) ? 0.f : 1.f - sy_absf((float)rm - 64.f) / 64.f;
        /* the input's share falls as a cosine (fx.json eq_etc B99: 0.87 / 0.7 / 0.42 at 16 / 32 / 48); the product
         * x -8 (with the route's -0.857: +6.9, measured) is already full at 32 (measured: the 2 kHz
         * line as at 64). ASSUMPTION: the product's share sqrt(2) sin, clamped at 1 */
        float rm_d = sy_cos2pi(0.25f * rmw), rm_p = -8.f * sy_minf(1.f, 1.41421f * sy_sin2pi(0.25f * rmw));
        float *dl = s->in_dl[0];
#ifdef HYB_MULTI
#define insel_z (insel_z_m[s->pidx])   /* (two parts: each its own insel, synth.c) */
#else
        static int insel_z = 1;         /* insel is all 0 (no input since the last one) */
#endif
        if (!in_l && !in_r && rmw == 0.f && insel_z) ;   /* (nothing to do: every x would be 0) */
        else
        for (i = 0; i < SY_BLOCK; i++) {
            float l = in_l ? in_l[i] : 0.f, r = in_r ? in_r[i] : 0.f, x;
            /* measured: 0 the left, 2 the right, 1 both sides (stereo in the reference, +1.5 dB;
             * here their mono sum: a deliberate difference) */
            x = sel == 2 ? r : sel == 1 ? 0.5f * (l + r) : l;
            /* MEASURED (441 Hz left, 1 kHz right): the product is the selected input's square (select
             * 0: 882 Hz and DC, 2: 2 kHz and DC; the stereo select each side's own), not left x right */
            if (rmw != 0.f) x = rm_d * x + rm_p * x * x;
            insel[i] = x;
            insel_z = !in_l && !in_r && rmw == 0.f;
        }
        /* the route through the filters / to the effects arrives 3 blocks later (384 frames); the voice path's
         * input has the voices' latency already */
        if ((s->opt & SY_OPT_LATENCY) && in_mode >= 1) {   /* all input routes: 384 frames (rate.json: dc onset) */
            for (i = 0; i < SY_BLOCK * 3; i++) dl[i] = dl[i + SY_BLOCK];
            for (i = 0; i < SY_BLOCK; i++) dl[3 * SY_BLOCK + i] = insel[i];
            in = SY_PIPE_ON(s) ? dl + SY_BLOCK : dl;   /* (the effects' block later: one less here) */
        } else in = insel;
    }
#ifdef HYB_MULTI
#undef insel_z
#endif
