#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
# The cleaner's firmware engine (build/cleaner_ble/engine): cleaner/engine, the constant tables stored smaller where
# it cannot be heard. Run after every engine change: python3 make_engine.py (the build scripts build_usb.sh /
# build_multi_ble.sh run it).
#
# The reverb (the unit sy_rv.c): the own one (engine/sy_cln_rv.c, PROVENANCE.md).
import os, re, shutil, struct, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
ENG = ROOT / "build" / "cleaner_ble" / "engine"
ENG.mkdir(parents=True, exist_ok=True)
for f in ENG.iterdir():
    if f.is_file():
        f.unlink()
E = ROOT / "cleaner" / "engine"
for f in E.iterdir():
    if f.suffix in (".c", ".h", ".inc"):
        shutil.copy(f, ENG / f.name)
(ENG / "sy_rv.c").write_text('#include "sy_cln_rv.c"   /* (the reverb unit of fm1.toml: the own reverb) */\n')

NUM = r"-?\d+\.\d*(?:[eE][-+]?\d+)?"


def floats(text, name):
    i = text.index(name)
    i = text.index("=", i)
    j = text.index("};", i)
    return [float(v) for v in re.findall(NUM, text[i + 1:j])], i, j


def sub(path, old, new):
    s = path.read_text()
    if old not in s:
        sys.exit("make_engine: not found in %s: %s" % (path.name, old[:70]))
    path.write_text(s.replace(old, new, 1))


def rows(vals, per=12):
    return ",\n".join("    " + ", ".join(str(v) for v in vals[k:k + per]) for k in range(0, len(vals), per))


# 1. (the distortion curves 1-4: since the cleaner a parametric fit, sy_cln_d14.h: nothing to shrink)

# 2. the LFO waves
p = ENG / "sy_hyb_lfow.c"
s = p.read_text()
v, i, j = floats(s, "sy_hyb_lfow[62][64]")
assert len(v) == 62 * 64, len(v)
q = [int(round(x * 1000)) for x in v]
assert all(abs(a / 1000 - b) < 1e-9 for a, b in zip(q, v))
k = s.index("const float sy_hyb_lfow")
s = s[:k] + "const int16_t sy_hyb_lfow[62][64] = {\n" + rows(q, 16) + "\n" + s[j:]
p.write_text(s)
sub(ENG / "sy_hyb_lfow.h", "extern const float sy_hyb_lfow[62][64];",
    "#include <stdint.h>\nextern const int16_t sy_hyb_lfow[62][64];   /* (Bluetooth build: x 1000) */")
sub(ENG / "synth.c", "const float *w = sy_hyb_lfow[shape > 67 ? 61 : shape - 6];",
    "const int16_t *w = sy_hyb_lfow[shape > 67 ? 61 : shape - 6];")
sub(ENG / "synth.c", "y = w[i & 63] + fr * (w[(i + 1) & 63] - w[i & 63]);",
    "y = ((float)w[i & 63] + fr * (float)(w[(i + 1) & 63] - w[i & 63])) * 0.001f;")


# 4c. HYB_ARP_PACK (the Bluetooth two-part build, its flash full): the arpeggiator's patterns without their unused
#     tails: each pattern's SY_ARP_N events one after another, SY_ARP_OFF the first's index (5124 -> 2548 B); synth.c
#     reads only the first SY_ARP_N[p] events of a pattern, the same bytes
t = (ENG / "sy_arp_tab.c").read_text()
ns = [int(x) for x in re.search(r"SY_ARP_N\[61\] = \{([^}]*)\}", t).group(1).split(",")]
i = t.index("const uint8_t SY_ARP_EV[61][SY_ARP_MAX][2] = {")
body = t[t.index("{", i) + 1:t.index("};", i)]
pats = re.findall(r"\{((?:\s*\{\s*\d+\s*,\s*\d+\s*\}\s*,?)*)\s*\}", body)
assert len(pats) == 61 and len(ns) == 61
ev, off = [], []
for n, pt in zip(ns, pats):
    pr = re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*\}", pt)
    assert len(pr) >= n and all(x == ("0", "0") for x in pr[n:]), "a pattern's tail is not empty"
    off.append(len(ev))
    ev += pr[:n]
assert len(ev) < 65536
t = t.replace("const uint8_t SY_ARP_EV[61][SY_ARP_MAX][2] = {", "#ifndef HYB_ARP_PACK\nconst uint8_t SY_ARP_EV[61][SY_ARP_MAX][2] = {", 1)
t = t.rstrip() + "\n#else\n/* (HYB_ARP_PACK: make_engine.py 4c) */\nconst uint16_t SY_ARP_OFF[61] = {" + ", ".join(map(str, off)) + \
    "};\nconst uint8_t SY_ARP_EVP[" + str(len(ev)) + "][2] = {" + ", ".join("{%s, %s}" % e for e in ev) + "};\n#endif\n"
(ENG / "sy_arp_tab.c").write_text(t)
sub(ENG / "sy_int.h", "extern const uint8_t SY_ARP_EV[61][SY_ARP_MAX][2];",
    "#ifdef HYB_ARP_PACK\nextern const uint16_t SY_ARP_OFF[61];\nextern const uint8_t SY_ARP_EVP[][2];\n"
    "#define SY_ARP_PAT(p) (SY_ARP_EVP + SY_ARP_OFF[p])\n#else\nextern const uint8_t SY_ARP_EV[61][SY_ARP_MAX][2];\n"
    "#define SY_ARP_PAT(p) SY_ARP_EV[p]\n#endif")
sub(ENG / "synth.c", "const uint8_t (*ev)[2] = SY_ARP_EV[pat - 1];", "const uint8_t (*ev)[2] = SY_ARP_PAT(pat - 1);")

# 4d. HYB_LFOW_PACK (the Bluetooth two-part build, its flash full, 0.86): the LFO waves (x 1000, |v| <= 1000) in 11
#     bits a point, one after another (7936 -> 5458 B); synth.c's lfo_shape reads a point through sy_lfow_at: the same
#     values (lossless)
t = (ENG / "sy_hyb_lfow.c").read_text()
i = t.index("const int16_t sy_hyb_lfow[62][64] = {")
j = t.index("};", i)
q = [int(x) for x in re.findall(r"-?\d+", t[t.index("{", i) + 1:j])]
assert len(q) == 62 * 64 and all(-1024 <= x < 1024 for x in q)
acc = 0
for n, x in enumerate(q):
    acc |= (x & 0x7ff) << (11 * n)
nb = (11 * len(q) + 7) // 8 + 2          # (two bytes past the end: a point's three-byte read)
pk = list(acc.to_bytes(nb, "little"))
t = t[:i] + "#ifndef HYB_LFOW_PACK\n" + t[i:j + 2] + "\n#else\n/* (HYB_LFOW_PACK: make_engine.py 4d) */\nconst uint8_t sy_hyb_lfow_p[" + \
    str(len(pk)) + "] = {" + ", ".join(map(str, pk)) + "};\n#endif" + t[j + 2:]
(ENG / "sy_hyb_lfow.c").write_text(t)
sub(ENG / "sy_hyb_lfow.h", "#include <stdint.h>\nextern const int16_t sy_hyb_lfow[62][64];   /* (Bluetooth build: x 1000) */",
    "#include <stdint.h>\n#ifdef HYB_LFOW_PACK\nextern const uint8_t sy_hyb_lfow_p[];   /* (make_engine.py 4d: 11 bits a point) */\n"
    "static inline __attribute__((always_inline)) int sy_lfow_at(int wv, int j)   /* wave wv's point j (x 1000) */\n{\n"
    "    const unsigned o = (unsigned)(wv * 64 + j) * 11u;\n    const uint8_t *b = sy_hyb_lfow_p + (o >> 3);\n"
    "    const int v = (int)(((unsigned)b[0] | (unsigned)b[1] << 8 | (unsigned)b[2] << 16) >> (o & 7u) & 0x7ffu);\n"
    "    return (v ^ 0x400) - 0x400;\n}\n"
    "#else\nextern const int16_t sy_hyb_lfow[62][64];   /* (Bluetooth build: x 1000) */\n#endif")
sub(ENG / "synth.c", "const int16_t *w = sy_hyb_lfow[shape > 67 ? 61 : shape - 6];",
    "#ifdef HYB_LFOW_PACK\n            const int wv = shape > 67 ? 61 : shape - 6;\n#define LFOW_AT(j) sy_lfow_at(wv, (j))\n#else\n"
    "            const int16_t *w = sy_hyb_lfow[shape > 67 ? 61 : shape - 6];\n#define LFOW_AT(j) w[(j)]\n#endif")
sub(ENG / "synth.c", "y = ((float)w[i & 63] + fr * (float)(w[(i + 1) & 63] - w[i & 63])) * 0.001f;",
    "y = ((float)LFOW_AT(i & 63) + fr * (float)(LFOW_AT((i + 1) & 63) - LFOW_AT(i & 63))) * 0.001f;\n#undef LFOW_AT")

print("make_engine: done (the reverb: the own one)")
