# SPDX-License-Identifier: GPL-3.0-only
"""perc_rv.py Q OUT.json [GROUPS] | perc_rv.py --cmp OLD.json NEW.json [--md]: perceptual measurements of a reverb as
a black box (rvlib.py; Q = full, half, half16 of the development engine's renderer, nfull, nhalf, nhalf16 of the own one;
CLN_RV_TUNE in the environment passes on to the own one). The band-averaged metrics of meas_rv.py missed what the ear
heard in 1.18C5 (pitch-dependent ringing, the damping, the tail's modulation); these look at it finely. Groups:

  pitch  per-pitch ringing: a short sine tone (0.3 s) on every semitone of MIDI 24-108 into the reverb (the tones an
         octave apart share a render: the reverb is linear, a band of +-1/6 octave around each tone isolates it; 12
         renders a setting). Per tone, from its end: the tail's level (50-250 ms after, against the tone's own level
         at the input) and its decay (T20 of the band's EDC, -5..-25 dB). Per setting the development engine's trend over pitch (a
         running median of 7 semitones) is the reference in --cmp: the resonance score is the 95th percentile over the
         tones of own T / development engine trend about its median, divided by the same of the development engine itself (1: no tone rings
         longer than the development engine's own spread; > 1.15 audible ringing), the level score the same in dB (own excess
         over the trend about its median, less the development engine's), and the largest single excess (which tone)
  fine   the fine damping curve: four impulses (at other phases of any modulation), their band energies averaged; T30
         (-5..-35 dB, else T20) in 1/12-octave bands 40 Hz - 16 kHz (1/6-octave: two bands' energies summed); the tail's
         spectrum over time (the level per 1/3 octave in 0.05-0.2, 0.2-0.6, 0.6-1.5, 1.5-3 s against 1 kHz: how the
         HF / LF damping evolves); the modal structure of the late tail (a 1 s window from 0.3 s, 1/2 Hz bins): peaks
         per Hz with a prominence >= 6 dB (per octave), and the spectrum's roughness (std of the dB spectrum about its
         1/3-octave trend: 5.6 dB for a diffuse, Rayleigh tail; more: sparse modes)
  mod    the modulation's character: eight sines (wobble.py) 1 s into the setting; the tail cut by level into early /
         middle / end (modtime_rv.py's windows); per partial the instantaneous frequency's deviation (cents), its
         spectrum 0.25-16 Hz: the rate distribution (the power's shares in the octaves 0.25-0.5 .. 8-16 Hz), its median
         rate, the spectral flatness (geometric / arithmetic mean; 1 noise, 0 one line) and the normalised entropy;
         the medians over the partials and both sides
Settings: rooms 0-3 at decay 104 and rooms 1, 3 at decay 64 (mode 2, colour 64, send 127), in the quality Q."""
import sys, os, json
import numpy as np
from scipy import signal
import rvlib as R
import rvana as A
import wobble
import modtime_rv as MT
FS = R.FS
SET = [(0, 104), (1, 104), (2, 104), (3, 104), (1, 64), (3, 64)]
NOTES = list(range(24, 109))


def hz(n):
    return 440.0 * 2 ** ((n - 69) / 12)


def secs(decay):
    return 7.5 if decay >= 100 else 4.0


# ---- pitch ----
TON, TLEN, AMP = 0.05, 0.3, 0.05


def tone_input(notes, T):
    x = np.zeros((int(T * FS), 2)); n = int(TLEN * FS); t = np.arange(n) / FS
    e = np.minimum(1, np.minimum(t, TLEN - t) / 0.005)
    s = sum(np.sin(2 * np.pi * hz(k) * t + k) for k in notes) * AMP * e
    a = int(TON * FS); x[a:a + n, 0] = x[a:a + n, 1] = s
    return x


def band_env(y, f, bw=1 / 6):
    lo, hi = f * 2 ** -bw, min(f * 2 ** bw, 0.45 * FS)
    sos = signal.butter(3, [lo, hi], 'bandpass', fs=FS, output='sos')
    b = signal.sosfiltfilt(sos, y, axis=0)
    return (b ** 2).sum(1)


def tone_stats(y, f):
    e = band_env(y, f)
    t1 = int((TON + TLEN) * FS)
    ref = 10 * np.log10(AMP ** 2 + 1e-30)          # (the tone at the input: a sine's power, both sides' sum / 2)
    lev = 10 * np.log10(e[t1 + int(0.05 * FS):t1 + int(0.25 * FS)].mean() / 2 + 1e-30) - ref
    edc = A.schroeder(e[t1:])
    t20 = A.decay_fit(edc, -5, -25)
    return float(lev), float(t20) if np.isfinite(t20) else None


def g_pitch(exe, env):
    items, keys = [], []
    for room, dec in SET:
        for o in range(12):
            ns = [k for k in NOTES if (k - NOTES[0]) % 12 == o]
            items.append((R.patch(room=room, decay=dec), [('run', R.mlab.units(secs(dec)))], tone_input(ns, secs(dec)), 30, env))
            keys.append((room, dec, ns))
    ys = R.mlab.render_many(exe, items, workers=4)
    res = {}
    for (room, dec, ns), y in zip(keys, ys):
        d = res.setdefault('r%d_d%d' % (room, dec), {})
        for k in ns:
            d[str(k)] = tone_stats(y, hz(k))
    return res


# ---- fine ----
def bands12(lo=40.0, hi=16000.0):
    n = int(np.floor(12 * np.log2(hi / lo))) + 1
    return [lo * 2 ** (i / 12) for i in range(n)]


def g_fine(exe, env, half):
    out = {}
    B = [f for f in bands12() if f * 2 ** (1 / 24) < (0.45 if not half else 0.22) * FS]
    for room, dec in SET:
        T = secs(dec) + 1.0
        items = []
        for k in range(4):
            x = np.zeros((int(T * FS) + 256, 2)); at = 256 + k * 9973; x[at, :] = 0.5
            items.append((R.patch(room=room, decay=dec), [('run', R.mlab.units(T))], x, 30, env))
        ys = R.mlab.render_many(exe, items, workers=4)
        ats = [256 + k * 9973 for k in range(4)]
        ys = [y[a:a + int((T - 0.3) * FS)] for y, a in zip(ys, ats)]
        nmin = min(len(y) for y in ys); ys = [y[:nmin] for y in ys]
        E = []
        for f in B:
            lo, hi = f * 2 ** (-1 / 24), f * 2 ** (1 / 24)
            sos = signal.butter(3, [lo, hi], 'bandpass', fs=FS, output='sos')
            E.append(np.mean([(signal.sosfiltfilt(sos, y, axis=0) ** 2).sum(1) for y in ys], 0))
        E = np.array(E)
        t12 = []
        for e in E:
            edc = A.schroeder(e); t = A.decay_fit(edc, -5, -35)
            if not np.isfinite(t): t = A.decay_fit(edc, -5, -25)
            t12.append(float(t) if np.isfinite(t) else None)
        t6 = []
        for i in range(0, len(B) - 1, 2):
            edc = A.schroeder(E[i] + E[i + 1]); t = A.decay_fit(edc, -5, -35)
            if not np.isfinite(t): t = A.decay_fit(edc, -5, -25)
            t6.append(float(t) if np.isfinite(t) else None)
        # the spectrum over time: 1/3 octave (three 1/12 bands... four: summed) against the 1 kHz band
        ev = {}
        i1k = int(np.argmin(np.abs(np.log2(np.array(B) / 1000.0))))
        for nm, (a, b) in (('w0', (0.05, 0.2)), ('w1', (0.2, 0.6)), ('w2', (0.6, 1.5)), ('w3', (1.5, 3.0))):
            ia, ib = int(a * FS), int(b * FS)
            L = 10 * np.log10(E[:, ia:ib].mean(1) + 1e-30)
            ev[nm] = [float(v) for v in (L - L[i1k])]
        # the modal structure of the late tail
        a = int(0.3 * FS); nwin = int(1.0 * FS)
        P = np.mean([np.abs(np.fft.rfft(y[a:a + nwin].sum(1) * np.hanning(nwin), 2 * nwin)) ** 2 for y in ys], 0)
        P1 = np.abs(np.fft.rfft(ys[0][a:a + nwin].sum(1) * np.hanning(nwin), 2 * nwin)) ** 2
        fr = np.fft.rfftfreq(2 * nwin, 1 / FS)
        Ld = 10 * np.log10(P1 + 1e-30)
        modal = {}
        for fc in (63, 125, 250, 500, 1000, 2000, 4000):
            m = (fr >= fc / 2 ** 0.5) & (fr < fc * 2 ** 0.5)
            seg = Ld[m]
            w = max(3, int(len(seg) / 6))
            trend = np.array([np.median(seg[max(0, i - w):i + w + 1]) for i in range(len(seg))])
            pk, _ = signal.find_peaks(seg, prominence=6.0)
            modal[str(fc)] = dict(pph=float(len(pk) / (fr[m][-1] - fr[m][0])), rough=float(np.std(seg - trend)))
        out['r%d_d%d' % (room, dec)] = dict(f12=[float(f) for f in B], t12=t12, t6=t6, ev=ev, modal=modal)
    return out


# ---- mod ----
def if_spec(x, f, a, b):
    fi, en = A.inst_freq(x, f, bw=0.02 * f)
    g = fi[a:b]; w = en[a:b]
    k = w > 0.1 * np.median(w)
    if k.mean() < 0.5:
        return None
    c = 1200 * np.log2(np.maximum(g, 1) / f)
    med = np.median(c[k]); c = np.where(k, c, med) - med
    dev = float(np.sqrt(np.mean(c[k] ** 2)))
    cc = signal.decimate(signal.decimate(c, 10), 10)
    fs = FS / 100
    Fm, P = signal.welch(cc - cc.mean(), fs=fs, nperseg=min(len(cc), int(8 * fs)), noverlap=None)
    m = (Fm >= 0.25) & (Fm <= 16)
    Fm, P = Fm[m], P[m] + 1e-30
    sh = [float(P[(Fm >= lo) & (Fm < 2 * lo)].sum() / P.sum()) for lo in (0.25, 0.5, 1, 2, 4, 8)]
    cum = np.cumsum(P) / P.sum()
    mr = float(Fm[np.searchsorted(cum, 0.5)])
    # flatness / entropy on a log-frequency grid (each octave the same weight)
    lg = np.log2(Fm)
    grid = np.linspace(np.log2(0.25), np.log2(16), 25)
    Pl = np.array([P[(lg >= grid[i]) & (lg < grid[i + 1])].mean() if ((lg >= grid[i]) & (lg < grid[i + 1])).any() else np.nan
                   for i in range(24)])
    Pl = Pl[np.isfinite(Pl)]
    fl = float(np.exp(np.mean(np.log(Pl))) / np.mean(Pl))
    p = Pl / Pl.sum(); ent = float(-(p * np.log(p)).sum() / np.log(len(p)))
    return dev, mr, fl, ent, sh


def g_mod(exe, env):
    items, keys = [], []
    for room, dec in SET:
        if dec < 100: continue
        items.append((R.patch(room=room, decay=dec), [('run', R.mlab.units(12))], wobble.tones(12), 30, env))
        keys.append('r%d_d%d' % (room, dec))
    ys = R.mlab.render_many(exe, items, workers=4)
    res = {}
    for k, y in zip(keys, ys):
        y = y[640:]; t0 = int(1.05 * FS); r = {}
        for nm, ab in zip(MT.WN, MT.windows(y, t0)):
            if ab is None: continue
            a, b = ab
            st = [s for c in (0, 1) for f in wobble.F for s in [if_spec(y[:, c], f, a, b)] if s]
            if len(st) < 3: continue
            r[nm] = dict(dev=float(np.median([s[0] for s in st])), mrate=float(np.median([s[1] for s in st])),
                         flat=float(np.median([s[2] for s in st])), ent=float(np.median([s[3] for s in st])),
                         share=[float(v) for v in np.median(np.array([s[4] for s in st]), 0)])
        res[k] = r
    return res


# ---- the comparison ----
def runmed(v, w=7):
    v = np.asarray(v, float); out = np.full(len(v), np.nan)
    for i in range(len(v)):
        s = v[max(0, i - w // 2):i + w // 2 + 1]; s = s[np.isfinite(s)]
        if len(s): out[i] = np.median(s)
    return out


def cmp_pitch(a, b, md):
    rows = []
    for key in a:
        if key not in b: continue
        ns = sorted(int(k) for k in a[key])
        la = np.array([a[key][str(n)][0] for n in ns]); lb = np.array([b[key][str(n)][0] for n in ns])
        ta = np.array([a[key][str(n)][1] or np.nan for n in ns]); tb = np.array([b[key][str(n)][1] or np.nan for n in ns])
        tra, lra = runmed(np.log(ta)), runmed(la)
        rh = np.exp(np.log(ta) - tra); ro = np.exp(np.log(tb) - tra)
        # (the scores about each one's median: the overall level and decay are meas_rv.py's; these are the peaks)
        sT = (np.nanpercentile(ro, 95) / np.nanmedian(ro)) / (np.nanpercentile(rh, 95) / np.nanmedian(rh))
        sL = (np.nanpercentile(lb - lra, 95) - np.nanmedian(lb - lra)) - (np.nanpercentile(la - lra, 95) - np.nanmedian(la - lra))
        i = int(np.nanargmax(ro)); j = int(np.nanargmax(lb - lra))
        rows.append((key, sT, sL, ns[i], ro[i], np.nanmax(rh), ns[j], (lb - lra)[j], np.nanmax(la - lra),
                     np.nanstd(np.log(tb) - runmed(np.log(tb))), np.nanstd(np.log(ta) - tra),
                     np.nanmedian(tb / ta), np.nanmedian(lb - la)))
    if md:
        print('| setting | resonance score (T) | level score (dB) | longest own tone: note, T / development engine trend (development engine max) | loudest own tone: note, dB over trend (development engine max) | T median own / hyb | level median (dB) |')
        print('|---|---|---|---|---|---|---|')
        for r in rows:
            print('| %s | %.2f | %+.1f | %d, %.2f (%.2f) | %d, %+.1f (%+.1f) | %.2f | %+.1f |' % (r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[11], r[12]))
    else:
        for r in rows:
            print('pitch %-8s score T %.2f  L %+5.1f dB | longest own: note %3d x%.2f (hyb max x%.2f) | loudest own: note %3d %+5.1f dB (hyb max %+5.1f) | logT spread own %.2f hyb %.2f | T med %.2f lev med %+.1f'
                  % r)
    return rows


def cmp_fine(a, b, md):
    for key in a:
        if key not in b: continue
        x, y = a[key], b[key]
        d6 = [100 * (q / p - 1) for p, q in zip(x['t6'], y['t6']) if p and q]
        f6 = [x['f12'][2 * i] * 2 ** (1 / 24) for i, (p, q) in enumerate(zip(x['t6'], y['t6'])) if p and q]
        d6 = np.array(d6); f6 = np.array(f6)
        mid = (f6 >= 100) & (f6 <= 8000)
        evd = {w: np.array(y['ev'][w]) - np.array(x['ev'][w]) for w in x['ev']}
        f12 = np.array(x['f12'])
        evs = ' '.join('%s %+.1f/%+.1f' % (w, np.median(evd[w][f12 < 300]), np.median(evd[w][(f12 > 3000) & (f12 < 8000)])) for w in evd)
        mo = ' '.join('%s:%.2f/%.2f,%.1f/%.1f' % (fc, x['modal'][fc]['pph'], y['modal'][fc]['pph'], x['modal'][fc]['rough'], y['modal'][fc]['rough'])
                      for fc in x['modal'])
        if md:
            print('| %s | %.1f / %.1f %% | %.0f Hz | %s |' % (key, np.max(np.abs(d6[mid])), np.median(np.abs(d6[mid])), f6[mid][np.argmax(np.abs(d6[mid]))], evs))
        else:
            print('fine %-8s T(1/6 oct, 100 Hz-8 kHz) max %5.1f %% at %5.0f Hz, median %4.1f %% | LF/HF over time (own-hyb dB) %s' % (
                key, np.max(np.abs(d6[mid])), f6[mid][np.argmax(np.abs(d6[mid]))], np.median(np.abs(d6[mid])), evs))
            print('     modal (peaks/Hz hyb/own, roughness dB hyb/own) %s' % mo)
            print('     T 1/6 oct %%: ' + ' '.join('%.0f:%+.0f' % (f, d) for f, d in zip(f6, d6)))


def cmp_mod(a, b, md):
    for key in a:
        for w in MT.WN:
            x, y = a[key].get(w), b.get(key, {}).get(w)
            if not x or not y: continue
            sx = ' '.join('%.2f' % v for v in x['share']); sy = ' '.join('%.2f' % v for v in y['share'])
            if md:
                print('| %s | %s | %.1f / %.1f | %.1f / %.1f | %.2f / %.2f | %.2f / %.2f |' % (key, w, x['dev'], y['dev'], x['mrate'], y['mrate'], x['flat'], y['flat'], x['ent'], y['ent']))
            else:
                print('mod %-8s %-6s dev %5.1f %5.1f  med rate %4.1f %4.1f  flat %.2f %.2f  ent %.2f %.2f  shares [%s] [%s]' % (
                    key, w, x['dev'], y['dev'], x['mrate'], y['mrate'], x['flat'], y['flat'], x['ent'], y['ent'], sx, sy))


if __name__ == '__main__':
    if sys.argv[1] == '--cmp':
        a, b = json.load(open(sys.argv[2])), json.load(open(sys.argv[3])); md = '--md' in sys.argv
        if 'pitch' in a and 'pitch' in b: cmp_pitch(a['pitch'], b['pitch'], md)
        if 'fine' in a and 'fine' in b: cmp_fine(a['fine'], b['fine'], md)
        if 'mod' in a and 'mod' in b: cmp_mod(a['mod'], b['mod'], md)
        sys.exit(0)
    q, out = sys.argv[1], sys.argv[2]
    groups = sys.argv[3].split(',') if len(sys.argv) > 3 else ['pitch', 'fine', 'mod']
    exe, env = R.engines(q)
    env = dict(env)
    if q.startswith('n') and os.environ.get('CLN_RV_TUNE'): env['CLN_RV_TUNE'] = os.environ['CLN_RV_TUNE']
    res = json.load(open(out)) if os.path.exists(out) and '--add' in sys.argv else {}
    for g in groups:
        res[g] = g_pitch(exe, env) if g == 'pitch' else g_fine(exe, env, 'half' in q) if g == 'fine' else g_mod(exe, env)
        print(g, 'done', flush=True)
        json.dump(res, open(out, 'w'))
