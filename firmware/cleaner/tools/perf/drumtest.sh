#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
# drumtest.sh [OUTDIR]: the drums of 1.18C42 against 1.18C41's: two host simulators (host/mk.sh mble,
# as the firmware's Bluetooth two-part build; one with -DDR_HALF=0, the drums as 1.18C41's), then drumcmp.py per
# instrument. HOST_M's scratch folders: $OUTDIR/old, $OUTDIR/new (each with a link "build" to the checkout's build)
H=$(cd "$(dirname "$0")" && pwd)
R=${HYB_ROOT:-$(cd "$(dirname "$0")/../../../.." && pwd)}
O=${1:-/tmp/infekt/drumtest}
for v in old new; do
    mkdir -p "$O/$v"; ln -sfn "$R/build" "$O/$v/build"
    F=; [ $v = old ] && F=--cflag=-DDR_HALF=0
    HOST_M="$O/$v" HOST_CFLAGS="$F" sh "$H/host/mk.sh" mble > "$O/$v.log" 2>&1
    cp "$O/$v/h/fm1t/build/host/cleaner-mble_host" "$O/host_$v" || { echo "drumtest: the $v build failed ($O/$v.log)"; exit 1; }
done
python3 "$H/drumcmp.py" "$O/host_old" "$O/host_new" "$O/wav"
