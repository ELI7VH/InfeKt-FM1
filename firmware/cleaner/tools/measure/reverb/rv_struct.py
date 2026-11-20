# SPDX-License-Identifier: GPL-3.0-only
"""rv_struct.py: the development engine's reverb's structure as a black box (FULL, rvlib.py OLD), the numbers sy_cln_rv.c takes:
  the rooms' first arrivals (mode 4: the outputs' taps RV_TAPR / RV_TAPL; mode 2 one frame more),
  the diffusers: the first ~15 ms of the right output fitted as four allpasses in series (delays searched around the
    room-3 values scaled, gains by least squares): RV_DIF, CP_DG*, RV_S (the rooms' scale: room-3 delays against
    each room's),
  the tank's skeleton: the response at decay 127 with the diffusers taken out (divided by their allpass spectrum,
    exact: |H| = 1), its largest echoes per channel: the second (modulated, smeared) taps 729 / 803 frames after
    the first (TA_M, TB_M), the full-amplitude echoes through the other half (TB_L = right's echo less its tap,
    TA_L = left's echo less the right tap),
  the pre-delay caps (meas_rv.py "pre" measures them: RV_CAP), the LFO (modtrack2.py: CP_LFO_HZ, CP_LFO_PP)."""
import numpy as np
from scipy.optimize import least_squares
from scipy.signal import find_peaks
import itertools
import rvlib as R

D3 = (121, 165, 314, 442)


def ap(x, D, g):
    out = np.zeros_like(x); v = np.zeros(len(x))
    for n in range(len(x)):
        d = v[n - D] if n >= D else 0.0
        v[n] = x[n] + g * d; out[n] = d - g * v[n]
    return out


def diffusers(y, fa, Ds, n):
    yy = y[fa:fa + n, 1]
    def model(p, Dl):
        x = np.zeros(len(yy)); x[0] = p[0]
        for D, g in zip(Dl, p[1:]): x = ap(x, D, g)
        return x
    best = None
    for dd in itertools.product((-1, 0, 1), repeat=4):
        Dl = [a + b for a, b in zip(Ds, dd)]
        r = least_squares(lambda p: model(p, Dl) - yy, [0.05, 0.66, -0.66, 0.66, -0.62])
        e = np.sum(r.fun ** 2) / np.sum(yy ** 2)
        if best is None or e < best[0]: best = (e, Dl, r.x[1:])
    return best


def skeleton(y, Dl, g, n=1 << 16):
    f = np.fft.rfftfreq(n)
    H = np.ones_like(f, dtype=complex)
    for D, gg in zip(Dl, g):
        z = np.exp(-2j * np.pi * f * D); H *= (-gg + z) / (1 - gg * z)
    out = []
    for c in (1, 0):
        t = np.fft.irfft(np.fft.rfft(y[:n, c]) * np.conj(H), n); t /= np.abs(t[:400]).max()
        e = np.sqrt(np.convolve(t ** 2, np.ones(7), 'same'))
        pk = find_peaks(e[:6000], height=0.3, distance=20)[0]
        out.append([(int(p), round(float(e[p] * np.sign(t[p - 3 + np.argmax(np.abs(t[p - 3:p + 4]))])), 2)) for p in pk])
    return out


if __name__ == '__main__':
    import rvana as A
    caps = {}
    for room in (0, 1, 2, 3):   # (the pre-delay's cap: the first arrival at A 114 = 127 less the tap's, mode 2)
        y2 = R.run(R.OLD, R.patch(mode=2, decay=32, room=room, time=127), R.impulse(1), 1, env={'SY_RVQ': '1'})[640:]
        y4 = R.run(R.OLD, R.patch(mode=4, decay=32, room=room), R.impulse(1), 1, env={'SY_RVQ': '1'})[640:]
        caps[room] = A.first_arrival(y2, -30) - A.first_arrival(y4, -30)
    S = {r: (32768 - caps[r]) / (32768 - caps[3]) for r in caps}   # (the network's share of the ring, against room 3)
    print('pre-delay caps', caps, 'room scales', {r: round(v, 4) for r, v in S.items()})
    for room in (3, 0, 1, 2):
        y4 = R.run(R.OLD, R.patch(mode=4, decay=127, room=room), R.impulse(1), 1, env={'SY_RVQ': '1'})[640:]
        fr, fl = A.first_arrival(y4[:, 1:]), A.first_arrival(y4[:, :1])
        y = R.run(R.OLD, R.patch(decay=127, room=room), R.impulse(3), 3, env={'SY_RVQ': '1'})[640:]
        fa = A.first_arrival(y[:, 1:])
        Ds = [int(round(d * S[room])) for d in D3]
        e, Dl, g = diffusers(y, fa, Ds, int(700 * S[room]))
        sk = skeleton(y, Dl, g)
        print('room %d: taps (mode 4) right %d left %d; diffusers %s gains %s (residual %.3f); scale %.4f'
              % (room, fr, fl, Dl, np.round(g, 4).tolist(), e, np.mean(np.array(Dl) / np.array(D3))))
        print('   skeleton right', sk[0][:8]); print('   skeleton left ', sk[1][:8])
