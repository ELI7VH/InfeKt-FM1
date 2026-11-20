#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""shimwidth.py BASE ENG:W[:G][,ENG:W[:G]...] [CASES] [--glide LOGENG]: the shimmer's stereo width over time
 on lab renderers (two parts, the lent float ring). BASE is the engine without WIDTH
(-DRVS_WIDTH=0, 1.18C46's) or any, for the references: the plain reverb (AMOUNT 0), today (WIDTH 0); each ENG at WIDTH
W and W GLIDE G (0..15, default 5: 1 s). Per case and phase (while playing; the tail 0-1.5, 1.5-4, 4-8 s): the
energy-weighted L/R correlation of the shimmer part (the render minus AMOUNT 0) and of the wet (minus FX SEND 0); the
shimmer part's level, its mono sum and mono / stereo ratio against today. CASES: pad, stab, arp, pad2, pad64 (default
all). --glide LOGENG (built -DCLN_RVS_LOG -DRVS_WLOG: the side's gain a block in the log): (1.18C47) W GLIDE 0, 5, 15
at WIDTH 127 on the pad (two decays), the stab and repeated chords: when the opening starts after the note-off, its
10-90 % and whole times, the side gain's largest change per 10 and per 100 ms, the wet's correlation in 0.5 s steps.
Measures only, no pass / fail."""
import os, sys, subprocess, tempfile, wave
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostcases  # noqa: E402

FS = 46875.
NB = lambda s: str(int(s * FS / 128))
TMP = tempfile.mkdtemp()


def pad(decay=110, room=3, send=100):
    return {"OSC_BAL": 64, "OSC_VOL": 40, "OSC2_DET": 64, "AE_A": 40, "AE_D": 90, "AE_S": 127, "AE_R": 70, "CUT1": 90,
            "FX_MODE": 2, "FX_SEND": send, "FX_TIME": 40, "FX_DECAY": decay, "FX_DEPTH": room, "FX_COLOUR": 64}


def stab(decay=116, room=3, send=100):
    p = pad(decay, room, send); p.update({"AE_A": 0, "AE_D": 60, "AE_S": 60, "AE_R": 30, "CUT1": 100}); return p


def ev_pad(hold=4., tail=10.):
    ch = (48, 55, 60, 64)
    return (["run", "10"] + sum([["on", str(n), "100"] for n in ch], []) + ["run", NB(hold)]
            + sum([["off", str(n)] for n in ch], []) + ["run", NB(tail)])


def ev_stab(tail=12.):
    ch = (52, 59, 64, 67)
    return (["run", "10"] + sum([["on", str(n), "110"] for n in ch], []) + ["run", NB(0.15)]
            + sum([["off", str(n)] for n in ch], []) + ["run", NB(tail)])


def read24(p):
    w = wave.open(p)
    b = np.frombuffer(w.readframes(w.getnframes()), np.uint8).reshape(-1, 3).astype(np.int32)
    v = b[:, 0] | b[:, 1] << 8 | b[:, 2] << 16
    return (((v << 8) >> 8) / 8388608.).reshape(-1, 2)


def render(eng, sets, shim, ev, env=None):
    pf = os.path.join(TMP, "p%d.raw" % os.getpid()); out = os.path.join(TMP, "o%d.wav" % os.getpid())
    open(pf, "wb").write(hostcases.Case("w", "", sets, []).patch())
    e = dict(os.environ, SY_RVQ="0", SY_SHIM=shim, **(env or {}))
    subprocess.run([eng, out, "preset", pf] + ev, env=e, check=True)
    x = read24(out); os.unlink(out); os.unlink(pf)
    return x


def width(x, win=0.5):
    """per window: correlation, side/mid energy (dB), level (dB, L+R energy)"""
    n = int(FS * win); k = len(x) // n
    L = x[:k * n, 0].reshape(k, n); R = x[:k * n, 1].reshape(k, n)
    ll, rr, lr = (L * L).sum(1), (R * R).sum(1), (L * R).sum(1)
    c = lr / np.sqrt(ll * rr + 1e-30)
    m, s = ll + rr + 2 * lr, ll + rr - 2 * lr
    sm = 10 * np.log10((s + 1e-30) / (m + 1e-30))
    lv = 10 * np.log10((ll + rr) / n / 2 + 1e-30)
    return c, sm, lv


def mono_change(x1, x0):
    """the mono sum's level change of the shimmer part when made wide (x1) against today's (x0), dB, over the whole render"""
    m1 = ((x1[:, 0] + x1[:, 1]) ** 2).sum(); m0 = ((x0[:, 0] + x0[:, 1]) ** 2).sum()
    return 10 * np.log10(m1 / m0)


def coh(x, win=1.0, lo=300., hi=8000., nfft=2048):
    """per window: the energy-weighted magnitude-squared coherence between L and R in lo..hi (Welch, nfft), and the
    IACC-like max |normalized cross-correlation| over lags +-1 ms"""
    from scipy.signal import csd, welch
    n = int(FS * win); k = len(x) // n; C = []; I = []
    lag = int(0.001 * FS)
    for j in range(k):
        L = x[j * n:(j + 1) * n, 0]; R = x[j * n:(j + 1) * n, 1]
        f, sxy = csd(L, R, FS, nperseg=nfft); _, sxx = welch(L, FS, nperseg=nfft); _, syy = welch(R, FS, nperseg=nfft)
        b = (f >= lo) & (f <= hi)
        C.append((np.abs(sxy[b]) ** 2).sum() / np.sqrt((sxx[b] ** 2).sum() * (syy[b] ** 2).sum() + 1e-60)
                 if False else (np.abs(sxy[b]) ** 2 / (sxx[b] * syy[b] + 1e-60) * (sxx[b] + syy[b])).sum() / ((sxx[b] + syy[b]).sum() + 1e-60))
        den = np.sqrt((L * L).sum() * (R * R).sum()) + 1e-30
        cc = [np.dot(L[lag:-lag], np.roll(R, s)[lag:-lag]) / den for s in range(-lag, lag + 1, 4)]
        I.append(np.max(np.abs(cc)))
    return np.array(C), np.array(I)


def ild_spread(x, win=0.5, sub=0.025):
    """per window: the std of the inter-channel level difference (dB) over its sub-windows (the images' movement)"""
    n = int(FS * win); m = int(FS * sub); k = len(x) // n; out = []
    for j in range(k):
        L = x[j * n:(j + 1) * n, 0][: n // m * m].reshape(-1, m); R = x[j * n:(j + 1) * n, 1][: n // m * m].reshape(-1, m)
        d = 10 * np.log10(((L * L).sum(1) + 1e-30) / ((R * R).sum(1) + 1e-30))
        out.append(np.std(d))
    return np.array(out)


def arp(decay=110, room=3, send=100):
    p = pad(decay, room, send); p.update({"AE_A": 0, "AE_D": 50, "AE_S": 40, "AE_R": 25, "CUT1": 95, "OSC_VOL": 60}); return p


def ev_arp(sec=4., tail=8.):
    seq = (60, 64, 67, 72, 76, 72, 67, 64); dt = 0.125; ev = ["run", "10"]; t = 0.; i = 0
    while t < sec:
        n = str(seq[i % 8]); ev += ["on", n, "100", "run", NB(dt * 0.6), "off", n, "run", NB(dt * 0.4)]; t += dt; i += 1
    return ev + ["run", NB(tail)]


CASES = {
    "pad": ("PAD r3 d110 OCT A100", pad(), ev_pad(), "100,0,100,64,0,%d,0,57",
            (("hold", 1, 4), ("t0-1.5", 4.2, 5.6), ("t1.5-4", 5.6, 8.1), ("t4-8", 8.1, 12.1))),
    "stab": ("STAB r3 d116 OCT A100", stab(), ev_stab(10.), "100,0,100,64,0,%d,0,57",
             (("t0-1.5", 0.3, 1.75), ("t1.5-4", 1.75, 4.25), ("t4-8", 4.25, 8.25))),
    "arp": ("ARP r3 d110 OCT A100", arp(), ev_arp(), "100,0,100,64,0,%d,0,57",
            (("play", 1, 4), ("t0-1.5", 4.1, 5.5), ("t1.5-4", 5.5, 8.), ("t4-8", 8., 12.))),
    "pad2": ("PAD r2 d120 OCT+5TH A127", pad(120, 2), ev_pad(), "127,1,100,64,0,%d,0,57",
             (("hold", 1, 4), ("t0-1.5", 4.2, 5.6), ("t1.5-4", 5.6, 8.1), ("t4-8", 8.1, 12.1))),
    "pad64": ("PAD r1 d100 OCT A64", pad(100, 1), ev_pad(), "64,0,100,64,0,%d,0,57",
              (("hold", 1, 4), ("t0-1.5", 4.2, 5.6), ("t1.5-4", 5.6, 8.1), ("t4-8", 8.1, 12.1))),
}


def corr(x, a, b):
    L = x[int(a * FS):int(b * FS), 0]; R = x[int(a * FS):int(b * FS), 1]
    return (L * R).sum() / np.sqrt((L * L).sum() * (R * R).sum() + 1e-60)


def ms(a):
    return ((a[:, 0] + a[:, 1]) ** 2).sum() / (2 * (a ** 2).sum())


def shx(sh, w, g=5):
    """the controls with WIDTH w, W GLIDE g (TAIL's byte, bits 1-4: g ^ 5)"""
    v = (sh % w).split(",")
    v[6] = str(int(v[6]) | ((g ^ 5) & 15) << 1)
    return ",".join(v)


def ev_rep(n=3, hold=1.6, gap=2.4, tail=5.):
    """repeated chords (a fifth apart): the re-narrowing"""
    ch = (48, 55, 60, 64); ev = ["run", "10"]
    for j in range(n):
        c = [x + (5 if j % 2 else 0) for x in ch]
        ev += sum([["on", str(m), "100"] for m in c], []) + ["run", NB(hold)] + sum([["off", str(m)] for m in c], [])
        ev += ["run", NB(gap if j < n - 1 else tail)]
    return ev


def glide(logeng, base):
    B = 128 / FS
    sh = "100,0,100,64,0,%d,0,57"

    def run(sets, s, ev):
        lg = os.path.join(TMP, "lg%d.txt" % os.getpid())
        x = render(logeng, sets, s, ev, env={"SY_SHLOG": lg})
        g = np.loadtxt(lg)[:, 5]; os.unlink(lg)
        return x, g

    def step(g, sec):
        m = max(1, int(round(sec / B)))
        return np.max(np.abs(g[m:] - g[:-m])) if len(g) > m else 0.

    def c25(x):
        n = int(FS * 0.25); k = len(x) // n
        L = x[:k * n, 0].reshape(k, n); R = x[:k * n, 1].reshape(k, n)
        return (L * R).sum(1) / np.sqrt((L * L).sum(1) * (R * R).sum(1) + 1e-30)
    for name, sets, ev, rel in (("PAD r3 d110", pad(110, 3), ev_pad(3., 8.), 3.), ("PAD r2 d120", pad(120, 2), ev_pad(3., 8.), 3.),
                                ("STAB r3 d116", stab(116, 3), ev_stab(8.), 0.15)):
        t0 = 10 * B + rel
        dry = render(base, dict(sets, FX_SEND=0), sh % 0, ev)
        c0 = c25(render(base, sets, sh % 0, ev) - dry)
        tc = (np.arange(len(c0)) + 0.5) * 0.25 - t0
        sel = [j for j in range(len(c0)) if -1. < tc[j] < 7.5][::2]
        print("%s, WIDTH 127 (the note-off at 0; the wet's correlation, 0.25 s windows at)" % name)
        print("  %-14s" % "t (s)" + " ".join("%5.1f" % tc[j] for j in sel))
        print("  %-14s" % "today (W0)" + " ".join("%5.2f" % c0[j] for j in sel))
        for g in (0, 5, 15):
            x, gg = run(sets, shx(sh, 127, g), ev)
            t = np.arange(len(gg)) * B
            j0 = int(round(t0 / B)); lo = gg[j0]; hi = gg.max(); af = t > t0
            st = t[np.argmax(af & (gg > lo + 1e-6))] - t0
            i10 = t[np.argmax(af & (gg >= lo + 0.1 * (hi - lo)))] - t0; i90 = t[np.argmax(af & (gg >= lo + 0.9 * (hi - lo)))] - t0
            e = t[np.argmax(af & (gg >= hi - 1e-6))] - t0
            m = t >= t0 + st - 0.05
            c = c25(x - dry)
            print("  %-14s" % ("W GLIDE %d" % g) + " ".join("%5.2f" % c[j] for j in sel))
            print("  %14s side gain %.2f -> %.2f: starts %.2f s after the note-off, 10-90 %% %.2f-%.2f s, whole %.2f s; "
                  "largest change per 10 ms %.4f, per 100 ms %.3f" % ("", lo, hi, st, i10, i90, e - st, step(gg[m], 0.01), step(gg[m], 0.1)))
    print("repeated chords (PAD r3 d110, 1.6 s on, 2.4 s off, three), WIDTH 127: the side gain every 0.4 s | largest change per 10 / 100 ms")
    for g in (0, 5, 15):
        x, gg = run(pad(110, 3), shx(sh, 127, g), ev_rep())
        t = np.arange(len(gg)) * B; m = t > 1.
        print("  W GLIDE %2d: %s | %.4f / %.3f" % (g, " ".join("%.2f" % gg[int(round(q / B))] for q in np.arange(0, 14, 0.4)),
                                                  step(gg[m], 0.01), step(gg[m], 0.1)))


args = sys.argv[1:]
logeng = None
if "--glide" in args:
    i = args.index("--glide"); logeng = args[i + 1]; del args[i:i + 2]
base = args[0]
specs = [a.split(":") for a in args[1].split(",")] if len(args) > 1 and args[1] else []
sel = args[2].split(",") if len(args) > 2 else (list(CASES) if specs else [])
for key in sel:
    title, sets, ev, sh, spans = CASES[key]
    eng0 = base
    dry = render(eng0, dict(sets, FX_SEND=0), sh % 0, ev)
    x00 = render(eng0, sets, "0" + sh[sh.index(","):] % 0, ev)
    x0 = render(eng0, sets, sh % 0, ev)
    print("%-26s %s" % (title, "  ".join("%7s" % s[0] for s in spans)) + "   (corr: shimmer part / wet)")
    print("  %-24s" % "plain reverb" + "  ".join("%7s" % ("  %5.2f" % corr(x00 - dry, a, b)) for _, a, b in spans))
    print("  %-24s" % "today (W0)" + "  ".join("%7s" % ("%.2f/%.2f" % (corr(x0 - x00, a, b), corr(x0 - dry, a, b))) for _, a, b in spans))
    for sp in specs:
        eng, w, g = sp[0], int(sp[1]), int(sp[2]) if len(sp) > 2 else 5
        if w == 0:
            continue
        x1 = render(eng, sets, shx(sh, w, g), ev)
        p1, p0 = x1 - x00, x0 - x00
        print("  %-24s" % ("%s W%d G%d" % (os.path.basename(eng), w, g)) + "  ".join("%7s" % ("%.2f/%.2f" % (corr(p1, a, b), corr(x1 - dry, a, b))) for _, a, b in spans)
              + "   lvl %+.1f dB, mono %+.1f dB, mono/st %+.1f dB" % (10 * np.log10((p1 ** 2).sum() / (p0 ** 2).sum()),
                                                                       mono_change(p1, p0), 10 * np.log10(ms(p1) / ms(p0))))

if logeng:
    glide(logeng, base)

import shutil  # noqa: E402
shutil.rmtree(TMP, ignore_errors=True)
