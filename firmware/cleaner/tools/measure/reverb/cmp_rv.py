# SPDX-License-Identifier: GPL-3.0-only
"""cmp_rv.py OLD.json NEW.json [--md]: meas_rv.py's metrics of two reverbs side by side: per metric the largest and
the median difference (T30 / EDT in %, levels in dB, arrivals in frames), the worst cases named; --md: as Markdown
table rows (PROVENANCE.md)."""
import sys, json
import numpy as np

a, b = json.load(open(sys.argv[1])), json.load(open(sys.argv[2]))
MD = '--md' in sys.argv
rows = []


def row(name, d, unit, keys=None, fmt='%.2f'):
    d = np.asarray(d, float); ok = np.isfinite(d)
    if not ok.any():
        return
    i = int(np.nanargmax(np.abs(np.where(ok, d, 0))))
    worst = keys[i] if keys else ''
    rows.append((name, fmt % np.nanmax(np.abs(d[ok])), fmt % np.nanmedian(np.abs(d[ok])), unit, worst))


if 'rooms' in a and 'rooms' in b:
    R0, R1 = a['rooms'], b['rooms']
    for band_set, nm in (((125, 250, 500, 1000, 2000, 4000), '125 Hz-4 kHz'), ((63, 8000, 16000), '63 Hz, 8, 16 kHz')):
        for met, unit in (('t30', '%'), ('edt', '%')):
            d, k = [], []
            for key in R0:
                if key.endswith('_127') or key not in R1: continue
                for f in band_set:
                    x, y = R0[key]['bands'].get(str(f), {}).get(met), R1[key]['bands'].get(str(f), {}).get(met)
                    if x and y and np.isfinite(x) and np.isfinite(y):
                        d.append(100 * (y / x - 1)); k.append('room/decay %s, %d Hz' % (key.replace('_', '/'), f))
            row('%s %s' % (met.upper(), nm), d, unit, k, '%.1f')
        for met, unit, nm2 in (('lev', 'dB', 'level'), ('early', 'dB', 'early share (80 ms)'), ('iacc_e', '', 'IACC 0-80 ms'),
                               ('iacc_l', '', 'IACC 80-580 ms'), ('lr', 'dB', 'left / right')):
            d, k = [], []
            for key in R0:
                if key.endswith('_127') or key not in R1: continue
                for f in band_set:
                    x, y = R0[key]['bands'].get(str(f), {}).get(met), R1[key]['bands'].get(str(f), {}).get(met)
                    if x is not None and y is not None:
                        d.append(y - x); k.append('%s, %d Hz' % (key, f))
            row('%s %s' % (nm2, nm), d, unit, k)
    d, k = [], []
    for key in R0:
        if key.endswith('_127'): continue
        d.append(R1[key]['first_r'] - R0[key]['first_r']); k.append(key + ' R')
        d.append(R1[key]['first_l'] - R0[key]['first_l']); k.append(key + ' L')
    row('first arrival', d, 'frames', k, '%.0f')
    for ms in ('2.5', '5', '10', '20', '40', '80'):
        d = [R1[key]['ned'][ms] - R0[key]['ned'][ms] for key in R0 if not key.endswith('_127')]
        row('echo density at %s ms' % ms, d, '', [key for key in R0 if not key.endswith('_127')])
    d = [R1[key]['mix_ms'] - R0[key]['mix_ms'] for key in R0 if not key.endswith('_127')]
    row('mixing time (density 0.9)', d, 'ms', [key for key in R0 if not key.endswith('_127')], '%.1f')
    d, k = [], []
    for key in R0:
        if not key.endswith('_127'): continue
        for f, v in R0[key].items():
            d.append(R1[key][f]['dbs'] - v['dbs']); k.append('%s %s Hz' % (key, f))
    row('decay 127: dB/s', d, 'dB/s', k)
if 'colour' in a and 'colour' in b:
    for w in ('early', 'late'):
        d, k = [], []
        for key, v in a['colour'].items():
            if key == 'bands': continue
            for i, (x, y) in enumerate(zip(v[w], b['colour'][key][w])):
                fc = a['colour']['bands'][i]
                if 40 < fc < 17000 and x > -40 and np.isfinite(x) and np.isfinite(y):
                    d.append(y - x); k.append('%s %d Hz' % (key, fc))
        row('colour (third octaves, %s)' % w, d, 'dB', k)
if 'pre' in a and 'pre' in b:
    row('pre-delay: first arrival', [b['pre'][k] - a['pre'][k] for k in a['pre']], 'frames', list(a['pre']), '%.0f')
if 'fb' in a and 'fb' in b:
    row('feedback: dB a repeat', [b['fb'][k]['per'] - a['fb'][k]['per'] for k in a['fb']], 'dB', list(a['fb']))
    row('feedback: first arrival', [b['fb'][k]['first'] - a['fb'][k]['first'] for k in a['fb']], 'frames', list(a['fb']), '%.0f')
    row('feedback: 3rd repeat spectrum', [x - y for k in a['fb'] for x, y in zip(b['fb'][k]['spec3'], a['fb'][k]['spec3'])], 'dB')
if 'clock' in a and 'clock' in b:
    row('delay clock: first arrival', [b['clock'][k] - a['clock'][k] for k in a['clock']], 'frames', list(a['clock']), '%.0f')
if 'send' in a and 'send' in b:
    row('send: wet level', [b['send'][k]['wet'] - a['send'][k]['wet'] for k in a['send'] if a['send'][k]['wet'] > -200], 'dB', list(a['send']))
    row('send: dry', [b['send'][k]['dry'] - a['send'][k]['dry'] for k in a['send']], '', list(a['send']), '%.4f')
if 'wobble' in a and 'wobble' in b:
    for met, unit in (('w6', 'Hz'), ('w20', 'Hz'), ('cstd', 'cent'), ('c95', 'cent')):
        d, k, x0 = [], [], []
        for key in a['wobble']:
            for c in 'LR':
                d.append(b['wobble'][key][c][met] - a['wobble'][key][c][met]); k.append(key + c); x0.append(a['wobble'][key][c][met])
        row('tail wobble %s (development engine median %.1f)' % (met, np.median(x0)), d, unit, k, '%.1f')
if MD:
    print('| metric | max diff | median diff | unit | worst case |')
    print('|---|---|---|---|---|')
    for r in rows:
        print('| %s | %s | %s | %s | %s |' % r)
else:
    for r in rows:
        print('%-46s max %8s  median %8s %-6s %s' % r)
