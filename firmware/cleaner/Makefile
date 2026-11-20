# SPDX-License-Identifier: GPL-3.0-only
# The cleaner's host build of the engine: build/engine, the lab's renderer (tools/lab/lab.py's interface).
CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Wno-unused-parameter -ffp-contract=off
ENGINE = engine/synth.c engine/sy_voice.c engine/sy_tables.c engine/sy_fx.c engine/sy_waves.c engine/sy_dist_tab.c engine/sy_arp_tab.c engine/sy_hyb_lfow.c

# The reverb of modes 2-4: the own one (engine/sy_cln_rv.c, PROVENANCE.md)
RV_SRC = engine/sy_cln_rv.c

build/engine: host/lab_engine.c $(ENGINE) engine/*.h engine/*.inc $(RV_SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) $(RV_FLAGS) -Iengine -o $@ host/lab_engine.c $(ENGINE) $(RV_SRC) -lm

# make sweep: host/rv_sweep.c with ASan and UBSan, the single build and the two-part builds (16-bit ring; lent float ring)
SAN = -O1 -g -std=c99 -fsanitize=address,undefined -fno-sanitize-recover=all -ffp-contract=off -Iengine
sweep: host/rv_sweep.c $(ENGINE) engine/*.h engine/sy_cln_rv.c
	@mkdir -p build
	$(CC) $(SAN) -o build/rv_sweep host/rv_sweep.c $(ENGINE) engine/sy_cln_rv.c -lm
	$(CC) $(SAN) -DHYB_MULTI -o build/rv_sweep_m host/rv_sweep.c $(ENGINE) engine/sy_cln_rv.c -lm
	$(CC) $(SAN) -DHYB_MULTI -DHYB_RV_LEND -o build/rv_sweep_ml host/rv_sweep.c $(ENGINE) engine/sy_cln_rv.c -lm
	build/rv_sweep && build/rv_sweep_m && build/rv_sweep_ml

# make spread: host/spread_test.c (synth.c START_SPREAD: a big chord's voice starts spread over blocks), the plain
# engine and the firmware's (two parts, the unison caps)
spread: host/spread_test.c $(ENGINE) engine/*.h engine/*.inc engine/sy_cln_rv.c
	@mkdir -p build
	$(CC) $(CFLAGS) -Iengine -o build/spread_test host/spread_test.c $(ENGINE) engine/sy_cln_rv.c -lm
	$(CC) $(CFLAGS) -Iengine -DSY_MAX_VOICES=16 -DHYB_MULTI -DSY_UNI_CAP -o build/spread_test_m host/spread_test.c $(ENGINE) engine/sy_cln_rv.c -lm
	build/spread_test && build/spread_test_m

# make shed: host/shed_test.c (synth.c sy_shed, SY_SHED_KEEP: the overload guard keeps each held note a voice;
# sy_note_voices), the firmware's engine (two parts, the unison caps); shed_test_old: SY_SHED_KEEP 0, the old order
shed: host/shed_test.c $(ENGINE) engine/*.h engine/*.inc engine/sy_cln_rv.c
	@mkdir -p build
	$(CC) $(CFLAGS) -Iengine -DSY_MAX_VOICES=16 -DHYB_MULTI -DSY_UNI_CAP -o build/shed_test host/shed_test.c $(ENGINE) engine/sy_cln_rv.c -lm
	build/shed_test

# make bud: host/bud_test.c (1.18C49: the budget's engine side, synth.c sy_bud, and its early fades through sy_shed),
# the firmware's engine (two parts, the unison caps), an invented heavy patch
bud: host/bud_test.c $(ENGINE) engine/*.h engine/*.inc engine/sy_cln_rv.c
	@mkdir -p build
	$(CC) $(CFLAGS) -Iengine -DSY_MAX_VOICES=16 -DHYB_MULTI -DSY_UNI_CAP -o build/bud_test host/bud_test.c $(ENGINE) engine/sy_cln_rv.c -lm
	build/bud_test -v

clean:
	rm -rf build

.PHONY: clean sweep spread shed bud fknob rvlf tail shimtest shimwidth

# make rvlf: the reverb LF path's interpolation images (sy_cln_rv.c RVLF_SPLINE) on build/engine, FULL and HALF at
# 44.1 and 46.875 kHz (tools/measure/reverb/rvlf_images.py: fails on a line above the tail's floor)
rvlf: build/engine
	python3 tools/measure/reverb/rvlf_images.py build/engine

# make fknob: host/fknob_test.c (GLO > AUDIO > FILT KNOBS: synth.c flt_glide), the plain engine and the firmware's
fknob: host/fknob_test.c $(ENGINE) engine/*.h engine/*.inc engine/sy_cln_rv.c
	@mkdir -p build
	$(CC) $(CFLAGS) -Iengine -o build/fknob_test host/fknob_test.c $(ENGINE) engine/sy_cln_rv.c -lm
	$(CC) $(CFLAGS) -Iengine -DSY_MAX_VOICES=16 -DHYB_MULTI -DSY_UNI_CAP -o build/fknob_test_m host/fknob_test.c $(ENGINE) engine/sy_cln_rv.c -lm
	build/fknob_test && build/fknob_test_m

# make tail: the tails' rest (sy_cln_rv.c RVF_GATE): the lab's renderer before (-DRVF_GATE=0) and
# after, single (float ring), two parts with the lent float ring / the 16-bit ring, two parts with a 32-bit ring;
# tools/perf/tailtest.py: the effects rest by each case's time, the difference at most -92 dBFS, nothing differs
# before the rest, the paths not changed bit-identical; (1.18C41) the 16-bit ring's fade (RVQ_FADE) against 1.18C40 (m_c40)
# and an engine without its gate (m_ng), HALF on the float ring (RV_HALF_F) against the 32-bit build's HALF
TAILB = build/tail
TAILM = -DSY_MAX_VOICES=16 -DSY_UNI_CAP -DHYB_MULTI
C40 = -DRVQ_FADE=0 -DRV_HALF_F=0 -DRVQ_PDZ=0   # (1.18C41's switches off: the 16-bit ring and HALF as 1.18C40)
tail: host/lab_engine.c $(ENGINE) engine/*.h engine/*.inc engine/sy_cln_rv.c
	@mkdir -p $(TAILB)
	$(CC) $(CFLAGS) -Iengine -DRVF_GATE=0 -o $(TAILB)/s_old host/lab_engine.c $(ENGINE) engine/sy_cln_rv.c -lm
	$(CC) $(CFLAGS) -Iengine -o $(TAILB)/s_new host/lab_engine.c $(ENGINE) engine/sy_cln_rv.c -lm
	$(CC) $(CFLAGS) -Iengine $(TAILM) -DHYB_RV_LEND -DHYB_RT_SPARE -DRVF_GATE=0 $(C40) -o $(TAILB)/m_old host/lab_engine.c $(ENGINE) engine/sy_cln_rv.c -lm
	$(CC) $(CFLAGS) -Iengine $(TAILM) -DHYB_RV_LEND -DHYB_RT_SPARE -o $(TAILB)/m_new host/lab_engine.c $(ENGINE) engine/sy_cln_rv.c -lm
	$(CC) $(CFLAGS) -Iengine $(TAILM) -DHYB_RV_LEND -DHYB_RT_SPARE $(C40) -o $(TAILB)/m_c40 host/lab_engine.c $(ENGINE) engine/sy_cln_rv.c -lm
	$(CC) $(CFLAGS) -Iengine $(TAILM) -DHYB_RV_LEND -DHYB_RT_SPARE -DRVQ_GATE=0.f -o $(TAILB)/m_ng host/lab_engine.c $(ENGINE) engine/sy_cln_rv.c -lm
	$(CC) $(CFLAGS) -Iengine $(TAILM) -DHYB_RV_BITS=32 -DRVF_GATE=0 -o $(TAILB)/u_old host/lab_engine.c $(ENGINE) engine/sy_cln_rv.c -lm
	$(CC) $(CFLAGS) -Iengine $(TAILM) -DHYB_RV_BITS=32 -o $(TAILB)/u_new host/lab_engine.c $(ENGINE) engine/sy_cln_rv.c -lm
	python3 tools/perf/tailtest.py $(TAILB)

# make shimtest (1.18C45): the shimmer's MODULATION (its pitch deviation, LFO 3 as the modulator) and its level (no
# pumping under TAIL LIMITED) on the lab renderer, two parts with the lent float ring (tools/perf/shimtest.py)
SHIMB = build/shim
shimtest: host/lab_engine.c $(ENGINE) engine/*.h engine/*.inc engine/sy_cln_rv.c
	@mkdir -p $(SHIMB)
	$(CC) $(CFLAGS) -Iengine $(TAILM) -DHYB_RV_LEND -DHYB_RT_SPARE -o $(SHIMB)/e_new host/lab_engine.c $(ENGINE) engine/sy_cln_rv.c -lm
	$(CC) $(CFLAGS) -Iengine $(TAILM) -DHYB_RV_LEND -DHYB_RT_SPARE -DRVS_LEVEL=0 -o $(SHIMB)/e_l0 host/lab_engine.c $(ENGINE) engine/sy_cln_rv.c -lm
	$(CC) $(CFLAGS) -Iengine $(TAILM) -DHYB_RV_LEND -DHYB_RT_SPARE -DCLN_RVS_LOG -DCLN_RV_TUNE -DRVS_TEST_DRY -o $(SHIMB)/e_dry host/lab_engine.c $(ENGINE) engine/sy_cln_rv.c -lm
	python3 tools/perf/shimtest.py $(SHIMB)

# make shimwidth (1.18C47): FX > SHIMMER's WIDTH over time, the engine without it (-DRVS_WIDTH=0,
# 1.18C46's) against the default build at WIDTH 64 / 127 and W GLIDE 0 / 5 / 15, and W GLIDE's law from the side
# gain's log (-DRVS_WLOG); measures only (tools/perf/shimwidth.py)
SHIMW = build/shimw
shimwidth: host/lab_engine.c $(ENGINE) engine/*.h engine/*.inc engine/sy_cln_rv.c
	@mkdir -p $(SHIMW)
	$(CC) $(CFLAGS) -Iengine $(TAILM) -DHYB_RV_LEND -DHYB_RT_SPARE -DRVS_WIDTH=0 -o $(SHIMW)/today host/lab_engine.c $(ENGINE) engine/sy_cln_rv.c -lm
	$(CC) $(CFLAGS) -Iengine $(TAILM) -DHYB_RV_LEND -DHYB_RT_SPARE -o $(SHIMW)/width host/lab_engine.c $(ENGINE) engine/sy_cln_rv.c -lm
	$(CC) $(CFLAGS) -Iengine $(TAILM) -DHYB_RV_LEND -DHYB_RT_SPARE -DCLN_RVS_LOG -DRVS_WLOG -o $(SHIMW)/wlog host/lab_engine.c $(ENGINE) engine/sy_cln_rv.c -lm
	python3 tools/perf/shimwidth.py $(SHIMW)/today $(SHIMW)/width:64:5,$(SHIMW)/width:127:0,$(SHIMW)/width:127:5,$(SHIMW)/width:127:15 pad,stab,arp,pad2 --glide $(SHIMW)/wlog
