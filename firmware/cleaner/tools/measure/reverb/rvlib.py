# SPDX-License-Identifier: GPL-3.0-only
"""rvlib.py: the reverb measured as a black box (the engine an executable, the lab's command line, mlab.py): the
audio input straight into the effects, an impulse / noise / sines in, the reverb's wet output out. Engines:
    RV_OLD  a host renderer built from the firmware engine with the development engine's reverb (the black box to match;
            SY_RVQ=1 FULL, 0 HALF, SY_SHIM=... the shimmer), RV_OLD16 the same as two parts (the 16-bit HALF ring)
    RV_NEW  the cleaner's own reverb (cleaner/build/engine, or a firmware-engine renderer)
Nothing of an engine's source is read: only its output."""
import os, sys
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import mlab  # noqa: E402

SCR = os.environ.get('RV_SCR', '/tmp/infekt/rvm')
OLD = os.environ.get('RV_OLD', os.path.join(SCR, 'old_full'))
OLD16 = os.environ.get('RV_OLD16', os.path.join(SCR, 'old_multi'))
NEW = os.environ.get('RV_NEW', os.path.join(SCR, 'new_full'))
NEW16 = os.environ.get('RV_NEW16', os.path.join(SCR, 'new_multi'))
FS = mlab.FS
MODE, SEND, TIME, FEEDBK, DECAY, ROOM, SHAPE, COLOUR = 112, 113, 114, 115, 116, 117, 118, 119
CLOCK = mlab.PB + 20


def patch(mode=2, decay=64, room=3, colour=64, time=0, fb=0, send=127, clock=0, sel=0):
    p = mlab.init_preset()
    p[mlab.INPUT_MODE] = 3; p[mlab.INPUT_SELECT] = sel; p[mlab.CHORUS_MIX] = 0
    p[MODE] = mode; p[SEND] = send; p[TIME] = time; p[FEEDBK] = fb; p[DECAY] = decay; p[ROOM] = room
    p[COLOUR] = colour; p[CLOCK] = clock
    return p


def run(exe, p, inp, secs, env=None, events=()):
    """inp (frames, 2) at the input; the output (frames, 2) for secs seconds"""
    n = mlab.units(secs)
    return mlab.render(exe, p, list(events) + [('run', n)], inp=inp, env=env)


def impulse(secs, ch=(1, 1), at=256, amp=0.5):
    x = np.zeros((int(secs * FS) + 256, 2)); x[at, 0] = amp * ch[0]; x[at, 1] = amp * ch[1]
    return x


def engines(q):
    """(exe, env) of a quality name: full, half, half16 (old) / nfull, nhalf, nhalf16 (new)"""
    return {'full': (OLD, {'SY_RVQ': '1'}), 'half': (OLD, {'SY_RVQ': '0'}), 'half16': (OLD16, {'SY_NOLEND': '1', 'SY_RVQ': '0'}),
            'nfull': (NEW, {'SY_RVQ': '1'}), 'nhalf': (NEW, {'SY_RVQ': '0'}), 'nhalf16': (NEW16, {'SY_NOLEND': '1', 'SY_RVQ': '0'})}[q]
