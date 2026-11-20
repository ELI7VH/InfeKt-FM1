# SPDX-License-Identifier: GPL-3.0-only
"""rvana.py: room-acoustic metrics of a reverb's response (both channels), used on black-box renders (rvlib.py).
T60 / EDT per octave band from Schroeder's backward integral, band levels, first arrival, early energy, echo density
(Abel & Huang's normalized echo density), IACC per band and time window, the tail's pitch wobble (instantaneous
frequency of a sine's tail)."""
import numpy as np
from scipy import signal

FS = 46875.0
OCT = (63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000)


def _sos(fc, kind='oct'):
    lo, hi = fc / 2 ** 0.5, min(fc * 2 ** 0.5, 0.49 * FS)
    return signal.butter(4, [lo, hi], 'bandpass', fs=FS, output='sos')


def bandpass(x, fc):
    return signal.sosfiltfilt(_sos(fc), x, axis=0)


def schroeder(e):
    c = np.cumsum(e[::-1])[::-1]
    return 10 * np.log10(np.maximum(c / max(c[0], 1e-30), 1e-30))


def decay_fit(edc, a, b):
    """T60 from the EDC between a and b dB (a > b, negative); nan when not reached"""
    i = np.nonzero(edc <= a)[0]; j = np.nonzero(edc <= b)[0]
    if not len(i) or not len(j) or j[0] - i[0] < 8:
        return np.nan
    t = np.arange(i[0], j[0]) / FS
    k = np.polyfit(t, edc[i[0]:j[0]], 1)[0]
    return -60.0 / k if k < 0 else np.nan


def t60_bands(y, bands=OCT, noise_tail=True):
    """per band: (T30 from -5..-35 dB, else T20 -5..-25, EDT 0..-10), the band's energy (dB) and its share; y (n, 2)"""
    out = {}
    for fc in bands:
        if fc * 2 ** 0.5 >= 0.5 * FS:
            continue
        b = bandpass(y, fc)
        e = (b ** 2).sum(1)
        edc = schroeder(e)
        t30 = decay_fit(edc, -5, -35)
        t20 = decay_fit(edc, -5, -25)
        edt = decay_fit(edc, 0.0, -10)
        out[fc] = dict(t30=t30, t20=t20, edt=edt, lev=10 * np.log10(e.sum() + 1e-30))
    return out


def first_arrival(y, thr_db=-40):
    a = np.abs(y).max(1)
    m = a.max()
    i = np.nonzero(a > m * 10 ** (thr_db / 20))[0]
    return int(i[0]) if len(i) else -1


def echo_density(x, win=0.02):
    """normalized echo density (Abel & Huang 2006) of a mono response, a value per sample hop of win/4"""
    n = int(win * FS); h = n // 4
    w = np.hanning(n); w /= w.sum()
    erfc = 0.3173105078629141      # erfc(1/sqrt 2): the share of a Gaussian beyond one sigma
    st = np.arange(0, len(x) - n, h)
    if not len(st):
        return np.zeros(0), np.zeros(0)
    seg = np.lib.stride_tricks.sliding_window_view(x, n)[st]   # (vectorised: the same values as one window a time)
    sd = np.sqrt((seg ** 2) @ w)
    d = ((np.abs(seg) > sd[:, None]) @ w) / erfc
    t = (st + n / 2) / FS
    return np.array(t), np.array(d)


def iacc(y, fc=None, lag_ms=1.0):
    if fc:
        y = bandpass(y, fc)
    a, b = y[:, 0], y[:, 1]
    L = int(lag_ms * 1e-3 * FS)
    den = np.sqrt(np.dot(a, a) * np.dot(b, b)) + 1e-30
    c = signal.correlate(a, b, 'full', method='fft')
    m = len(b) - 1
    return float(np.max(np.abs(c[m - L:m + L + 1])) / den)


def inst_freq(x, f0, bw=None):
    """the instantaneous frequency (Hz) of a narrow band around f0 and its envelope"""
    bw = bw or max(40.0, 0.15 * f0)
    sos = signal.butter(4, [f0 - bw, f0 + bw], 'bandpass', fs=FS, output='sos')
    z = signal.hilbert(signal.sosfiltfilt(sos, x))
    ph = np.unwrap(np.angle(z))
    f = np.diff(ph) * FS / (2 * np.pi)
    return f, np.abs(z[1:])
