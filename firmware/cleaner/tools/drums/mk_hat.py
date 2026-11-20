#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""mk_hat.py: the drum kit's sampled hi-hat (MODEL SMP on CH / OH, docs/drum-hat-sample.md) from a WAV file.

The WAV holds two hits, a closed and an open hat (the times below or --closed / --open). Each hit is stored as
  - its low part: the hit decimated to RATE (11.025 kHz up), DC removed, pre-emphasised for the player's linear
    interpolation and its low-pass (so the played spectrum is flat up to about 0.48 RATE at TUNE 0), 8-bit mu-law
    (--bits 8) or 4-bit PCM with a scale (1/8 octave a step) per block of 16 samples (--bits 4, 4.5 bits a sample);
  - two envelopes in frames of FLS stored samples (about 3 ms): the level of the hit above 0.9 x RATE / 2 (the band the
    player fills with high-passed noise) and of its low part (for the tail), the sample's noise floor removed from
    both (so they reach zero), 1/16 octave a step;
  - its tail: a frame (the anchor), the two levels there and the slope of the decay before it (the player continues
    the hit from the anchor with noise when DECAY is longer than 100 %); the frame of its peak (a DECAY below 100 %
    fades from there).
The end of a hit is where its level reaches the noise floor (+6 dB), or the end of its segment (the open hat, cut),
with a short fade there. The output is a C header for fm1/app.c (default fm1/hat/hat_tab.h). The build includes it when it is there; without it the model is
not offered.

  python3 mk_hat.py hat.wav                                  # -> ../../fm1/hat/hat_tab.h (8 bit, 22.05 kHz)
  python3 mk_hat.py hat.wav --bits 4 --out ../../fm1/hat/hat_tab4.h   # the small one (4 bit)
  python3 mk_hat.py hat.wav --closed 0:0.2525 --open 0.2525:  # the hits' segments in seconds (end empty: the file's)
  python3 mk_hat.py hat.wav --dump DIR                        # also the stored low parts as WAV files (a check)

Needs numpy and scipy.
"""
import argparse
import math
import os
import sys

import numpy as np
import scipy.io.wavfile as wavfile
import scipy.signal as sig

FS = 44100                               # the player's rate (the FM-1's): the stored rate is FS / DIV
HP_K = 0.9566                            # the noise band from HP_K x RATE / 2 up (10.55 kHz at 22.05 kHz: the prototype's split)
LP_K = 0.5                               # the player's low-pass after the interpolation: LP_K x RATE (x the pitch)
FRAME_S = 0.003                          # the envelopes' frames, seconds (rounded to stored samples)
FADE_S = 0.008                           # the fade at a hit's end, seconds
ANCHOR_DB = -30.                         # a hit that decays into its floor: its tail's anchor this far below its peak
FIT_FRAMES = 14                          # the slope: fitted over this many frames before the anchor
END_DB = -60.                            # the tail ends this far below the hit's peak (the player: te)
Q4 = (0.54119610, 1.30656296)            # a 4th-order Butterworth's two sections


def mulaw_enc(v, mu=255.):
    y = np.sign(v) * np.log1p(mu * np.abs(v)) / np.log1p(mu)
    q = np.clip(np.round(np.abs(y) * 127), 0, 127).astype(int)
    return np.where(v < 0, 128 | q, q).astype(np.uint8)


def mulaw_tab(mu=255.):                  # the magnitudes of codes 0..127, int16 (the player's table)
    q = np.arange(128) / 127.
    return np.round(32767 * ((1 + mu) ** q - 1) / mu).astype(int)


BLK = 16                                 # 4-bit: samples a block (one scale byte each)


def bpcm_enc(x):                         # x in -1..1: 4-bit codes c (the value (2 c - 15) / 15 x the block's scale
    codes, sb, out = [], [], np.zeros(len(x))   # 2^(-e / 8)), the scale bytes e, the decoded values (app.c hat_next)
    lv = (2. * np.arange(16) - 15.) / 15.
    for k in range(0, len(x), BLK):
        b = x[k:k + BLK]
        e0 = int(round(-8. * math.log2(max(float(np.abs(b).max()), 1e-6))))
        best = None
        for e in range(max(0, e0 - 3), min(255, e0 + 6) + 1):
            sc = 2. ** (-e / 8.)
            c = np.argmin(np.abs(b[:, None] / sc - lv[None, :]), axis=1)
            err = float(np.sum((lv[c] * sc - b) ** 2))
            if best is None or err < best[0]:
                best = (err, e, c, lv[c] * sc)
        _, e, c, d = best
        codes += list(c)
        sb.append(e)
        out[k:k + BLK] = d
    return codes, sb, out


def frames_rms(x, fl):
    n = (len(x) + fl - 1) // fl
    y = np.zeros(n * fl)
    y[:len(x)] = x
    return np.sqrt(np.mean(y.reshape(n, fl) ** 2, axis=1))


def env_bytes(g):                        # levels -> (scale, bytes: scale x 2^(-b / 16), 255 silence)
    gs = float(g.max()) if g.max() > 0 else 1.
    with np.errstate(divide="ignore"):
        b = np.round(-16. * np.log2(np.maximum(g, 1e-30) / gs))
    b = np.where(g <= gs * 2. ** (-254. / 16.), 255, np.clip(b, 0, 254)).astype(int)
    return gs, b


def env_dec(gs, b):
    return np.where(b == 255, 0., gs * 2. ** (-b / 16.))


def lp_resp(f, fc, fs):                  # |H| of the player's 4th-order Butterworth low-pass (bilinear)
    sos = sig.butter(4, fc / (fs / 2), "low", output="sos")
    _, h = sig.sosfreqz(sos, worN=f, fs=fs)
    return np.abs(h)


def encode_hit(name, seg, floor_f, floor_h, floor_l, div, bits):
    rate = FS / div
    fl = max(1, int(round(FRAME_S * rate)))   # a frame in stored samples
    flo = fl * div                       # in samples at FS
    hp = sig.butter(4, HP_K * rate / 2 / (FS / 2), "high", output="sos")
    # the end: the last 5 ms frame above the floor + 6 dB (the floor never reached: the segment's end, a cut)
    f5 = int(0.005 * FS)
    r5 = frames_rms(seg, f5)
    above = np.nonzero(r5 > 2. * floor_f)[0]
    end = min(len(seg), (int(above[-1]) + 1) * f5) if len(above) else len(seg)
    cut = end >= len(seg) - f5
    end = (end // flo) * flo if not cut else end
    nf_fade = int(FADE_S * FS)
    seg = seg[:end].copy()
    w = np.ones(end)
    w[end - nf_fade:] = 0.5 + 0.5 * np.cos(np.pi * np.arange(nf_fade) / nf_fade)
    seg *= w
    # the low part: decimated (an anti-aliased FIR), pre-emphasised for the linear interpolation and the player's
    # low-pass, up to 0.48 x rate (+12 dB at most)
    lo = sig.resample_poly(seg, 1, div)
    nfft = 1 << int(math.ceil(math.log2(len(lo) * 2 + 64)))
    X = np.fft.rfft(lo, nfft)
    f = np.fft.rfftfreq(nfft, 1. / rate)
    # (the interpolation at TUNE 0: DIV - 1 points between two samples, a triangle of 2 DIV - 1 taps at FS)
    with np.errstate(invalid="ignore", divide="ignore"):
        tri = np.where(f > 0, (np.sin(np.pi * f * div / FS) / (div * np.sin(np.pi * f / FS))) ** 2, 1.)
    comp = tri * lp_resp(np.minimum(f, rate * 0.5), LP_K * rate, FS)
    gain = np.where(f <= 0.48 * rate, 1. / np.maximum(comp, 10 ** (-12 / 20)), 1.)
    lo = np.fft.irfft(X * gain, nfft)[:len(lo)]
    pk = float(np.abs(lo).max())
    if bits == 8:
        codes = mulaw_enc(lo / pk)
        tab = mulaw_tab()
        dec = np.where(codes & 128, -1, 1) * tab[codes & 127] / 32767. * pk
        data, sb = list(codes), []
    else:
        cs, sb, d = bpcm_enc(lo / pk)
        dec = d * pk
        cs = cs + [0] * (len(cs) & 1)
        data = [cs[k] | cs[k + 1] << 4 for k in range(0, len(cs), 2)]
    n = len(lo)
    # the envelopes (a frame's value at its start, the player ramps between them): the band above the split (the
    # noise's level) and the low part as decoded, the floor removed
    hb = sig.sosfilt(hp, seg)
    gh = frames_rms(hb, flo)
    gh = np.sqrt(np.maximum(gh ** 2 - floor_h ** 2, 0.))
    gl = frames_rms(dec, fl)
    gl = np.sqrt(np.maximum(gl ** 2 - floor_l ** 2, 0.))
    nf = len(gh)
    gl = np.pad(gl, (0, max(0, nf - len(gl))))[:nf]
    ehs, eh = env_bytes(gh)
    els, el = env_bytes(gl)
    ghd, gld = env_dec(ehs, eh), env_dec(els, el)
    # the tail's anchor and slope: a cut hit at its last frame before the fade; a hit that decays into its floor where
    # its band's level is ANCHOR_DB below its peak
    pkh = ghd.max()
    if cut:
        kt = max(FIT_FRAMES, (end - nf_fade) // flo - 1)
    else:
        kk = np.nonzero(ghd < pkh * 10 ** (ANCHOR_DB / 20))[0]
        kk = kk[kk > int(np.argmax(ghd))]
        kt = int(kk[0]) if len(kk) else nf - 1
    k0 = max(int(np.argmax(ghd)) + 1, kt - FIT_FRAMES)
    ks = np.arange(k0, kt + 1)
    yh = np.log2(np.maximum(ghd[ks], 1e-12))
    sl, ic = np.polyfit(ks, yh, 1)
    sl = min(float(sl), -0.01)           # (log2 a frame: decaying, never flat)
    th = float(2. ** (ic + sl * kt))
    yl = np.log2(np.maximum(gld[ks], 1e-12))
    tl = float(2. ** np.mean(yl - sl * (ks - kt)))   # (the low part's level at the anchor, on the same slope)
    pk_all = max(pkh, gld.max())
    te = math.log2(10 ** (END_DB / 20) * pk_all / max(th, tl))
    if not cut:                          # (a hit that decays into its floor: its tail at 100 % reaches -60 dB where the
        sl = min(sl, te / max(1, nf - kt))   # hit ends, so a DECAY above 100 % lengthens it from there on)
    kp = int(np.argmax(ghd + gld))       # (the peak's frame: DECAY below 100 % fades from there)
    snr = 10. * math.log10(float(np.sum(lo ** 2)) / max(float(np.sum((dec - lo) ** 2)), 1e-30))
    info = dict(name=name, n=n, nf=nf, kt=kt, kp=kp, cut=cut, end_s=end / FS, fl=fl, sl=sl, th=th, tl=tl, te=te,
                slope_dbs=sl * 6.0206 * rate / fl, pk=pk, snr=snr)
    return dict(data=data, sb=sb, eh=eh, el=el, pk=pk, ehs=ehs, els=els, info=info, dec=dec, fl=fl)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wav")
    ap.add_argument("--closed", default="0:0.2525", help="the closed hit's segment, seconds A:B")
    ap.add_argument("--open", default="0.2525:", help="the open hit's segment, seconds A:B (B empty: the end)")
    ap.add_argument("--div", type=int, default=2, choices=(2, 3, 4), help="the stored rate: 44.1 kHz / DIV")
    ap.add_argument("--bits", type=int, default=8, choices=(8, 4), help="8 mu-law, 4 block-scaled 4-bit PCM")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "fm1",
                                                  "hat", "hat_tab.h"))
    ap.add_argument("--dump", help="a folder: the stored low parts as decoded, as WAV files (FS / DIV)")
    a = ap.parse_args()
    sr, x = wavfile.read(a.wav)
    x = x.astype(float)
    if x.ndim > 1:
        x = x.mean(axis=1)
    x /= 32768. if np.abs(x).max() > 2. else 1.
    if sr != FS:
        g = math.gcd(sr, FS)
        x = sig.resample_poly(x, FS // g, sr // g)
    x = sig.sosfilt(sig.butter(1, 15. / (FS / 2), "high", output="sos"), x)   # (the DC)
    # the floor: the median of the quietest 15 % of 5 ms frames (the whole file), full band and the noise band
    f5 = int(0.005 * FS)
    rf = frames_rms(x, f5)
    rate = FS / a.div
    rh = frames_rms(sig.sosfilt(sig.butter(4, HP_K * rate / 2 / (FS / 2), "high", output="sos"), x), f5)
    rl = frames_rms(sig.sosfilt(sig.butter(4, HP_K * rate / 2 / (FS / 2), "low", output="sos"), x), f5)
    q = max(1, int(0.15 * len(rf)))
    floor_f, floor_h, floor_l = (float(np.median(np.sort(r)[:q])) for r in (rf, rh, rl))
    hits = []
    for name, spec in (("closed", a.closed), ("open", a.open)):
        s0, s1 = spec.split(":")
        i0 = int(float(s0) * FS)
        i1 = int(float(s1) * FS) if s1 else len(x)
        hits.append(encode_hit(name, x[i0:i1], floor_f, floor_h, floor_l, a.div, a.bits))
    fl = hits[0]["fl"]
    data, env, sbs = [], [], []
    rows = []
    for h in hits:
        rows.append((len(data), h["info"]["n"], len(env), h["info"]["nf"], len(sbs), h))
        data += h["data"]
        env += list(h["eh"]) + list(h["el"])
        sbs += h["sb"]
    out = ["/* The drum kit's sampled hi-hat (docs/drum-hat-sample.md),",
           " * generated by tools/drums/mk_hat.py from %s: */" % os.path.basename(a.wav),
           "#define HAT_TAB 1",
           "#define HAT_BITS %du                    /* 8: mu-law; 4: 4-bit PCM, a scale a block of HAT_BLK */" % a.bits,
           "#define HAT_BLK %du" % BLK,
           "#define HAT_RATE %.1ff               /* the stored rate, Hz */" % rate,
           "#define HAT_FLS %du                    /* a frame of the envelopes, stored samples */" % fl,
           "#define HAT_HPK %.3ff                 /* the noise band from HAT_HPK x HAT_RATE / 2 */" % HP_K,
           "#define HAT_LPK %.3ff                 /* the low-pass after the interpolation at HAT_LPK x HAT_RATE */" % LP_K,
           "static const hat_hit_t HAT_H[2] = {   /* d, n, e, nf, kt, kp, s, cut; pk, ehs, els; sl, th, tl, te */"]
    for d0, n, e0, nf, s0, h in rows:
        i = h["info"]
        out.append("    {%du, %du, %du, %du, %du, %du, %du, %du, %.6gf, %.6gf, %.6gf, %.6gf, %.6gf, %.6gf, %.6gf},   /* %s */"
                   % (d0, n, e0, nf, i["kt"], i["kp"], s0, int(i["cut"]), h["pk"], h["ehs"], h["els"], i["sl"], i["th"], i["tl"],
                      i["te"], i["name"]))
    out.append("};")
    out.append("static const uint8_t HAT_E[%d] = {   /* each hit: nf frames of the noise band, nf of the low part */"
               % len(env))
    for k in range(0, len(env), 24):
        out.append("    " + ", ".join(str(v) for v in env[k:k + 24]) + ",")
    out.append("};")
    if a.bits == 8:
        t = mulaw_tab()
        out.append("static const int16_t HAT_MU[128] = {   /* mu-law (255): codes 0..127, the magnitude x 32767 */")
        for k in range(0, 128, 16):
            out.append("    " + ", ".join(str(int(v)) for v in t[k:k + 16]) + ",")
        out.append("};")
    if a.bits == 4:
        out.append("static const uint8_t HAT_SB[%d] = {   /* 4 bit: each block's scale, 2^(-e / 8) */" % len(sbs))
        for k in range(0, len(sbs), 24):
            out.append("    " + ", ".join(str(int(v)) for v in sbs[k:k + 24]) + ",")
        out.append("};")
    out.append("static const uint8_t HAT_D[%d] = {   /* the low parts, one after the other */" % len(data))
    for k in range(0, len(data), 24):
        out.append("    " + ", ".join(str(int(v)) for v in data[k:k + 24]) + ",")
    out.append("};")
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "w") as fo:
        fo.write("\n".join(out) + "\n")
    for h in hits:
        i = h["info"]
        print("%-6s %5d samples (%.3f s%s), %3d frames, peak %d, anchor %3d, slope %.0f dB/s, SNR %.1f dB"
              % (i["name"], i["n"], i["end_s"], ", cut" if i["cut"] else "", i["nf"], i["kp"], i["kt"], i["slope_dbs"],
                 i["snr"]))
    print("data %d B, scales %d B, envelopes %d B -> %s" % (len(data), len(sbs), len(env), a.out))
    if a.dump:
        os.makedirs(a.dump, exist_ok=True)
        for h in hits:
            y = np.clip(h["dec"] * 32767., -32768, 32767).astype(np.int16)
            wavfile.write(os.path.join(a.dump, "hat_%s_low.wav" % h["info"]["name"]), int(rate), y)
    return 0


if __name__ == "__main__":
    sys.exit(main())
