#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
# bk_test.sh: the user banks' store's own test (bk_test.c: bankstore.c against fm1-base's storage.c on a simulated
# flash: the update from 1.18C29, power losses, wear). HYB_ROOT the checkout (default: this one), FM1_BASE fm1-base's
HERE=$(cd "$(dirname "$0")" && pwd)
R=${HYB_ROOT:-$(cd "$HERE/../../../.." && pwd)}
FB=${FM1_BASE:-$R/fm1-base}
O=${TMPDIR:-/tmp}/bk_test.$$
cc -O2 -std=gnu11 -Wall -Wno-unused-function -I"$FB/include" -I"$FB/device" -I"$R/cleaner/fm1" "$HERE/bk_test.c" -o "$O" && "$O" "$@"
r=$?; rm -f "$O"; exit $r
