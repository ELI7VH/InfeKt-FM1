#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
"""cold_predict.py SHORT.d LONG.d DIS [--base SHORT.d LONG.d DIS] [--tpb T]: what the emulator does not see.

The emulator counts instructions; the FM-1's flash cache does not show in it. The device's benchmark (1.18C34-BENCH,
the development notes, cpu-plan.md section 15) found warm loops as fast from the flash as from RAM, but code run
ONCE A BLOCK from the flash 8-10x the emulator: its lines are cold every block (the oscillators' wave reads and the main
loop evict them). This adds that term to an emulator run: two runs of cpu_case.sh / case35.sh (their PC histograms
h.hist and logs, the steady part between them) and the build's disassembly give

  - the first core's interrupt instructions a block (as fdiff.py), at the emulator's rate;
  - the bytes of flash code (0x02000000 up) the audio's path runs at least once every block (each instruction's
    count / blocks >= 0.9), the main loop's and the timer's functions left out (MAIN below: they run outside the
    render, or warm, 29 times a block); each such byte costs ~T ticks of 24 MHz a block on the device (--tpb, default
    0.24: the 1.18C34-BENCH calibration's fixed term, 3 852 ticks a block the emulator does not have (findings_A001),
    over the 16.3 KB of such code the emulator's a test patch run (1 key, 1.18C35) runs every block; rough: the device's
    pre + ev + post + feq excess, ~3.3 k ticks, agrees within ~20 %);
  - the device's expected extra load from it, % of a block (69 660 ticks: 128 frames at the codec's rate).

--base: the same for another build (the runs of the same case): the differences, i.e. the gain the device should show
that the emulator cannot (code moved from the flash into RAM: RV_SLOT / SY_BLK_RAM, 1.18C36)."""
import collections
import re
import sys

MAIN = re.compile(r"^(input|ble_|touch_|fm1_cstart|fm1_timer5_irq|fm1__|isr_|sq_keys_put|sq_lights|sq_idle|sq_midi_in|cv_text|"
                   r"dw_tick|plat_(lights|enc|buttons|lock|unlock|ms|ticks24|midi_in)|shim_|ln_|app_key_|app_frame|"
                   r"app_input_only|sie_|usb_|ota_|oct_lights|sr_|radio_|periph_|key_rs_|patch_label|name_label|"
                   r"ui_|qa_|em_|hold_zone_fix|send_in|play_on|sq_qcatch|sq_endp|sq_pv_|dr_qcatch|ble_midi|q_used|"
                   r"fm1_adc|uac_service|app_tick|memcpy$)")
BLOCK_TICKS = 69660.0


def hist(p):
    c = collections.Counter()
    for l in open(p + "/h.hist"):
        m = re.match(r"\s*(?:0x)?([0-9a-fA-F]+)\s+(\d+)", l)
        if m:
            c[int(m.group(1), 16)] += int(m.group(2))
    return c


def info(p):
    t = open(p + "/log.txt").read()
    r = float(re.search(r"\(([\d.]+) M ISR instructions per emulated s\)", t).group(1))
    ms = int(re.search(r"time (\d+) ms", t).group(1))
    halves = int(re.search(r"(\d+) audio halves", t).group(1))
    return r * ms / 1000., halves


def one(a, b, dis):
    ra, rb = info(a), info(b)
    blocks = rb[1] - ra[1]
    isr = (rb[0] - ra[0]) * 1e6 / blocks
    fn, sz, cur = {}, {}, None
    for l in open(dis, errors="replace"):
        m = re.match(r"^([A-Za-z_$][\w.$]*):\s*$", l)
        if m:
            cur = m.group(1)
            continue
        m = re.match(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)", l)
        if m and cur:
            k = int(m.group(1), 16)
            fn[k], sz[k] = cur, len(m.group(2).split())
    ha, hb = hist(a), hist(b)
    per = collections.Counter()
    for k, f in fn.items():
        if k < 0x02000000 or MAIN.match(f):
            continue
        if (hb.get(k, 0) - ha.get(k, 0)) / blocks >= 0.9:
            per[f] += sz[k]
    return isr, per


def main():
    a = sys.argv[1:]
    tpb = 0.24
    if "--tpb" in a:
        i = a.index("--tpb")
        tpb = float(a[i + 1])
        del a[i:i + 2]
    base = None
    if "--base" in a:
        i = a.index("--base")
        base = one(*a[i + 1:i + 4])
        del a[i:i + 4]
    if len(a) != 3:
        sys.exit(__doc__)
    isr, per = one(*a)
    cold = sum(per.values())
    print("first core's interrupt: %.0f k instructions a block; flash code run every block (audio): %d B -> ~%.0f ticks, "
          "%.1f %% of a block on the device (%.2f ticks a byte)" % (isr / 1e3, cold, cold * tpb, 100 * cold * tpb / BLOCK_TICKS, tpb))
    for f, n in per.most_common(25):
        print("  %-26s %5d B" % (f, n))
    if base:
        bisr, bper = base
        bc = sum(bper.values())
        print("against the base: interrupt %.0f -> %.0f k a block (emulator); flash run every block %d -> %d B: the device's "
              "expected gain beyond the emulator's ~%.0f ticks = %.1f %% of a block" % (bisr / 1e3, isr / 1e3, bc, cold,
                                                                                     (bc - cold) * tpb, 100 * (bc - cold) * tpb / BLOCK_TICKS))
        gone = [(bper[f] - per.get(f, 0), f) for f in bper if bper[f] > per.get(f, 0)]
        for d, f in sorted(gone, reverse=True)[:20]:
            print("  %-26s -%5d B" % (f, d))


if __name__ == "__main__":
    main()
