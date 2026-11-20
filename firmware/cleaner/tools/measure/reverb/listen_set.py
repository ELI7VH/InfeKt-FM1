# SPDX-License-Identifier: GPL-3.0-only
"""listen_set.py OUTDIR: a small listening set, the development engine's reverb (rvlib.py OLD) against the own one (NEW): per case
a pair OUTDIR/pairs/NN_name_target.wav / _own.wav and a blind ABX set (OUTDIR/abx/NN_A.wav, _B.wav: the two in a
random order, _X.wav one of them; OUTDIR/abx_key.txt), zipped as OUTDIR.zip. The patches are the test bank's
(MLAB_PATDIR: a folder of patch files, not part of this release); the WAVs are never committed."""
import os, sys, random, shutil, wave
import numpy as np
import rvlib as R

PAT = os.environ.get('MLAB_PATDIR', 'pat')
FS = int(R.FS)


def wav(path, y):
    q = np.clip(np.round(y * 8388607), -8388608, 8388607).astype(np.int32)
    b = np.zeros((len(q), 2, 3), np.uint8)
    for k in range(3): b[:, :, k] = (q >> (8 * k)) & 255
    w = wave.open(path, 'wb'); w.setnchannels(2); w.setsampwidth(3); w.setframerate(FS); w.writeframes(b.tobytes()); w.close()


def chord(keys, hold, tail, vel=100):
    ev = [('on', k, vel) for k in keys] + [('run', R.mlab.units(hold))] + [('off', k) for k in keys] + [('run', R.mlab.units(tail))]
    return ev


def seq(notes, step, tail):
    ev = []
    for k in notes:
        ev += [('on', k, 100), ('run', R.mlab.units(step * 0.5)), ('off', k), ('run', R.mlab.units(step * 0.5))]
    return ev + [('run', R.mlab.units(tail))]


def pat(n):
    return bytearray(open(os.path.join(PAT, '%03d.raw' % n), 'rb').read())


def drum():   # (a noise burst with a fast decay at the input: a snare-like hit, the reverb alone)
    rng = np.random.default_rng(3)
    x = np.zeros((int(6 * R.FS), 2))
    for t in (0.0, 0.75, 1.5, 2.25):
        n = int(0.12 * R.FS); e = np.exp(-np.arange(n) / (0.02 * R.FS))
        s = int(t * R.FS); x[s:s + n, 0] = x[s:s + n, 1] = 0.5 * rng.standard_normal(n) * e
    return x


CASES = [  # name, quality, patch, events, input, shimmer
    ('ambrstng_room3_full', 'full', lambda: pat(30), chord((48, 55, 60, 64), 2.5, 7), None, None),
    ('sawpad_room3_full', 'full', lambda: pat(35), chord((45, 52, 57, 60), 2.0, 7), None, None),
    ('labrats_room0_full', 'full', lambda: pat(69), seq((48, 55, 60, 63, 67, 72, 67, 63), 0.25, 4), None, None),
    ('aftrglow_decay111_full', 'full', lambda: pat(14), chord((50, 57, 62), 1.5, 8), None, None),
    ('rainsouls_room0_half', 'half', lambda: pat(62), chord((48, 60, 67), 1.5, 5), None, None),
    ('pluck_seq_half16', 'half16', lambda: pat(33), seq((60, 64, 67, 72, 76, 72, 67, 64), 0.3, 4), None, None),
    ('sawpad_shimmer_half', 'half', lambda: pat(35), chord((45, 52, 57), 2.0, 8), None, '100,1,100,64,0,0,0,57'),
    ('hits_room1_rev_fb', 'full', lambda: R.patch(mode=3, room=1, decay=104, time=24, fb=64, colour=60), [('run', R.mlab.units(6))], drum, None),
]

if __name__ == '__main__':
    out = sys.argv[1]
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(os.path.join(out, 'pairs')); os.makedirs(os.path.join(out, 'abx'))
    rnd = random.Random(20261108)
    key = []
    for i, (nm, q, pf, ev, inp, sh) in enumerate(CASES):
        ys = {}
        for who, qq in (('target', q), ('own', 'n' + q)):
            exe, env = R.engines(qq)
            env = dict(env)
            if sh: env['SY_SHIM'] = sh
            y = R.mlab.render(exe, pf(), ev, inp=inp() if inp else None, env=env)
            ys[who] = y
        g = 0.9 / max(np.abs(ys['target']).max(), np.abs(ys['own']).max(), 1e-6)   # (the same gain for both)
        for who, y in ys.items():
            wav(os.path.join(out, 'pairs', '%02d_%s_%s.wav' % (i + 1, nm, who)), y * g)
        a, b = rnd.sample(['target', 'own'], 2); x = rnd.choice(['target', 'own'])
        for lab, who in (('A', a), ('B', b), ('X', x)):
            wav(os.path.join(out, 'abx', '%02d_%s.wav' % (i + 1, lab)), ys[who] * g)
        key.append('%02d %-26s A=%s B=%s X=%s' % (i + 1, nm, a, b, x))
        print(key[-1], flush=True)
    open(os.path.join(out, 'abx_key.txt'), 'w').write('\n'.join(key) + '\n')
    open(os.path.join(out, 'README.txt'), 'w').write(
        'pairs/: each case rendered with the target reverb and the own reverb (the same gain).\n'
        'abx/: NN_A, NN_B are the two in a random order, NN_X is one of them. Listen, guess, then read abx_key.txt.\n')
    shutil.make_archive(out, 'zip', out)
    print('listen_set:', out + '.zip')
