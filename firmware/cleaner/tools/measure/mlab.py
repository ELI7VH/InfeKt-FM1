# SPDX-License-Identifier: GPL-3.0-only
"""The cleaner's measuring helpers: an engine rendered as a black box through the lab's command line
    ENGINE OUT.wav preset FILE run N on NOTE VEL off NOTE midi STATUS D1 D2 ...   ($INPUT: the audio input, int32 L/R)
Every fitted number of the cleaner comes from such renders (PROVENANCE.md): the engine measured is an executable,
nothing of its source or tables is read.
    HYB  the development engine's host engine (default: the scratchpad build), CLN the cleaner's (cleaner/build/engine)
    MLAB_REF  the folder with data/init_patch.raw (default: the the specification team repository's reference/)
    MLAB_BANK the test bank's patches (default: that repository's reference/patches)"""
import os, subprocess, tempfile, wave
import numpy as np
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
REF = os.environ.get('MLAB_REF', '/path/to/spec/reference')
BANK = os.environ.get('MLAB_BANK', os.path.join(REF, 'patches'))
HYB = os.environ.get('HYB', '/tmp/infekt/cl/eng_hyb')
CLN = os.environ.get('CLN', os.path.join(ROOT, 'cleaner', 'build', 'engine'))
FS = 46875.0
BLOCK = 128
PB = 128
# page A bytes used here
OSC1_SHAPE, OSC1_PW, OSC1_WAVE, OSC2_SHAPE, OSC_BAL, SUB_VOL, OSC_VOL, NOISE_VOL = 17, 18, 19, 22, 33, 34, 36, 37
CUTOFF, CUTOFF2, F1_RESO, F2_RESO, F1_ENV, F2_ENV, F1_KF, F2_KF, F_BAL, SAT_TYPE = 40, 41, 42, 43, 44, 45, 46, 47, 48, 49
F1_MODE, F2_MODE, F_ROUTE = 51, 52, 53
LFO1_RATE, LFO1_SHAPE, LFO1_KEYTRIG, LFO2_RATE, LFO2_SHAPE, LFO2_PAN = 67, 68, 73, 79, 80, 90
INPUT_MODE, INPUT_SELECT, CHORUS_MIX, FX_MODE, FX_SEND = 101, 102, 105, 112, 113
DIST_TYPE, DIST_INT = PB + 100, PB + 101


def init_preset():
    return bytearray(open(os.path.join(REF, 'data', 'init_patch.raw'), 'rb').read())


def bank():
    d = BANK
    return [(f[:-4], bytearray(open(os.path.join(d, f), 'rb').read())) for f in sorted(os.listdir(d)) if f.endswith('.raw')]


def read_wav(path):
    w = wave.open(path)
    n, ch, sw = w.getnframes(), w.getnchannels(), w.getsampwidth()
    b = np.frombuffer(w.readframes(n), dtype=np.uint8).reshape(-1, 3)
    v = (b[:, 0].astype(np.int32) | b[:, 1].astype(np.int32) << 8 | b[:, 2].astype(np.int32) << 16)
    return (((v << 8) >> 8).astype(np.float64) / (1 << 23)).reshape(-1, ch)


def render(exe, preset, events, inp=None, pre=30, env=None):
    """the audio after the patch is in (pre blocks before the events dropped): float (frames, 2). events: ('on', n, v),
    ('off', n), ('midi', st, d1, d2), ('run', blocks), ('preset', bytes). inp: (frames, 2) at the input, from the events'
    start (pre blocks of zeros before it)"""
    with tempfile.TemporaryDirectory() as t:
        pf = os.path.join(t, 'p.raw'); open(pf, 'wb').write(bytes(preset))
        args = [exe, os.path.join(t, 'o.wav'), 'preset', pf, 'run', str(pre)]
        k = 0
        for e in events:
            if e[0] == 'preset':
                k += 1; q = os.path.join(t, 'p%d.raw' % k); open(q, 'wb').write(bytes(e[1])); args += ['preset', q]
            else:
                args += [str(x) for x in e]
        e2 = dict(os.environ, **(env or {}))
        e2.pop('INPUT', None)
        if inp is not None:
            w = np.zeros((pre * BLOCK + len(inp), 2))
            w[pre * BLOCK:] = inp
            q = np.clip(np.round(w * 8388608.0), -8388608, 8388607).astype('<i4')
            fn = os.path.join(t, 'in.raw'); q.tofile(fn); e2['INPUT'] = fn
        r = subprocess.run(args, capture_output=True, env=e2)
        if r.returncode:
            raise RuntimeError(r.stderr.decode())
        return read_wav(os.path.join(t, 'o.wav'))[pre * BLOCK:]


def render_many(exe, jobs, workers=8):
    """jobs: (preset, events[, inp]) each"""
    with ThreadPoolExecutor(workers) as ex:
        return list(ex.map(lambda j: render(exe, *j), jobs))


def units(sec):
    return int(round(sec * FS / BLOCK))


def db(x):
    return 20 * np.log10(np.maximum(np.abs(x), 1e-12))


def rms(x):
    return float(np.sqrt(np.mean(np.asarray(x) ** 2)))


def diff_db(a, b):
    """the difference's RMS against the signal's (dB; -inf: bit-identical)"""
    n = min(len(a), len(b)); d = a[:n] - b[:n]
    e = rms(d)
    return -np.inf if e == 0 else 20 * np.log10(e / (rms(a[:n]) + 1e-15))


def band_db(x, nfft=4096):
    """third-octave band levels (dB) of a signal (both channels summed in power), 25 Hz - 20 kHz"""
    x = np.asarray(x)
    if x.ndim == 2: x = x.T
    else: x = x[None]
    P = 0
    for c in x:
        if len(c) < nfft: c = np.pad(c, (0, nfft - len(c)))
        segs = len(c) // (nfft // 2) - 1
        w = np.hanning(nfft)
        for s in range(segs):
            P = P + np.abs(np.fft.rfft(c[s * nfft // 2:s * nfft // 2 + nfft] * w)) ** 2
    f = np.fft.rfftfreq(nfft, 1 / FS)
    cs = 1000 * 2 ** (np.arange(-16, 14) / 3)
    out = []
    for fc in cs:
        m = (f >= fc * 2 ** (-1 / 6)) & (f < fc * 2 ** (1 / 6))
        out.append(10 * np.log10(np.sum(P[m]) + 1e-20) if m.any() else np.nan)
    return cs, np.array(out)
