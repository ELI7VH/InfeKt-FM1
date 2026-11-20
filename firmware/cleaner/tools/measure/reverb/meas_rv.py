# SPDX-License-Identifier: GPL-3.0-only
"""meas_rv.py Q OUT.json [GROUPS]: the reverb's room metrics measured as a black box (rvlib.py: Q = full, half,
half16 of the development engine's renderer, nfull, nhalf, nhalf16 of the cleaner's). Groups (default all):
  rooms    rooms 0-3 x decay 0..127: per octave T30, EDT, level, early share (80 ms), IACC early / late; first arrival;
           echo density (Abel & Huang) at 2.5..80 ms after the first arrival; decay 127: dB/s per octave
  colour   rooms 3 and 0, decay 80, colour 0..127: third-octave levels against colour 64 (early 150 ms, late 1-2 s)
  pre      modes 2-4 x rooms x A 114: the first arrival
  fb       modes 3, 4, rooms 3 / 0, A 115: the repeats' period and level a repeat, the 3rd repeat's octave levels
  clock    B 20 = 1..19 at tempo 120 and 100 (B 16): the first arrival
  send     A 113 = 0..127: the dry and the wet level
  wobble   sines 250 / 1000 / 4000 Hz into rooms 0 / 3, decay 112: the tail's line width at -6 / -20 dB and its
           instantaneous frequency's spread (cents)
compare with cmp_rv.py."""
import sys, json
import numpy as np
from scipy import signal
import rvlib as R
import rvana as A
FS = R.FS
DECAYS = (0, 16, 32, 48, 64, 80, 96, 104, 112, 120)
ROOMS = (0, 1, 2, 3)


def T(d):
    return 1.5 if d <= 32 else 3 if d <= 64 else 6 if d <= 96 else 24 if d <= 112 else 40


def jobs_run(exe, env, items):
    jobs = [(p, [('run', R.mlab.units(secs))] if not ev else list(ev) + [('run', R.mlab.units(secs))], inp, 30, env)
            for p, inp, secs, ev in items]
    return R.mlab.render_many(exe, jobs)


def ir_metrics(y):
    y = y[640:]
    fa = A.first_arrival(y, -40)
    far = A.first_arrival(y[:, 1:], -40); fal = A.first_arrival(y[:, :1], -40)
    out = dict(first=fa, first_r=far, first_l=fal)
    tb = A.t60_bands(y)
    E = int(0.08 * FS)
    for fc, v in tb.items():
        b = A.bandpass(y, fc)
        e = (b ** 2).sum(1)
        v['early'] = float(10 * np.log10(e[:fa + E].sum() / (e.sum() + 1e-30) + 1e-30))
        v['iacc_e'] = A.iacc(b[fa:fa + E]); v['iacc_l'] = A.iacc(b[fa + E:fa + E + int(0.5 * FS)])
        v['lr'] = float(10 * np.log10(((b[:, 0] ** 2).sum() + 1e-30) / ((b[:, 1] ** 2).sum() + 1e-30)))
    out['bands'] = {str(k): {kk: float(vv) for kk, vv in v.items()} for k, v in tb.items()}
    t, d = A.echo_density(y[:, 0] + y[:, 1], 0.01)
    t0 = fa / FS
    out['ned'] = {str(ms): float(np.interp(t0 + ms / 1000, t, d)) for ms in (2.5, 5, 10, 20, 40, 80, 160)}
    k = np.nonzero(d > 0.9)[0]
    out['mix_ms'] = float((t[k[0]] - t0) * 1000) if len(k) else -1.0
    out['ned_l'] = {str(ms): float(np.interp(t0 + ms / 1000, *A.echo_density(y[:, 0], 0.01))) for ms in (5, 20, 80)}
    return out


def g_rooms(exe, env):
    items, keys = [], []
    for r in ROOMS:
        for d in DECAYS:
            items.append((R.patch(room=r, decay=d), R.impulse(T(d)), T(d), None)); keys.append('%d_%d' % (r, d))
    ys = jobs_run(exe, env, items)
    res = {k: ir_metrics(y) for k, y in zip(keys, ys)}
    items = [(R.patch(room=r, decay=127), R.impulse(16), 16, None) for r in ROOMS]
    for r, y in zip(ROOMS, jobs_run(exe, env, items)):
        y = y[640:]; v = {}
        for fc in A.OCT:
            if fc * 1.41 > FS / 2: continue
            e = (A.bandpass(y, fc) ** 2).sum(1)
            a = 10 * np.log10(e[int(2 * FS):int(3 * FS)].mean() + 1e-30); b = 10 * np.log10(e[int(14 * FS):int(15 * FS)].mean() + 1e-30)
            v[str(fc)] = dict(lev=float(a), dbs=float((b - a) / 12))
        res['%d_127' % r] = v
    return res


def g_colour(exe, env):
    C = (0, 16, 32, 48, 56, 60, 63, 64, 65, 68, 72, 80, 96, 112, 127)
    res = {}
    for r in (3, 0):
        ys = jobs_run(exe, env, [(R.patch(room=r, decay=80, colour=c), R.impulse(3), 3, None) for c in C])
        cs, e64 = R.mlab.band_db(ys[C.index(64)][640:640 + 7000], 2048)
        _, l64 = R.mlab.band_db(ys[C.index(64)][640 + 47000:640 + 94000], 2048)
        for c, y in zip(C, ys):
            _, e = R.mlab.band_db(y[640:640 + 7000], 2048); _, l = R.mlab.band_db(y[640 + 47000:640 + 94000], 2048)
            res['%d_%d' % (r, c)] = dict(early=list(map(float, e - e64)), late=list(map(float, l - l64)),
                                       abs_early=list(map(float, e)))
    res['bands'] = list(map(float, cs))
    return res


def g_pre(exe, env):
    TT = (0, 1, 2, 4, 8, 16, 27, 28, 40, 64, 127)
    res = {}
    for m in (2, 3, 4):
        ys = jobs_run(exe, env, [(R.patch(mode=m, room=r, time=t, decay=32), R.impulse(1.2), 1.2, None) for r in ROOMS for t in TT])
        k = 0
        for r in ROOMS:
            for t in TT:
                res['%d_%d_%d' % (m, r, t)] = A.first_arrival(ys[k][640:], -30); k += 1
    return res


def g_fb(exe, env):
    res = {}
    for m in (3, 4):
        for r in (3, 0):
            FB = (32, 64, 96, 120, 127)
            ys = jobs_run(exe, env, [(R.patch(mode=m, room=r, time=24, fb=f, decay=0), R.impulse(3.2), 3.2, None) for f in FB])
            for f, y in zip(FB, ys):
                y = y[640:]; e = (y ** 2).sum(1); P = 6144
                sl = [10 * np.log10(e[k * P:(k + 1) * P].sum() + 1e-30) for k in range(1, 9)]
                b3 = [10 * np.log10((A.bandpass(y[3 * P:4 * P], fc) ** 2).sum() + 1e-30) for fc in (125, 500, 2000, 8000)]
                b1 = [10 * np.log10((A.bandpass(y[1 * P:2 * P], fc) ** 2).sum() + 1e-30) for fc in (125, 500, 2000, 8000)]
                res['%d_%d_%d' % (m, r, f)] = dict(per=float(np.mean(np.diff(sl[1:]))), first=A.first_arrival(y, -30),
                                                  spec3=list(map(float, np.array(b3) - np.array(b1))))
    return res


def g_clock(exe, env):
    res = {}
    for bpm in (120, 100):
        for r in (0, 3):
            C = (1, 3, 5, 6, 7, 10, 19, 40)
            items = []
            for c in C:
                p = R.patch(room=r, decay=32, clock=c); p[R.mlab.PB + 16] = bpm - 63
                items.append((p, R.impulse(1.5), 1.5, None))
            for c, y in zip(C, jobs_run(exe, env, items)):
                res['%d_%d_%d' % (bpm, r, c)] = A.first_arrival(y[640:], -30)
    return res


def g_send(exe, env):
    S = (0, 16, 32, 48, 64, 80, 96, 112, 127)
    ys = jobs_run(exe, env, [(R.patch(room=3, decay=64, send=s), R.impulse(3), 3, None) for s in S])
    res = {}
    for s, y in zip(S, ys):
        y = y[640:]
        res[str(s)] = dict(dry=float(y[0, 0]), wet=float(10 * np.log10((y[1:] ** 2).sum() + 1e-30)))
    return res


def sine(f, on=1.0, secs=8):
    x = np.zeros((int(secs * FS), 2)); n = int(on * FS); t = np.arange(n) / FS
    x[:n, 0] = x[:n, 1] = 0.25 * np.sin(2 * np.pi * f * t) * np.minimum(1, np.minimum(t, on - t) / 0.005)
    return x


def g_wobble(exe, env):
    res = {}
    for r in (0, 3):
        F = (250.0, 1000.0, 4000.0)
        ys = jobs_run(exe, env, [(R.patch(room=r, decay=112), sine(f), 8, None) for f in F])
        for f, y in zip(F, ys):
            y = y[640:]; v = {}
            for c in (0, 1):
                x = y[int(1.2 * FS):int(7.2 * FS), c]
                X = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
                fr = np.fft.rfftfreq(len(x), 1 / FS)
                m = (fr > f - 60) & (fr < f + 60)
                P = 10 * np.log10(X[m] / X[m].max()); ff = fr[m] - f
                ws = []
                for lv in (-6, -20):
                    i = np.nonzero(P > lv)[0]; ws.append(float(ff[i[-1]] - ff[i[0]]))
                fi, en = A.inst_freq(y[:, c], f, bw=0.05 * f)
                s = slice(int(1.3 * FS), int(5 * FS)); g = fi[s]; w = en[s]; k = w > 0.05 * np.median(w)
                cents = 1200 * np.log2(np.maximum(g[k], 1) / f)
                v['LR'[c]] = dict(w6=ws[0], w20=ws[1], cstd=float(np.std(cents)), c95=float(np.percentile(np.abs(cents), 95)))
            res['%d_%d' % (r, f)] = v
    return res


G = dict(rooms=g_rooms, colour=g_colour, pre=g_pre, fb=g_fb, clock=g_clock, send=g_send, wobble=g_wobble)
if __name__ == '__main__':
    q, out = sys.argv[1], sys.argv[2]
    groups = sys.argv[3].split(',') if len(sys.argv) > 3 else list(G)
    exe, env = R.engines(q)
    try:
        res = json.load(open(out))
    except Exception:
        res = {}
    for g in groups:
        res[g] = G[g](exe, env); print(g, 'done', flush=True)
        json.dump(res, open(out, 'w'), indent=0)
