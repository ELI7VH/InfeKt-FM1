#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""hostprof.py: the host instruction profiler's engine (driven by hostprof.sh).

  hostprof.py run    --engine BIN --out DIR [--cases a,b|all|bank] [--n1 20 --n2 60] [--jobs 4] [--src ROOT]
  hostprof.py report DIR [--top 25] [--md] [--cases a,b] [--lines FUNC[,FUNC] --nlines 40 --case NAME]

Each case is rendered twice under `valgrind --tool=callgrind` (n1 and n2 blocks of 128 frames, collecting only inside
sy_render and below), and the difference of the two profiles divided by n2 - n1 is the steady cost of one block:
the start-up, the table set-up and the WAV output cancel, so do the attack's first blocks. Counts are x86-64
instructions of the host build, per function (clone suffixes .constprop / .isra / .part merged) and per source line
(callgrind -g line info; lines of inlined code are charged to the function that inlined them, under the inlined
code's own file:line). Results: DIR/NAME.json."""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hostcases  # noqa: E402

SUFFIX = re.compile(r"(\.(constprop|isra|part|cold|lto_priv|localalias)(\.\d+)?)+$")

# function groups (the CPU plan's section 1); first match wins
GROUPS = [
    ("OSC", r"^(cln_osc|co_\w+|vr_osc|osc_run|blep\w*|sub_ring|osc_setup|osc_\w+|ph_\w+|cln_o\w+)$"),
    ("FILT", r"^(vr_filt\w*|filt_\w+|lad\w*|cln_filt\w*|sat\w*)$"),
    ("AMP", r"^(vr_amp\w*|vr_pan\w*|qroot)$"),
    ("CTRL", r"^(voice_block|matrix|env_step|lfo_\w+|ctl_\w+|mtx\w*|voice_\w+|vc_\w+|vr_\w+|tab_\w*|sy_tab|sy_exp2|sy_log2|sy_voice_render\w*|interp|ji_semi|uni_x|hi_gain|sy_stretch_cents|send_law|arp_\w+|dw_tick|dl_\w+)$"),
    ("REV", r"^(rv_\w+|rvlf_\w+|rvs_\w+|cln_rv\w*|hyb_rv\w*|rvc_\w+|RV_\w+)$"),
    ("FX", r"^(sy_fx_render|fx_\w+|chorus\w*|delay\w*|phaser\w*|eq\w*|dist\w*|boost\w*|shim\w*|gain_ramp|delay_rmp|onepole|send_\w+|src_value|colour_coef|tpt_g)$"),
    ("FRAME", r"^(sy_render|sy_multi_render|sy_render_i24|i24_out)$"),
]
GROUP_NAMES = [g for g, _ in GROUPS] + ["OTHER"]


def group_of(fn):
    for g, rx in GROUPS:
        if re.match(rx, fn):
            return g
    return "OTHER"


def norm(fn):
    return SUFFIX.sub("", fn)


def parse(path):
    """callgrind.out (written with --compress-strings=no --compress-pos=no) -> ({fn: self}, {(fn, file, line): self})"""
    fns, lines = {}, {}
    fn = fl = None
    after_calls = False
    for raw in open(path, errors="replace"):
        if not raw or raw[0] == "#":
            continue
        c = raw[0]
        if c.isdigit():
            if after_calls:             # the inclusive cost of the call just made: not self cost
                after_calls = False
                continue
            p = raw.split()
            ln, cost = int(p[0]), int(p[1])
            fns[fn] = fns.get(fn, 0) + cost
            k = (fn, fl, ln)
            lines[k] = lines.get(k, 0) + cost
        elif raw.startswith("fn="):
            fn = norm(raw[3:].strip())
        elif raw.startswith("fl=") or raw.startswith("fi=") or raw.startswith("fe="):
            fl = raw[3:].strip()
        elif raw.startswith("calls="):
            after_calls = True
        # (cfn=, cfi=, cfl=, ob=, cob= and the header lines carry nothing we need)
    return fns, lines


def valgrind(engine, case, blocks, tmp, tag):
    pf = os.path.join(tmp, "%s.raw" % tag)
    open(pf, "wb").write(case.patch())
    cg = os.path.join(tmp, "%s.cg" % tag)
    cmd = ["valgrind", "--tool=callgrind", "--callgrind-out-file=" + cg, "--compress-strings=no", "--compress-pos=no",
           "--toggle-collect=sy_render", "--quiet", engine, os.path.join(tmp, tag + ".wav"), "preset", pf] \
        + hostcases.events(case, blocks)
    env = dict(os.environ, **case.env)
    env.pop("INPUT", None)
    r = subprocess.run(cmd, env=env, capture_output=True)
    if r.returncode:
        raise RuntimeError("%s: rc %d %s" % (case.name, r.returncode, r.stderr.decode()[-400:]))
    res = parse(cg)
    os.unlink(cg)
    os.unlink(os.path.join(tmp, tag + ".wav"))
    return res


def run_case(engine, case, n1, n2, outdir, tmp, srcroot):
    a_f, a_l = valgrind(engine, case, n1, tmp, case.name + "_a")
    b_f, b_l = valgrind(engine, case, n2, tmp, case.name + "_b")
    d = float(n2 - n1)
    fns = {k: (b_f.get(k, 0) - a_f.get(k, 0)) / d for k in b_f}
    fns = {k: v for k, v in fns.items() if abs(v) >= 0.5}
    lines = []
    for k, v in b_l.items():
        x = (v - a_l.get(k, 0)) / d
        if x >= 0.5:
            lines.append((k[0], os.path.relpath(k[1], srcroot) if k[1].startswith(srcroot) else os.path.basename(k[1]), k[2], x))
    lines.sort(key=lambda t: -t[3])
    res = {"case": case.name, "doc": case.doc, "n1": n1, "n2": n2, "total": sum(fns.values()), "fns": fns,
           "lines": lines[:6000]}
    json.dump(res, open(os.path.join(outdir, case.name + ".json"), "w"))
    return case.name, res["total"]


def cmd_run(a):
    out = Path(a.out); out.mkdir(parents=True, exist_ok=True)
    srcroot = str(Path(a.src).resolve())
    sel = a.cases
    allc = hostcases.all_cases(bank=True)
    if sel == "all":
        cs = [c for c in allc if not c.name.startswith("bank_")] + [c for c in allc if c.name.startswith("bank_")]
    elif sel == "main":
        cs = [c for c in allc if not c.name.startswith("bank_")]
    elif sel == "bank":
        cs = [c for c in allc if c.name.startswith("bank_")]
    else:
        names = sel.split(",")
        cs = [c for c in allc if c.name in names]
        if len(cs) != len(names):
            sys.exit("unknown case in " + sel)
    tmp = tempfile.mkdtemp(prefix="hostprof", dir="/dev/shm" if os.path.isdir("/dev/shm") else None)
    with ThreadPoolExecutor(a.jobs) as ex:
        futs = [ex.submit(run_case, a.engine, c, a.n1 if not c.name.startswith("bank_") else a.n1b,
                          a.n2 if not c.name.startswith("bank_") else a.n2b, out, tmp, srcroot) for c in cs]
        for f in futs:
            n, t = f.result()
            print("%-22s %9.1f k/block" % (n, t / 1000.0), flush=True)
    subprocess.run(["rm", "-rf", tmp])


def load(d, names=None):
    res = {}
    order = list(hostcases.CASES)
    fs = sorted(Path(d).glob("*.json"), key=lambda f: (order.index(f.stem) if f.stem in order else len(order), f.stem))
    for f in fs:
        j = json.load(open(f))
        if names is None or j["case"] in names:
            res[j["case"]] = j
    return res


def aggregate_bank(res):
    """the bank's cases -> one pseudo case 'bank32' (the mean over the patches) and 'bank_max' (the heaviest patch's total)"""
    b = [v for k, v in res.items() if k.startswith("bank_")]
    if not b:
        return res
    fns = {}
    for v in b:
        for k, x in v["fns"].items():
            fns[k] = fns.get(k, 0) + x / len(b)
    lines = {}
    for v in b:
        for fn, fl, ln, x in v["lines"]:
            lines[(fn, fl, ln)] = lines.get((fn, fl, ln), 0) + x / len(b)
    agg = {"case": "bank32", "doc": "mean of the %d test-bank patches (4-note chord)" % len(b), "total": sum(fns.values()),
           "fns": fns, "lines": sorted([(k[0], k[1], k[2], x) for k, x in lines.items()], key=lambda t: -t[3])}
    r = {k: v for k, v in res.items() if not k.startswith("bank_")}
    r["bank32"] = agg
    r["bank_max"] = max(b, key=lambda v: v["total"])
    r["bank_max"] = dict(r["bank_max"], case="bank_max", doc="heaviest bank patch: " + r["bank_max"]["case"])
    return r


def groups_of(v):
    g = dict.fromkeys(GROUP_NAMES, 0.0)
    for fn, x in v["fns"].items():
        g[group_of(fn)] += x
    return g


def table(rows, header, md):
    w = [max(len(str(r[i])) for r in [header] + rows) for i in range(len(header))]
    out = []
    fmt = lambda r: ("| " + " | ".join(str(c).rjust(w[i]) if i else str(c).ljust(w[i]) for i, c in enumerate(r)) + " |") if md \
        else "  ".join(str(c).rjust(w[i]) if i else str(c).ljust(w[i]) for i, c in enumerate(r))
    out.append(fmt(header))
    if md:
        out.append("|" + "|".join(("-" * (x + 1) + ":") if i else ("-" * (x + 2)) for i, x in enumerate(w)) + "|")
    out += [fmt(r) for r in rows]
    return "\n".join(out)


def cmd_report(a):
    res = aggregate_bank(load(a.dir))
    if a.cases:
        res = {k: v for k, v in res.items() if k in a.cases.split(",")}
    names = [k for k in res if not k.startswith("bank_") or k == "bank_max"]
    if a.lines:
        v = res[a.case]
        for fn in a.lines.split(","):
            ls = [t for t in v["lines"] if t[0] == fn][:a.nlines]
            tot = v["fns"].get(fn, 0)
            print("\n%s in %s: %.1f k/block self, top %d lines" % (fn, a.case, tot / 1000, len(ls)))
            for _, fl, ln, x in ls:
                print("  %9.1f  %5.1f%%  %s:%d  %s" % (x, 100 * x / tot if tot else 0, fl, ln, source(a.src, fl, ln)))
        return
    chunk = a.cols
    for s in range(0, len(names), chunk):
        cn = names[s:s + chunk]
        tot = {}
        for k in cn:
            for fn, x in res[k]["fns"].items():
                tot[fn] = max(tot.get(fn, 0), x)
        top = sorted(tot, key=lambda f: -tot[f])[:a.top]
        rows = [[fn] + ["%.1f" % (res[k]["fns"].get(fn, 0) / 1000) for k in cn] for fn in top]
        rows.append(["(other)"] + ["%.1f" % ((res[k]["total"] - sum(res[k]["fns"].get(f, 0) for f in top)) / 1000) for k in cn])
        rows.append(["TOTAL"] + ["%.1f" % (res[k]["total"] / 1000) for k in cn])
        print("\nInstructions per 128-frame block (thousands), self cost per function\n")
        print(table(rows, ["function"] + cn, a.md))
    print("\nGroup sums (thousands per block)\n")
    rows = []
    for k in names:
        g = groups_of(res[k])
        rows.append([k] + ["%.1f" % (g[n] / 1000) for n in GROUP_NAMES] + ["%.1f" % (res[k]["total"] / 1000)])
    print(table(rows, ["case"] + GROUP_NAMES + ["TOTAL"], a.md))


def source(root, fl, ln):
    try:
        for cand in (Path(root) / fl, Path(root) / "engine" / fl):
            if cand.is_file():
                return cand.read_text(errors="replace").splitlines()[ln - 1].strip()[:110]
    except (IndexError, OSError):
        pass
    return ""


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest="cmd", required=True)
    r = sp.add_parser("run")
    r.add_argument("--engine", required=True); r.add_argument("--out", required=True)
    r.add_argument("--cases", default="main"); r.add_argument("--jobs", type=int, default=4)
    r.add_argument("--n1", type=int, default=20); r.add_argument("--n2", type=int, default=60)
    r.add_argument("--n1b", type=int, default=10); r.add_argument("--n2b", type=int, default=40)
    r.add_argument("--src", default=str(Path(__file__).resolve().parents[2]))
    p = sp.add_parser("report")
    p.add_argument("dir"); p.add_argument("--top", type=int, default=25); p.add_argument("--md", action="store_true")
    p.add_argument("--cases", default=""); p.add_argument("--cols", type=int, default=8)
    p.add_argument("--lines", default=""); p.add_argument("--nlines", type=int, default=40); p.add_argument("--case", default="euro")
    p.add_argument("--src", default=str(Path(__file__).resolve().parents[2]))
    a = ap.parse_args()
    {"run": cmd_run, "report": cmd_report}[a.cmd](a)
