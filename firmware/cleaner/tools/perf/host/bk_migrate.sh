#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
# bk_migrate.sh OLD_HOST NEW_HOST [OUTDIR]: the update from 1.18C29 to the six banks (1.18C30), end to end in the host
# simulators (mk.sh mble-lent of each release, both with this hostcmd.c: its ubfill, ubsum, setsave, setsum, lsput,
# lssum). The old release fills banks C, D, E (the screen's A, B, C; extensions too), the arp and drum patterns, saves
# part A's and part B's patches (one the init patch), two launcher sets that name the init patch and slots, reboots and
# dumps its store (stdump); bk_test mkimg writes the banks' objects into a flash image as fm1-base's storage.c does on
# the device (A/B copies, older copies beside); the new release boots from that store and that flash. Its banks, the
# patterns' objects, the settings' patches and the sets must read as the old release read them; then saves into
# banks D-F and a reboot. Exit status 0: all passed
O=$1; N=$2; D=${3:-$(mktemp -d)}; H=$(cd "$(dirname "$0")" && pwd); r=0
mkdir -p "$D"
cat > "$D/old.txt" <<EOT
wait 1500
oldobj
ubfill 1 400 3
ubfill 2 40 1
setsave C005 INIT
lsput 0 INIT C010
lsput 3 E100 C005
lsput 7 INIT INIT
reboot
wait 1500
setsum
lssum
ubsum 3
objsum
stdump $D/st.bin
EOT
cat > "$D/new.txt" <<EOT
wait 1500
stload $D/st.bin
data $D/bk.img 0xA5000
reboot
wait 1500
setsum
lssum
ubsum 3
objsum
bkstat
ubsum
ubfill 9 300 6
ubsum
reboot
wait 1500
ubsum
bkstat
EOT
"$O" "$D/old.txt" "$D" > "$D/old.out" 2>&1 || { echo "FAIL the old release's run"; r=1; }
grep -q "^FAIL" "$D/old.out" && { grep "^FAIL" "$D/old.out"; r=1; }
TMPDIR=$D "$H/bk_test.sh" mkimg "$D/st.bin" "$D/bk.img" || { echo "FAIL mkimg"; exit 1; }
"$N" "$D/new.txt" "$D" > "$D/new.out" 2>&1 || { echo "FAIL the new release's run"; r=1; }
grep -q "^FAIL" "$D/new.out" && { grep "^FAIL" "$D/new.out"; r=1; }
pick() { grep -E "^  (setsum|lssum|ubsum|objsum)" "$1"; }
pick "$D/old.out" > "$D/old.sum"
pick "$D/new.out" | head -n "$(wc -l < "$D/old.sum")" | sed 's/ (part B plays.*//' > "$D/new.sum"
sed -i 's/ (part B plays.*//' "$D/old.sum"
# (objsum's last figures: the writes the store was asked for, which differ by the runs)
sed -i 's/ writes [0-9]* skipped [0-9]*$//' "$D/old.sum" "$D/new.sum"
if diff "$D/old.sum" "$D/new.sum" > "$D/diff.txt"; then echo "  ok   the update: settings, launcher sets, banks C-E (A-C), the patterns as 1.18C29 read them"
else echo "FAIL the update differs:"; cat "$D/diff.txt"; r=1; fi
cat "$D/old.sum"
grep -E "ubsum bank [345]: 0 filled" "$D/new.out" | head -3 | grep -c . | grep -q 3 && echo "  ok   banks D-F empty after it" || { echo "FAIL banks D-F"; r=1; }
grep -E "bkstat|ubsum" "$D/new.out" | tail -9
a=$(grep "ubsum" "$D/new.out" | tail -12 | head -6); b=$(grep "ubsum" "$D/new.out" | tail -6)
[ "$a" = "$b" ] && echo "  ok   300 saves into all six banks kept over a reboot" || { echo "FAIL the six banks after a reboot"; r=1; }
exit $r
