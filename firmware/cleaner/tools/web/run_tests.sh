#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
# The InfeKt-FM1 page's tests: run_tests.sh M-BOOTLOADER.fwsc FIRMWARE.fwsc [WAVES_DIR]
#   the installer core (m-bootloader's package as the test package), the patch core (made-up patches: make_syx.py), its
#   converter for the successor model's 512-byte singles (test_bigconv.mjs, a made-up single),
#   the wave core (WAVES_DIR: ../waves/test_host.sh's output; skipped without it), and the page under jsdom with a
#   simulated FM-1, its Editor tab with a simulated remote edit (test_editor.mjs), its port detection among MIDI keyboards
#   that do not answer (test_ports.mjs), the patch manager in Chromium (test_organizer.mjs), the patch export with a simulated dump (test_export.mjs),
#   the Installation tab's "?" tooltips and its layout in Chromium (test_install_view.mjs), the factory banks'
#   option and their write after an install, the wait display over it (test_factory.mjs: made-up banks) (FIRMWARE: a release package
#   FM-1_13009500, embedded by make_page.py; jsdom from the working directory's node_modules: run it where
#   `npm install jsdom` was done)
# Both variants of the page (make_page.py: without and with --synth, the sound in the browser): the page tests run on
# each; then the web synth itself: test_websynth.mjs in Chromium, and wasm/fidelity.mjs (its module against the host lab
# engine built with the same switches: bit for bit; skipped without clang's wasm32 target and wasm-ld)
B=$1; F=$2; WD=$3; T=$(cd "$(dirname "$0")" && pwd); O=$(mktemp -d)
[ -f "$B" ] && [ -f "$F" ] || { echo "usage: run_tests.sh M-BOOTLOADER.fwsc FIRMWARE.fwsc [WAVES_DIR]"; exit 2; }
fail=0
run() { out=$("$@" 2>&1); r=$?; echo "$out" | tail -1; [ $r -eq 0 ] || { echo "$out" | grep FAIL; fail=1; }; }
run node "$T/test_installer.mjs" "$B"
python3 - "$O" <<'EOF'
import random, sys
r = random.Random(5)
for k in range(32):
    d = [r.randrange(128) for _ in range(256)]
    d[240:250] = [ord(c) for c in f"TEST {k:03d}  "[:10]]
    open(f"{sys.argv[1]}/p{k:02d}.raw", "wb").write(bytes(d))
EOF
python3 "$T/../bank_upload/make_syx.py" "$O/bank.syx" --mid "$O/bank.mid" "$O"/p*.raw >/dev/null || fail=1
run node "$T/test_bank_core.mjs" "$O/bank.syx" "$O/bank.mid"
run node "$T/test_bigconv.mjs"
if [ -n "$WD" ]; then run node "$T/test_waves_core.mjs" "$WD"; else echo "waves core: skipped (no WAVES_DIR)"; fi
python3 "$T/make_page.py" "$O/page.html" --fwsc "$F" --boot "$B" >/dev/null || fail=1
WSOK=; command -v wasm-ld >/dev/null && echo 'int f(void){return 0;}' | clang --target=wasm32 -x c -c - -o /dev/null 2>/dev/null && WSOK=1
if [ -n "$WSOK" ]; then
    sh "$T/wasm/build.sh" "$O/ws.wasm" -Oz >/dev/null || fail=1
    python3 "$T/make_page.py" "$O/page_ws.html" --fwsc "$F" --boot "$B" --synth --wasm "$O/ws.wasm" >/dev/null || fail=1
    PAGES="page page_ws"
else
    echo "web synth: skipped (no clang wasm32 / wasm-ld)"; PAGES=page
fi
# the factory banks and wavetables (make_page.py --factory, --waves): two made-up banks (TEST A000.., TEST B000..; bank
# A's first patch with an extension frame after its single) and three made-up waves, a page with them, test_factory.mjs
# on it and on the page without
python3 - "$O" <<'PYEOF' || fail=1
import random, sys
r = random.Random(7)
def single(data, prog, bank=1):
    f = [0xF0, 0, 0x20, 0x33, 0x01, 0, 0x10, bank, prog] + data
    return bytes(f + [sum(f[5:265]) & 0x7F, 0xF7])
def ext(x, prog, bank=1):
    f = [0xF0, 0x7D, 0x49, 0x58, 1, bank, prog, len(x)] + x
    return bytes(f + [sum(f[4:]) & 0x7F, 0xF7])
for L in "AB":
    out = b""
    for k in range(128):
        d = [r.randrange(128) for _ in range(256)]
        d[240:250] = [ord(c) for c in f"TEST {L}{k:03d}".ljust(10)]
        out += single(d, k)
        if L == "A" and k == 0:
            out += ext([90, 1, 100, 64, 0, 0, 0, 57] + [0] * 12 + [5], k)
    open(f"{sys.argv[1]}/fact_{L}.syx", "wb").write(out)
# made-up wavetables (make_page.py --waves): "Wave 03..05.wav" (single cycles of 600 samples, 16-bit), a sine and
# a triangle file (left out), a read-me with the names
import math, os, struct
d = sys.argv[1] + "/waves"
os.makedirs(d, exist_ok=True)
def wav(path, ys):
    data = b"".join(struct.pack("<h", int(max(-1, min(1, y)) * 32000)) for y in ys)
    open(path, "wb").write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, 48000, 96000, 2, 16)
                           + b"data" + struct.pack("<I", len(data)) + data)
N = 600
wav(d + "/Sine.wav", [math.sin(2 * math.pi * i / N) for i in range(N)])
wav(d + "/Triangle.wav", [1 - 4 * abs(i / N - 0.5) for i in range(N)])
for k, h in ((3, 3), (4, 5), (5, 9)):
    wav(f"{d}/Wave {k:02d}.wav", [math.sin(2 * math.pi * i / N) + 0.5 * math.sin(2 * math.pi * h * i / N) for i in range(N)])
open(d + "/Read me.txt", "w").write("Sine\nTriangle\nWave 03 - TEST W03\nWave 04 - TEST W04\nWave 05 - TEST W05\n")
PYEOF
python3 "$T/make_page.py" "$O/page_fac.html" --fwsc "$F" --boot "$B" --factory "$O/fact_A.syx" "$O/fact_B.syx" --waves "$O/waves" >/dev/null || fail=1
run node "$T/test_factory.mjs" "$O/page_fac.html" "$O/page.html"
PW=; node -e 'require("playwright")' 2>/dev/null && PW=1
for P in $PAGES; do
    echo "-- $P.html"
    run node "$T/test_ui.mjs" "$O/$P.html"
    run node "$T/test_editor.mjs" "$O/$P.html"
    run node "$T/test_export.mjs" "$O/$P.html"
    run node "$T/test_ports.mjs" "$O/$P.html"
    # the patch manager in Chromium (Playwright from the working directory's node_modules or the global ones; skipped
    # without it)
    if [ -n "$PW" ]; then run node "$T/test_organizer.mjs" "$O/$P.html"; else echo "organizer: skipped (no playwright)"; fi
    if [ -n "$PW" ]; then run node "$T/test_install_view.mjs" "$O/$P.html"; else echo "install view: skipped (no playwright)"; fi
done
if [ -n "$WSOK" ]; then
    if [ -n "$PW" ]; then run node "$T/test_websynth.mjs" "$O/page_ws.html" "$O/page.html" "$O/bank.syx"; else echo "websynth: skipped (no playwright)"; fi
    E=$T/../../engine
    ${CC:-cc} -O2 -std=c99 -ffp-contract=off -w -DHYB_MULTI -DHYB_RV_LEND -DSY_MAX_VOICES=16 -DSY_DELAY_FRAMES=32768 -DSY_UNI_CAP \
        -I"$E" -o "$O/lab_ws" "$T/../../host/lab_engine.c" "$E/synth.c" "$E/sy_voice.c" "$E/sy_tables.c" "$E/sy_fx.c" "$E/sy_waves.c" \
        "$E/sy_dist_tab.c" "$E/sy_arp_tab.c" "$E/sy_hyb_lfow.c" "$E/sy_cln_rv.c" -lm || fail=1
    run node "$T/wasm/fidelity.mjs" "$O/lab_ws" "$O/ws.wasm"
fi
rm -rf "$O"
exit $fail
