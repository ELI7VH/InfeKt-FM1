# SPDX-License-Identifier: GPL-3.0-only
"""co_design.py: the own oscillator's two designed parts (sy_cln_osc.inc), designs, not measurements:
taps(c1, c2): the table waves' filter (co_fir_taps): for each clock rate rho (ticks a sample) a symmetric 7-tap FIR at
    the sample rate whose response follows, in dB, the step response's own at f / (rho fs) (the measured C's taps times
    sinc^2 of a tick) up to 0.45 fs, the table waves' top; its response at 0 exactly 1. -> (R0, DR, rows of h0..h3)
    (the 6-voice state: taps(0, 0, R6_0, R6_D, R6_N), the clock at half its ticks: 0.79-1 ticks a sample)
clean(c1, c2): CLEAN's step (co_gc): the measured step response's own response up to GC_XP cycles a tick (0.4-0.5 fs
    at the clocks' 1.6-2 ticks a sample), nothing from GC_XS on, GC_L ticks either side of its middle, GC_R points a
    tick. (Chosen on a model of the saw: the lines below 16 kHz within 0.16 dB of ORIGINAL's, the alias energy against
    the lines 7-16 dB under ORIGINAL's at keys 60-108.)"""
import numpy as np

R0, DR, N = 1.50, 0.05, 13
R6_0, R6_D, R6_N = 0.75, 0.025, 13
GC_L, GC_XP, GC_XS, GC_R = 10.5, 0.25, 0.33, 16


def resp(x, c1, c2):
    s0, s1, s2 = c1, c2 - c1, 1 - 2 * c2
    return (s2 + 2 * s1 * np.cos(2 * np.pi * x) + 2 * s0 * np.cos(4 * np.pi * x)) * np.sinc(x) ** 2


def taps(c1, c2, r0=R0, dr=DR, n=N):
    f = np.linspace(0, 0.45, 500)                 # (cycles a sample)
    w = 2 * np.pi * f
    # h0 + 2 (h1 + h2 + h3) = 1 (the response at 0 exact): h0 = 1 - 2 (h1 + h2 + h3)
    A = np.stack([2 * np.cos(k * w) - 2 for k in range(1, 4)], 1)
    rows = []
    for i in range(n):
        rho = r0 + dr * i
        H = resp(f / rho, c1, c2)
        wt = 1 / np.maximum(np.abs(H), 1e-3) * np.where(f < 0.32, 4.0, 1.0)   # (relative error, the audible band first)
        c, *_ = np.linalg.lstsq(A * wt[:, None], (H - 1) * wt, rcond=None)
        rows.append(np.r_[1 - 2 * c.sum(), c])
    return r0, dr, rows


def err(c1, c2, *a):
    """the largest error (dB) below 0.32 and below 0.45 cycles a sample, over the rates"""
    r0, dr, rows = taps(c1, c2, *a)
    f = np.linspace(0, 0.45, 500); w = 2 * np.pi * f; out = []
    for i, h in enumerate(rows):
        G = h[0] + sum(2 * h[k] * np.cos(k * w) for k in range(1, 4))
        e = 20 * np.log10(np.abs(G / resp(f / (r0 + dr * i), c1, c2)))
        out.append((np.abs(e[f < 0.32]).max(), np.abs(e).max()))
    return np.array(out)


def clean(c1, c2):
    """1 - C of CLEAN's step: its response the measured one's (T sinc^2, resp) up to GC_XP cycles a tick, falling as a
    cosine^2 to 0 at GC_XS, nothing above; the kernel that response's (zero phase, GC_L ticks either side of its middle,
    a Blackman window), integrated"""
    n = int(2 * GC_L * GC_R)
    m = 32 * n
    t = np.linspace(0, 2 * GC_L, m + 1)
    xx = np.linspace(0, 0.5, 4001)
    Ht = resp(xx, c1, c2) * np.where(xx <= GC_XP, 1, np.where(xx >= GC_XS, 0, np.cos(np.pi / 2 * (xx - GC_XP) / (GC_XS - GC_XP)) ** 2))
    h = 2 * np.trapezoid(Ht[None, :] * np.cos(2 * np.pi * xx[None, :] * (t[:, None] - GC_L)), xx, axis=1) * np.blackman(m + 1)
    C = np.cumsum(h)
    C = (C - C[0]) / (C[-1] - C[0])
    return 1 - C[::32]                             # n + 1 points, 1 .. 0


if __name__ == '__main__':
    r0, dr, rows = taps(1 / 15, 5 / 15)
    for i, c in enumerate(rows):
        print(r0 + dr * i, c)
    print(err(1 / 15, 5 / 15))
