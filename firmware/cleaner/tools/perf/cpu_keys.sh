# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
# cpu_keys.sh: the CPU benchmark's cases as fm1-emu key sequences (sourced by cpu_case.sh; times in ms). The A0xx / K / ML
# cases need a build with bank A filled (no factory banks are released); EURO runs on any build
LAY="1500:GLO,1800:enc0:+1,2100:REC,2400:REC,2700:enc0:+1,3000:REC,3300:HOME,3600:HOME"   # GLO > MULTI: LAYER
HA="3900:enc6:+1,4100:enc6:+23"
LSH3="4500:FX,4800:enc0:+2,5000:enc2:+60,5200:enc2:+60,5400:enc2:+10,5600:enc3:+1,5800:enc3:+2,6000:HOME"
HB3="6200:HOME,6600:enc6:+1,6800:enc6:+12"
NT="7500:midi:903c64,7500:midi:904064,7500:midi:904364"
K1="$LAY,$HA,$LSH3,$HB3,$NT,7700:PLAY"
HB7="6200:HOME,6600:enc6:+1,6800:enc6:+6"
K2="$LAY,$HA,6000:HOME,$HB7,$NT"
N16=""; for n in 30 33 36 38 40 43 45 47 48 50 52 55 57 59 60 62; do N16="$N16,7500:midi:90$(printf %02x $n)64"; done
K3="3000:HOME${N16}"
CH8=""; for n in 48 52 55 59 60 64 67 71; do CH8="$CH8,7500:midi:90$(printf %02x $n)64"; done
P() { echo "3000:HOME,3900:enc6:+1,4100:enc6:+$(($1-1)),5000:HOME"; }
A008c="$(P 8)$CH8"; A019c="$(P 19)$CH8"; A024c="$(P 24)$CH8"; A010c="$(P 10)$CH8"
MAB="$LAY,3900:enc6:+1,4100:enc6:+7,6000:HOME,6200:HOME,6600:enc6:+1,6800:enc6:+19,7200:HOME"
ML="$MAB,7500:midi:903c64"
# EURO (after 1.18C30, the overload work): full keys on a 4-voice unison pad with a reverb send, the patch over USB at
# 2.5 s (cpu_case.sh: EURO_SYX, else euro_like.py's own): 16 notes in one block at 4 s, held; at 6 s all off and 16
# others on in the same block (stealing, the release tails); at 8 s 8 more on top; at 10 s all 24 struck again
on() { t=$1; shift; for n in "$@"; do printf ",%s:midi:90%02x64" $t $n; done; }
off() { t=$1; shift; for n in "$@"; do printf ",%s:midi:80%02x00" $t $n; done; }
EA="36 40 43 47 48 52 55 59 60 64 67 71 72 76 79 83"; EB="38 41 45 48 50 53 57 60 62 65 69 72 74 77 81 84"
EC="30 33 86 88 90 93 95 96"
EURO="3000:HOME$(on 4000 $EA)$(off 6000 $EA)$(on 6000 $EB)$(on 8000 $EC)$(off 10000 $EB $EC)$(on 10003 $EB $EC)"
