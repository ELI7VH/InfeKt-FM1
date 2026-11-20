# SPDX-License-Identifier: GPL-3.0-only
"""apply_fit.py: fit_rv.py's and fit_shim.py's constants (their JSON in the scratch folder) into engine/sy_cln_rv.c's
tables (between the FIT BEGIN / FIT END marks), each with its name; the source's other values as they are."""
import os, re, json
import rvlib as R
import fit_rv, fit_shim
SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'engine', 'sy_cln_rv.c')


def cf(v):
    t = '%.7g' % v
    return t + ('' if ('.' in t or 'e' in t) else '.') + 'f'


def block(names, vals, per=4):
    rows = []
    for i in range(0, len(names), per):
        ks = names[i:i + per]
        rows.append('    ' + ' '.join(cf(vals[k]) + ',' for k in ks) + '   /* ' + ' '.join(ks) + ' */')
    return '\n'.join(rows) + '\n'


s = open(SRC).read()
P = json.load(open(fit_rv.ST))
Q = json.load(open(fit_shim.ST)) if os.path.exists(fit_shim.ST) else {}
marks = [m.start() for m in re.finditer(r'/\* FIT BEGIN', s)]
for n, (names, vals) in enumerate(((fit_rv.NAMES, P), (fit_shim.SPN, None))):
    i = s.index('\n', marks[n]) + 1 if n == 0 else s.index('\n', [m.start() for m in re.finditer(r'/\* FIT BEGIN', s)][n]) + 1
    j = s.index('/* FIT END */', i)
    if vals is None:   # (the shimmer's: the fitted ones over the source's defaults)
        cur = [float(x) for x in re.findall(r'(-?[\d.]+(?:e-?\d+)?)f,', s[i:j])]
        vals = dict(zip(names, cur)); vals.update(Q)
    s = s[:i] + block(names, vals) + s[j:]
open(SRC, 'w').write(s)
print('apply_fit: written', SRC)
