# SPDX-License-Identifier: GPL-3.0-only
"""cbuild.py: the cleaner's host engine built with extra -D flags into the scratch folder (a measurement build: the
constants a fit is searching for given on the command line). build(name, flags) -> the executable's path. The flags
reach the units that read them (synth.c, sy_tables.c); the other objects are kept between builds."""
import os, subprocess, hashlib
import mlab

SCR = os.environ.get('MLAB_SCRATCH', '/tmp/infekt/cl')
CL = os.path.join(mlab.ROOT, 'cleaner')
SRC = ['host/lab_engine.c'] + ['engine/' + f for f in ('synth.c', 'sy_voice.c', 'sy_tables.c', 'sy_fx.c', 'sy_waves.c',
                                                         'sy_dist_tab.c', 'sy_arp_tab.c', 'sy_hyb_lfow.c')]
FLAGGED = ('engine/synth.c', 'engine/sy_tables.c')


def _cc(args):
    r = subprocess.run(['cc'] + args, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr)


def build(name, flags=(), all_units=False):
    out = os.path.join(SCR, 'cb_' + name)
    od = os.path.join(SCR, 'cb_obj'); os.makedirs(od, exist_ok=True)
    pf, ps = [], [os.path.join(CL, 'engine', 'sy_cln_rv.c')]   # (the own reverb, PROVENANCE.md)
    base = ['-O2', '-std=c99', '-w', '-ffp-contract=off'] + pf + ['-I' + CL + '/engine']
    objs = []
    for f in [os.path.join(CL, s) for s in SRC] + ps:
        fl = list(flags) if (all_units or any(f.endswith(x) for x in FLAGGED)) else []
        hs = [open(f, 'rb').read()] + [open(os.path.join(CL, 'engine', h), 'rb').read()
                                         for h in sorted(os.listdir(os.path.join(CL, 'engine'))) if h.endswith(('.h', '.inc'))]
        key = hashlib.sha1(b''.join(hs) + ' '.join(base + fl).encode()).hexdigest()[:16]
        o = os.path.join(od, os.path.basename(f) + '.' + key + '.o')
        if not os.path.exists(o):
            _cc(base + fl + ['-c', f, '-o', o])
        objs.append(o)
    _cc(['-o', out] + objs + ['-lm'])
    return out
