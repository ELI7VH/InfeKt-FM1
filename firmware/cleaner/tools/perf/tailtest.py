#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tailtest.py DIR [--max-db -92] [--long] [--patch NAME=FILE ...] [--cases a,b] [--jobs 4] [--wavs OUT]

The tails' rest (sy_cln_rv.c RVF_GATE): renders long-tail cases with the engine before (DIR/X_old,
built with -DRVF_GATE=0, i.e. 1.18C38's tails) and after (DIR/X_new) for the builds X = s (single, float ring), m (two
parts, Bluetooth's lent float ring for FULL; SY_NOLEND=1 the 16-bit ring), u (two parts, a 32-bit ring), and checks per
case:
  - the difference new - old (the 24-bit WAVs, peak dBFS) is at most --max-db (the user's rule: -92 dBFS) and the first
    difference comes where the old one's peak (100 ms from there) is 18 dB under that at least: nothing differs while
    the notes or their tails sound;
  - the effects rest (part A's chain skipped: lab_engine SY_REST) by the case's time T in the new build;
  - a case marked "same" (the delay, a held decay: not changed) is bit-identical.
(1.18C41) The 16-bit ring's fade (sy_cln_rv.c RVQ_FADE) against 1.18C40 (DIR/m_c40, built with -DRVQ_FADE=0
-DRV_HALF_F=0) and an engine without the 16-bit gate (DIR/m_ng, -DRVQ_GATE=0.f), kind "fade":
  - nothing differs from 1.18C40 before its gate (the first difference not before 1.18C40's last sound);
  - no step at the end: the last 20 ms before the output is 0 under --end-db (-120 dBFS peak; 1.18C40: the floor,
    -79 dBFS on a test patch), and the level (rms of 20 ms windows) falls by at most --drop-db from one window to the
    next while it is above -130 dBFS;
  - the effects rest at most RVQ_FADE's time (+0.05 s) after 1.18C40's;
  - "fadewake": a second note in the middle of the fade (1.18C40's gate + 0.4 s): the output the same as without
    the gate (m_ng) before the fade and from the block after the note's on, in between the un-gated tail times a
    gain <= 1 (|new - ng| <= the un-gated tail's peak before the note + 3 dB): the fade taken back without a step of
    its own.
The pre-delay's dead band in modes 3 and 4 (RVQ_PDZ), kind "pdz": the effects rest by the case's time (1.18C40's
limit cycle never let them), the level of every second within --pdz-db of 1.18C40's while that is above -60 dBFS rms,
and the end without a step (as "fade").
HALF on the float ring (RV_HALF_F), kind "eq": the two-part build's HALF, Bluetooth's memory lent or not, bit-identical
(the same ring); against 1.18C40's 16-bit HALF (m_c40, its ring cleared in the same blocks: the LFO's phase the same)
every second's level within --pdz-db while that is above -60 dBFS rms and the difference at most --half-db rms a
second (the 16-bit words' noise, now gone); the rest by the case's time.
The cases are the project's own patches (the INIT patch with named bytes set, hostcases.py); --patch NAME=FILE adds a
256-byte patch from outside the repository (never stored) as cases NAME_f (FULL) and NAME_q (a quiet note) on every
build. --long renders the old engine until it rests too (up to 200 s: the time to idle before / after). --wavs OUT
keeps each case's last seconds, old and new, normalised to -1 dBFS peak (for listening). Exit code 1 on a failure."""
import argparse
import math
import os
import subprocess
import sys
import tempfile
import wave
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hostcases  # noqa: E402

FS = 46875
BLK = 128
B = dict(hostcases.B, VOL=91, AE_R=63, PH_STAGES=128 + 84, PH_MIX=128 + 85, PH_RATE=128 + 86, PH_DEPTH=128 + 87,
         PH_FREQ=128 + 88, PH_FB=128 + 89, EQ_MID=128 + 92, EQ_MIDF=128 + 93, EQ_Q=128 + 94, EQ_LOW=128 + 95,
         EQ_HIGH=128 + 96, EQ_LOWF=128 + 45, BOOST=128 + 97, BOOST_T=128 + 98)
RV = dict(FX_MODE=2, FX_SEND=86, FX_TIME=20, FX_DECAY=104, FX_DEPTH=12, FX_COLOUR=49)
PAD = dict(OSC2_DET=40, CUT1=90, AE_A=20, AE_R=64)
FULL, HALF, NOLEND = {"SY_RVQ": "1"}, {"SY_RVQ": "0"}, {"SY_NOLEND": "1"}
FADE_S = 360 * BLK / FS                 # (sy_cln_rv.c RVQ_FADE: 360 blocks)
GATE_BLKS = 8                           # (1.18C40's last sound after its gate's block: the chorus's delay, the EQ)
WAKE_BLKS = 4                           # (the take-back: in the block whose input reaches half a step, a slow attack's
                                        # first blocks below it)


class T:
    """a tail case: patch bytes over INIT, notes held `hold` blocks, `secs` rendered, the rest due by `due` s (None:
    not checked), `same`: must be bit-identical (a path not changed), `wake`: a second note at `wake` s"""
    def __init__(self, name, doc, sets, builds, env, notes=(60, 64, 67), vel=100, hold=40, secs=30.0, due=25.0,
                 same=False, wake=None, raw=None, kind=None):
        self.name, self.doc, self.sets, self.builds, self.env = name, doc, dict(sets), builds, dict(env)
        self.notes, self.vel, self.hold, self.secs, self.due, self.same, self.wake, self.raw = \
            list(notes), vel, hold, secs, due, same, wake, raw
        self.kind = kind                # (None: against the old tails; "fade", "fadewake", "eq": 1.18C41)

    def patch(self):
        if self.raw is not None:
            p = list(self.raw[:256])
        else:
            p = list(hostcases.BASE)
        for k, v in self.sets.items():
            p[B[k] if isinstance(k, str) else k] = v
        if self.raw is None:
            nm = self.name[:10].encode()
            p[240:240 + len(nm)] = list(nm)
        return bytes(p)

    def events(self, secs):
        ev = []
        for n in self.notes:
            ev += ["on", str(n), str(self.vel)]
        ev += ["run", str(self.hold)]
        for n in self.notes:
            ev += ["off", str(n)]
        n = int(secs * FS / BLK) - self.hold
        if self.wake is not None:
            w = int(self.wake * FS / BLK) - self.hold
            ev += ["run", str(w), "on", str(self.notes[0]), str(self.vel), "run", str(self.hold), "off", str(self.notes[0])]
            n -= w + self.hold
        return ev + ["run", str(max(n, 1))]


def cases():
    c = []
    S, M, U = "s", "m", "u"
    pad = dict(PAD, **RV)
    for bld, env in ((S, FULL), (S, HALF), (M, FULL), (U, FULL), (U, HALF)):
        tag = bld + ("f" if env is FULL else "h")
        c.append(T("pad_d104_" + tag, "pad, reverb decay 104", pad, bld, env, due=20))
        c.append(T("pad_d120_" + tag, "pad, reverb decay 120", dict(pad, FX_DECAY=120), bld, env, secs=60, due=55))
    for bld, env in ((S, FULL), (M, FULL), (U, FULL)):
        tag = bld + "f"
        c.append(T("pad_d112_" + tag, "pad, reverb decay 112", dict(pad, FX_DECAY=112), bld, env, secs=40, due=35))
        c.append(T("quiet_" + tag, "a soft note (velocity 12, patch volume 40), decay 112",
                   dict(pad, FX_DECAY=112, VOL=40), bld, env, notes=[60], vel=12, secs=30, due=25))
        c.append(T("eqmax_" + tag, "EQ every band +16 dB at the bass, boost 127, phaser feedback 127",
                   dict(pad, FX_DECAY=112, EQ_MID=127, EQ_MIDF=32, EQ_Q=0, EQ_LOW=127, EQ_LOWF=64, EQ_HIGH=127,
                        BOOST=127, PH_MIX=40, PH_FB=127, PH_STAGES=5), bld, env, secs=40, due=38))
        c.append(T("mode3_" + tag, "reverb mode 3 (feedback 100), the longest pre-delay (depth 0: 16531 frames)",
                   dict(pad, FX_MODE=3, FX_FB=100, FX_TIME=64, FX_DEPTH=0, FX_DECAY=104), bld, env, secs=40, due=35))
        c.append(T("mode4_" + tag, "reverb mode 4, decay 112", dict(pad, FX_MODE=4, FX_DECAY=112), bld, env, secs=40, due=35))
        c.append(T("wake_" + tag, "decay 104, a second note at 20 s (after the rest)", pad, bld, env, notes=[60],
                   secs=30, due=None, wake=20.0))
        c.append(T("held127_" + tag, "decay 127 (held): no rest, as before", dict(pad, FX_DECAY=127), bld, env, notes=[60],
                   secs=12, due=None, same=True))
    m16 = dict(FULL, **NOLEND)
    c.append(T("pad_d104_m16", "the 16-bit ring (Bluetooth on): its fade", pad, M, m16, kind="fade", due=None))
    c.append(T("pad_d120_m16", "the 16-bit ring, decay 120: its fade", dict(pad, FX_DECAY=120), M, m16, secs=60,
               kind="fade", due=None))
    c.append(T("quiet_m16", "the 16-bit ring, a soft note, decay 112: its fade", dict(pad, FX_DECAY=112, VOL=40), M, m16,
               notes=[60], vel=12, kind="fade", due=None))
    c.append(T("fb110_m16", "the 16-bit ring, mode 3, feedback 110 (1.18C40: a limit cycle, no rest)",
               dict(pad, FX_MODE=3, FX_FB=110, FX_TIME=30), M, m16, secs=30, due=15, kind="pdz"))
    c.append(T("fb100m4_m16", "the 16-bit ring, mode 4, feedback 100 (1.18C40: a limit cycle, no rest)",
               dict(pad, FX_MODE=4, FX_FB=100, FX_TIME=20), M, m16, secs=30, due=15, kind="pdz"))
    c.append(T("fb64_m16", "the 16-bit ring, mode 3, feedback 64", dict(pad, FX_MODE=3, FX_FB=64, FX_TIME=20), M, m16,
               secs=30, due=15, kind="pdz"))
    c.append(T("wake_m16", "the 16-bit ring: a note in the middle of the fade", pad, M, m16, notes=[60], secs=30,
               kind="fadewake", due=None))
    c.append(T("held127_m16", "the 16-bit ring, decay 127 (held): no gate, as before", dict(pad, FX_DECAY=127), M, m16,
               notes=[60], secs=12, due=None, same=True))
    c.append(T("pad_d104_mh", "HALF, Bluetooth's memory lent: the 32-bit build's float HALF", pad, M, HALF, kind="eq"))
    c.append(T("pad_d104_mh16", "HALF, Bluetooth on: the 32-bit build's float HALF", pad, M, dict(HALF, **NOLEND),
               kind="eq"))
    c.append(T("pad_d120_mh16", "HALF, Bluetooth on, decay 120: the 32-bit build's float HALF", dict(pad, FX_DECAY=120),
               M, dict(HALF, **NOLEND), secs=60, due=40, kind="eq"))
    for bld in (S, M):
        c.append(T("delay_fb127_" + bld, "delay, feedback 127, time 127: not changed",
                   dict(PAD, FX_MODE=1, FX_SEND=86, FX_TIME=127, FX_FB=127), bld, {}, notes=[60], secs=30, due=None, same=True))
    return c


def patch_cases(name, raw):
    out = []
    for bld, env, tag in (("s", FULL, "s"), ("m", FULL, "mf"), ("m", dict(FULL, **NOLEND), "m16"), ("u", FULL, "u"),
                          ("m", HALF, "mh"), ("m", dict(HALF, **NOLEND), "mh16")):
        kind = "fade" if tag == "m16" else "eq" if tag in ("mh", "mh16") else None
        out.append(T("%s_%s" % (name, tag), name + " one short G3", {}, bld, env, notes=[55], hold=37, secs=40,
                     due=30, raw=raw, kind=kind))
        out.append(T("%s_q_%s" % (name, tag), name + " one soft G3 (velocity 32)", {}, bld, env, notes=[55], vel=32,
                     hold=37, secs=40, due=30, raw=raw, kind=kind))
    out.append(T("%s_w_m16" % name, name + " a note in the middle of the 16-bit ring's fade", {}, "m",
                 dict(FULL, **NOLEND), notes=[55], hold=37, secs=40, raw=raw, kind="fadewake", due=None))
    return out


def read24(path):
    w = wave.open(path)
    raw = w.readframes(w.getnframes())
    b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
    v = b[:, 0] | b[:, 1] << 8 | b[:, 2] << 16
    return ((v << 8) >> 8).reshape(-1, 2)


def render(engine, case, secs, tmp, tag):
    pf = os.path.join(tmp, tag + ".raw")
    open(pf, "wb").write(case.patch())
    wav = os.path.join(tmp, tag + ".wav")
    env = dict(os.environ, SY_REST="1", **case.env)
    env.pop("INPUT", None)
    r = subprocess.run([engine, wav, "preset", pf] + case.events(secs), env=env, capture_output=True)
    if r.returncode:
        raise RuntimeError("%s: rc %d %s" % (case.name, r.returncode, r.stderr.decode()[-300:]))
    rest = [int(x.split()[1]) for x in r.stderr.decode().splitlines() if x.startswith("rest ")][0]
    x = read24(wav)
    os.unlink(wav)
    os.unlink(pf)
    return x, (rest * BLK / FS if rest >= 0 else None)


def db(x):
    return 20 * math.log10(x) if x > 0 else float("-inf")


def norm_tail(x, secs):
    """the last `secs` before the signal ends (its last sample above 0), normalised to -1 dBFS"""
    nz = np.nonzero(np.abs(x).max(axis=1))[0]
    end = (nz[-1] + 1 + FS // 2) if len(nz) else len(x)
    y = x[max(0, end - int(secs * FS)):end].astype(np.float64)
    m = np.abs(y).max()
    return y * (0.891 * 8388607.0 / m) if m > 0 else y


def write24(path, y):
    v = np.clip(np.round(y), -8388608, 8388607).astype(np.int32).reshape(-1)
    b = np.stack([v & 255, (v >> 8) & 255, (v >> 16) & 255], axis=1).astype(np.uint8).tobytes()
    w = wave.open(path, "wb")
    w.setnchannels(2); w.setsampwidth(3); w.setframerate(FS)
    w.writeframes(b)
    w.close()


def last_sound(x):
    nz = np.nonzero(np.abs(x).max(axis=1))[0]
    return (nz[-1] + 1) if len(nz) else 0


def win_rms_db(x, n):
    k = len(x) // n
    y = x[:k * n].astype(np.float64).reshape(k, n, 2) / 8388608.0
    return 10 * np.log10((y ** 2).mean(axis=(1, 2)) + 1e-30)


def one_fade(d, case, a):
    """kind fade / fadewake (the module's doc): returns the row of one()"""
    import copy
    new, c40, ng = (os.path.join(d, "m_" + t) for t in ("new", "c40", "ng"))
    ok, why = True, []
    with tempfile.TemporaryDirectory() as tmp:
        x, to = render(c40, case, case.secs, tmp, "o")
        gate = last_sound(x)            # (1.18C40's gate: its last sound, the block its clear began)
        cw = case
        if case.kind == "fadewake":
            cw = copy.copy(case)
            cw.wake = (gate // BLK * BLK) / FS + 0.4
            x, to = render(c40, cw, cw.secs, tmp, "o")
            z, _ = render(ng, cw, cw.secs, tmp, "g")
        y, tn = render(new, cw, cw.secs, tmp, "n")
    dd = np.abs(x - y).max(axis=1)
    nzd = np.nonzero(dd)[0]
    first = nzd[0] / FS if len(nzd) else None
    pk = db(dd.max() / 8388608.0) if len(nzd) else float("-inf")
    lvl = db(np.abs(x[max(0, gate - FS // 10):gate]).max() / 8388608.0) if gate else None   # (the old's last 100 ms)
    if case.kind != "pdz" and len(nzd) and nzd[0] < gate - GATE_BLKS * BLK:
        ok = False; why.append("differs at %.3f s, before 1.18C40's gate (%.3f s)" % (nzd[0] / FS, gate / FS))
    info = ""
    if case.kind == "pdz":
        ra, rb = win_rms_db(x, FS), win_rms_db(y, FS)
        dv = [abs(ra[i] - rb[i]) for i in range(min(len(ra), len(rb))) if ra[i] > -60.0]
        mv = max(dv) if dv else 0.0
        if mv > a.pdz_db:
            ok = False; why.append("a second's level %.2f dB off 1.18C40's" % mv)
        gate = last_sound(y)            # (the end: the new one's own gate)
        info = "levels within %.2f dB; " % mv
    if case.kind in ("fade", "pdz"):
        e = last_sound(y)
        endpk = db(np.abs(y[max(0, e - FS // 50):e]).max() / 8388608.0) if e else float("-inf")
        oldpk = db(np.abs(x[max(0, gate - FS // 50):gate]).max() / 8388608.0) if gate else float("-inf")
        r = win_rms_db(y[max(0, gate - 2 * FS):e + FS // 50], FS // 50)
        drops = [r[i] - r[i + 1] for i in range(len(r) - 1) if r[i] > -130.0]
        mx = max(drops) if drops else 0.0
        info += "end %.1f (old %.1f) dBFS, drop %.1f dB/20 ms" % (endpk, oldpk, mx)
        if endpk > a.end_db:
            ok = False; why.append("the end's last 20 ms at %.1f dBFS > %.1f" % (endpk, a.end_db))
        if mx > a.drop_db:
            ok = False; why.append("a drop of %.1f dB in 20 ms > %.1f" % (mx, a.drop_db))
        if case.kind == "fade" and to is not None and (tn is None or tn > to + FADE_S + 0.05):
            ok = False; why.append("rest %s > 1.18C40's %.2f s + the fade" % ("never" if tn is None else "%.2f s" % tn, to))
    else:
        w0 = int(cw.wake * FS / BLK) * BLK   # (the note's on: the first block of the wake)
        g0 = gate // BLK * BLK - GATE_BLKS * BLK   # (the gate's block: its fade begins in it)
        g2 = last_sound(x) // BLK * BLK - BLK   # (the second note's tail at its gate: compared up to there)
        dz = np.abs(y[:g2] - z[:g2]).max(axis=1)
        nzz = np.nonzero(dz)[0]
        if len(nzz) and (nzz[0] < g0 or nzz[-1] >= w0 + (WAKE_BLKS + GATE_BLKS) * BLK):
            ok = False; why.append("differs from the un-gated engine outside the fade (%.3f .. %.3f s)" %
                                    (nzz[0] / FS, nzz[-1] / FS))
        zp = np.abs(z[g0:w0]).max()
        if len(nzz) and dz.max() > zp * 1.413:
            ok = False; why.append("|new - un-gated| %.1f dBFS above the un-gated tail's peak %.1f + 3 dB" %
                                    (db(dz.max() / 8388608.0), db(zp / 8388608.0)))
        info = "note at %.2f s; vs un-gated: %s (tail there %.1f dBFS pk)" % (
            cw.wake, "identical" if not len(nzz) else "%.3f .. %.3f s, pk %.1f" % (nzz[0] / FS, nzz[-1] / FS,
                                                                                 db(dz.max() / 8388608.0)), db(zp / 8388608.0))
    if case.due is not None and (tn is None or tn > case.due):
        ok = False; why.append("rest %s > %.0f s" % ("never" if tn is None else "%.1f s" % tn, case.due))
    if a.wavs:
        os.makedirs(a.wavs, exist_ok=True)
        write24(os.path.join(a.wavs, case.name + "_c40.wav"), norm_tail(x, 4.0))
        write24(os.path.join(a.wavs, case.name + "_new.wav"), norm_tail(y, 4.0))
    return case, first, lvl, pk, to, tn, ok, "; ".join(why + ([info] if ok and info else []))


def one_eq(d, case, a):
    """kind eq: HALF on the float ring, lent or not the same; against 1.18C40's 16-bit HALF"""
    import copy
    other = copy.copy(case)
    other.env = dict(case.env)
    if "SY_NOLEND" in other.env:
        other.env.pop("SY_NOLEND")
    else:
        other.env["SY_NOLEND"] = "1"
    with tempfile.TemporaryDirectory() as tmp:
        y, tn = render(os.path.join(d, "m_new"), case, case.secs, tmp, "n")
        z, _ = render(os.path.join(d, "m_new"), other, case.secs, tmp, "l")
        x, to = render(os.path.join(d, "m_c40"), case, case.secs, tmp, "o")
    ok, why = True, []
    if len(z) != len(y) or np.any(z != y):
        ok = False; why.append("lent and not lent differ")
    n = min(len(x), len(y))
    dd = np.abs(x[:n] - y[:n]).max(axis=1)
    nzd = np.nonzero(dd)[0]
    ra, rb, rd = win_rms_db(x[:n], FS), win_rms_db(y[:n], FS), win_rms_db(x[:n] - y[:n], FS)
    lv = [abs(ra[i] - rb[i]) for i in range(len(ra)) if ra[i] > -60.0]
    mv, md = (max(lv) if lv else 0.0), float(rd.max())
    if mv > a.pdz_db:
        ok = False; why.append("a second's level %.2f dB off 1.18C40's" % mv)
    if md > a.half_db:
        ok = False; why.append("a second's difference %.1f dBFS rms > %.1f" % (md, a.half_db))
    if ok:
        why.append("lent = not lent; levels within %.2f dB of 1.18C40, the difference <= %.1f dBFS rms a second" % (mv, md))
    if case.due is not None and (tn is None or tn > case.due):
        ok = False; why.append("rest %s > %.0f s" % ("never" if tn is None else "%.1f s" % tn, case.due))
    pk = db(dd.max() / 8388608.0) if len(nzd) else float("-inf")
    return case, (nzd[0] / FS if len(nzd) else None), None, pk, to, tn, ok, "; ".join(why)


def one(d, case, a):
    if case.kind in ("fade", "fadewake", "pdz"):
        return one_fade(d, case, a)
    if case.kind == "eq":
        return one_eq(d, case, a)
    old, new = os.path.join(d, case.builds + "_old"), os.path.join(d, case.builds + "_new")
    with tempfile.TemporaryDirectory() as tmp:
        y, tn = render(new, case, case.secs, tmp, "n")
        secs = case.secs
        if a.long and not case.same:
            secs = 200.0
        x, to = render(old, case, secs, tmp, "o")
    if len(x) > len(y):
        y = np.concatenate([y, np.zeros((len(x) - len(y), 2), np.int32)])
    dd = np.abs(x - y).max(axis=1)
    nzd = np.nonzero(dd)[0]
    first = nzd[0] / FS if len(nzd) else None
    pk = db(dd.max() / 8388608.0) if len(nzd) else float("-inf")
    lvl = db(np.abs(x[nzd[0]:nzd[0] + FS // 10]).max() / 8388608.0) if len(nzd) else None
    ok, why = True, []
    if pk > a.max_db:
        ok = False; why.append("difference %.1f dB > %.1f" % (pk, a.max_db))
    if case.same and len(nzd):
        ok = False; why.append("not bit-identical")
    if not case.same and len(nzd) and lvl > a.max_db - 18.0:
        ok = False; why.append("differs at %.2f s where the old is at %.1f dBFS (a tail cut)" % (first, lvl))
    if case.due is not None and (tn is None or tn > case.due):
        ok = False; why.append("rest %s > %.0f s" % ("never" if tn is None else "%.1f s" % tn, case.due))
    if a.wavs and not case.same:
        os.makedirs(a.wavs, exist_ok=True)
        write24(os.path.join(a.wavs, case.name + "_old.wav"), norm_tail(x, 4.0))
        write24(os.path.join(a.wavs, case.name + "_new.wav"), norm_tail(y, 4.0))
    return case, first, lvl, pk, to, tn, ok, "; ".join(why)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dir"); ap.add_argument("--max-db", type=float, default=-92.0)
    ap.add_argument("--long", action="store_true"); ap.add_argument("--patch", action="append", default=[])
    ap.add_argument("--cases", default=""); ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--wavs", default="")
    ap.add_argument("--end-db", type=float, default=-120.0); ap.add_argument("--drop-db", type=float, default=6.0)
    ap.add_argument("--pdz-db", type=float, default=0.5); ap.add_argument("--half-db", type=float, default=-75.0)
    a = ap.parse_args()
    cs = cases()
    for p in a.patch:
        n, f = p.split("=", 1)
        cs += patch_cases(n, Path(f).read_bytes())
    if a.cases:
        want = a.cases.split(",")
        cs = [c for c in cs if any(c.name.startswith(w) for w in want)]
    with ThreadPoolExecutor(a.jobs) as ex:
        res = list(ex.map(lambda c: one(a.dir, c, a), cs))
    f = lambda t: "-" if t is None else "%.1f" % t
    print("%-18s %8s %9s %9s %8s %8s  %s" % ("case", "1st diff", "old there", "diff pk", "rest old", "rest new", "result"))
    bad = 0
    for c, first, lvl, pk, to, tn, ok, why in res:
        to_s = f(to) if to is not None else (">%.0f" % (200.0 if a.long and not c.same else c.secs))
        print("%-18s %8s %9s %9s %8s %8s  %s" % (c.name, f(first), "-" if lvl is None else "%.1f" % lvl,
              "-inf" if pk == float("-inf") else "%.1f" % pk, to_s, f(tn), ("ok" + (" (" + why + ")" if why else ""))
              if ok else "FAIL: " + why))
        bad += not ok
    print("%d cases: %d ok, %d failed (limit %.1f dBFS)" % (len(res), len(res) - bad, bad, a.max_db))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
