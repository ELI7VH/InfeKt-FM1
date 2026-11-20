#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
# mk.sh [multi|mble|mble-lent]: the host simulator from a copy of the app with test commands (multi: the USB build's
# two parts; mble: the Bluetooth build with two parts, as build_multi_ble.sh makes it; it boots with Bluetooth on as saved, its memory lent after GLO > BLUETOOTH OFF, a save and a
# reboot; mble-lent: that build booted with Bluetooth off, as a test build's HYB_TEST_BT_OFF, its REVERB FULL as
# shipped as the other builds' (RVQ_HALF_DEF 0): the shimmer's scripts run there, 1.04)
# HYB_ROOT: the checkout to build from (default: the folder that holds cleaner/), HOST_M: the scratch folder
# (default scratchpad/seqb; its h/fm1t is replaced), HOST_CFLAGS: more --cflag=... (e.g. --cflag=-DHYB_NO_HAT)
# The firmware has no factory banks (app.c): tests/run.sh runs the "nofactory" scripts
R=${HYB_ROOT:-$(cd "$(dirname "$0")/../../../.." && pwd)}
M=${HOST_M:-/tmp/infekt/seqb}
KC=${HOST_KC:-hcc}
rm -rf $M/h/fm1t; mkdir -p $M/h/fm1t; cd $R/cleaner/fm1
cp app.c app.h bankstore.c ui.c waves.c splash.c remote.c patchx.c bench.c bench.h bench_big.inc bench_tab.inc bench_tab_s.inc init_patch.h fm1.toml words.tsv $M/h/fm1t/
ln -s $R/cleaner/fm1/hat $M/h/fm1t/hat
NB=; [ "$1" = multi ] && NB=--no-ble
cat >> $M/h/fm1t/app.h <<'EOT'
#define APP_HOST_CMD
int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d);
#define APP_HOST_MS
void app_host_ms(void);
EOT
{ printf '%s\n' '#include <stdint.h>' 'static void host_midi_tap(uint32_t pkt, uint32_t to);' \
    '#define plat_midi_send(p, t) host_midi_tap(p, t)   /* (hostcmd.c: the notes out counted, "bal") */' \
    '#define app_render app_render_dut                  /* (hostcmd.c: the output hashed, "ohash") */' \
    'static void host_sx_tap(const uint8_t *f, uint32_t n);' \
    '#define plat_sysex_send(f, n) host_sx_tap(f, n)    /* (hostcmd.c: the SysEx out kept, "wsx") */' \
    'static void host_leds_tap(uint32_t b, uint32_t k);' \
    '#define plat_leds(b, k) host_leds_tap(b, k)        /* (hostcmd.c, 1.05: the buttons lit as set, "btnis") */' \
    'static void host_lights_tap(uint32_t b, uint32_t k, uint32_t d, uint32_t dk, int g);' \
    '#define plat_lights(b, k, d, dk, g) host_lights_tap(b, k, d, dk, g)' \
    'static void host_mid_tap(uint32_t b, uint32_t k);' \
    '#define plat_lights_mid(b, k) host_mid_tap(b, k)   /* (hostcmd.c, 1.18C43: the middle level too, "ltis") */' \
    'static void host_fade_tap(uint32_t k, uint32_t lv);' \
    '#define plat_lights_fade(k, lv) host_fade_tap(k, lv)   /* (hostcmd.c, 1.18C46: the fade level, "ltfade") */' \
    'static int host_batt(void);' \
    '#define plat_battery() host_batt()                 /* (hostcmd.c: "batt" sets the level the launcher shows) */' \
    'static int32_t host_enc(int role);' \
    '#define plat_enc(r) host_enc(r)                    /* (hostcmd.c, 1.18C21: a detent in between two reads, "turnlate") */'
  grep -q host_midi_in $R/cleaner/tools/perf/host/hostcmd.c && printf '%s\n' '#define HOST_XM' \
    'static int host_midi_in(uint32_t *p, int *port);' \
    '#define plat_midi_in(p, q) host_midi_in(p, q)     /* (hostcmd.c, 1.19.5: a simulated clock master, "xm") */'
  cat $M/h/fm1t/app.c; } > $M/h/fm1t/app.c.t && mv $M/h/fm1t/app.c.t $M/h/fm1t/app.c
cat $R/cleaner/tools/perf/host/hostcmd.c >> $M/h/fm1t/app.c
B="tools/build.py"
MB="--name CLEANER-MBLE --cflag=-DHYB_MULTI --cflag=-DHYB_BANK_PACK --cflag=-DHYB_RV_PACK --cflag=-DHYB_ARP_PACK --cflag=-DHYB_RV_LEND --cflag=-DHYB_LFOW_PACK --cflag=-DHYB_RT_SPARE"
[ "$1" = mble ] && B="$R/cleaner/fm1/build_variant.py $MB $HOST_CFLAGS --"
[ "$1" = mble-lent ] && B="$R/cleaner/fm1/build_variant.py $MB --cflag=-DHYB_TEST_BT_OFF --cflag=-DRVQ_HALF_DEF=0 $HOST_CFLAGS --"
[ "$1" = multi ] && [ -n "$HOST_CFLAGS" ] && B="$R/cleaner/fm1/build_variant.py --cflag=-DHYB_MULTI --cflag=-DHYB_RV_BITS=32 $HOST_CFLAGS --"
[ -z "$1" ] && [ -n "$HOST_CFLAGS" ] && B="$R/cleaner/fm1/build_variant.py $HOST_CFLAGS --"
cd ${FM1_BASE:-$R/fm1-base} && CC=$KC python3 $B $M/h/fm1t --host $NB 2>&1 | grep -E "error|^host|warning" 
ls -la $M/h/fm1t/build/host/*_host 2>/dev/null | tail -1
