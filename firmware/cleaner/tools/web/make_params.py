#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
"""The Editor tab's parameter table, from the firmware's own sources (so the page and the panel stay in step):
cleaner/fm1/ui.c (the panel's blocks and EDIT groups: U_ROW, U_GRP, U_MENU; the value lists EN_*; the matrix's short
names MX_SHORT, MX_DUP, which name the destinations 72-127 in the MXDST list as the panel's knob does; the
shimmer's targets SHX_TN), cleaner/fm1/app.c (the shimmer's extension: SHX_DEFS, SHX_RB,
SHX_VD, SHX_NT, SHX_L3OFF; HYB_VERSION from app.h) and cleaner/fm1/init_patch.h (the init patch: each knob's value on
a double-click).

    make_params.py [OUT.json]          (no OUT: the JSON to stdout; make_page.py embeds it into the page)

Rows marked "shim" exist only in the builds with the shimmer (HYB_SHIMMER), "noshim" only without it. Nothing is
copied by hand: a parameter added to ui.c appears on the page at the next make_page.py."""
import json, pathlib, re, sys

FM1 = pathlib.Path(__file__).resolve().parent.parent.parent / "fm1"


def strip_comments(s):
    return re.sub(r"/\*.*?\*/", " ", re.sub(r"//[^\n]*", "", s), flags=re.S)


def block(src, start):
    """the text of the brace block after `start` (its outer braces left out)"""
    i = src.index(start)
    i = src.index("{", i + len(start) - 1)
    d, j = 0, i
    while True:
        c = src[j]
        if c == '"':
            j = src.index('"', j + 1)
        elif c == "{":
            d += 1
        elif c == "}":
            d -= 1
            if not d:
                return src[i + 1:j]
        j += 1


def mark_pp(text):
    """#ifdef HYB_SHIMMER / #else / #endif as tokens @S+ @S- @S0 (other # lines dropped)"""
    out, stack = [], []
    for ln in text.split("\n"):
        t = ln.strip()
        if t.startswith("#"):
            if re.match(r"#\s*if(def)?\s+.*HYB_SHIMMER", t):
                stack.append("shim"); out.append(" @S+ ")
            elif re.match(r"#\s*if", t):
                stack.append("other"); out.append(" @S0 ")
            elif re.match(r"#\s*else", t):
                if stack and stack[-1] == "shim": out.append(" @S- ")
            elif re.match(r"#\s*endif", t):
                k = stack.pop() if stack else None
                if k == "shim": out.append(" @S0 ")
            continue
        out.append(ln)
    return "\n".join(out)


TOK = re.compile(r'\s*(?:(@S[+\-0])|("(?:[^"\\]|\\.)*")|([A-Za-z_]\w*)|(\d+)u?|(.))')


def tokens(s):
    for m in TOK.finditer(s):
        if m.group(1): yield ("pp", m.group(1))
        elif m.group(2): yield ("str", json.loads(m.group(2)))
        elif m.group(3): yield ("id", m.group(3))
        elif m.group(4): yield ("num", int(m.group(4)))
        elif m.group(5) and not m.group(5).isspace(): yield ("p", m.group(5))


def parse_init(toks):
    """a brace initializer list into nested lists; each item [flag, value]; flag from the @S tokens around it"""
    toks = list(toks)
    pos, flag = 0, [None]

    def items():
        nonlocal pos
        out, cur = [], []
        while pos < len(toks):
            kind, v = toks[pos]
            if kind == "pp":
                flag[0] = {"@S+": "shim", "@S-": "noshim", "@S0": None}[v]; pos += 1; continue
            if kind == "p" and v == "{":
                pos += 1
                f = flag[0]
                cur.append(("list", items(), f))
                continue
            if kind == "p" and v == "}":
                pos += 1
                if cur: out.append(cur)
                return out
            if kind == "p" and v == ",":
                if cur: out.append(cur)
                cur = []; pos += 1; continue
            cur.append((kind, v, flag[0])); pos += 1
        if cur: out.append(cur)
        return out
    return items()


def expand_macros(text):
    """ui.c's row macros written out: {byte, kind, en, max, label, name}"""
    text = re.sub(r"PB_\((\d+)\)", lambda m: str(128 + int(m.group(1))), text)
    text = re.sub(r"\bN_\(([^()]*?)\)", lambda m: "{%s}" % _pn(m.group(1), "V_NUM"), text)
    text = re.sub(r"\bBI\(([^()]*?)\)", lambda m: "{%s}" % _pn(m.group(1), "V_BI"), text)
    text = re.sub(r"\bOO\(([^()]*?)\)", lambda m: "{%s}" % _pn(m.group(1), "V_OFFON", mx="1"), text)
    text = re.sub(r"\bEN\(([^()]*?)\)", lambda m: "{%s}" % _en(m.group(1)), text)
    for m in re.finditer(r"#define (\w+) (\{[^\n]*?\})", UIC):   # (the rows' own constants: DRY_P)
        if re.search(r"\b%s\b" % m.group(1), text):
            text = re.sub(r"\b%s\b" % m.group(1), re.sub(r"PB_\((\d+)\)", lambda q: str(128 + int(q.group(1))), m.group(2)), text)
    def mx(m):
        i, s, d, a = [x.strip() for x in m.group(1).split(",")]
        s, d, a = 128 + int(s), 128 + int(d), 128 + int(a)
        return ('{"SLOT %s", {{%d, V_ENUM, E_MXSRC, 0, "SOURCE", "Mx%s Source"}, {%d, V_ENUM, E_MXDST, 0, "DEST", '
                '"Mx%s Dest"}, {%d, V_BI, 0, 0, "AMOUNT", "Mx%s Amount"}}}' % (i, s, i, d, i, a, i))
    return re.sub(r"\bMX\(([^()]*?)\)", mx, text)


def _pn(args, kind, mx="0"):
    b, l, n = [x.strip() for x in _split(args)]
    return "%s, %s, 0, %s, %s, %s" % (b, kind, mx, l, n)


def _en(args):
    b, e, m, l, n = [x.strip() for x in _split(args)]
    return "%s, V_ENUM, %s, %s, %s, %s" % (b, e, m, l, n)


def _split(args):
    out, cur, q = [], "", False
    for c in args:
        if c == '"': q = not q
        if c == "," and not q: out.append(cur); cur = ""
        else: cur += c
    out.append(cur)
    return out


def main():
    global UIC
    ui = UIC = (FM1 / "ui.c").read_text()
    app = (FM1 / "app.c").read_text()
    uic = strip_comments(ui)

    kinds = [k for k in re.findall(r"\b(V_\w+)", block(uic, "enum { V_NUM"))]
    kinds = ["V_NUM"] + [k for k in kinds if k != "V_NUM"]
    enums = {}
    for m in re.finditer(r"static const char \*const (EN_\w+)\[\] = \{(.*?)\};", uic, re.S):
        enums[m.group(1)] = [json.loads(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"', m.group(2))]

    # the rows
    text = expand_macros(mark_pp(block(uic, "static const u_row_t U_ROW[] =")))
    rows = []
    # the blocks: the rows before the line "#define U_NBLK" (the shimmer build's; its SHIMMER block the 25th)
    blk_text = expand_macros(mark_pp(block(uic, "static const u_row_t U_ROW[] =").split("#define U_NBLK")[0]))
    nblk = len([r for r in parse_init(tokens(blk_text)) if r and r[0][0] == "list"])
    for r in parse_init(tokens(text)):
        if not r or r[0][0] != "list":
            continue
        tag_item, params_item = r[0][1][0], r[0][1][1]
        flag = r[0][2]
        tag = tag_item[0][1]
        ps = []
        for p in params_item[0][1]:
            vals = [x[0][1] for x in p[0][1]]
            b, k, e, mx, lab, name = vals
            ps.append({"b": b, "k": k[2:], "e": e[2:] if isinstance(e, str) else None, "m": mx, "l": lab, "n": name})
        rows.append({"tag": tag, "p": ps, "f": flag})
    # which rows are blocks: the ones before "#define U_NBLK" (in the shimmer build, SHIMMER is the 25th)
    for i, r in enumerate(rows):
        r["blk"] = i < nblk
    # the groups and the menus
    grps = []
    for r in parse_init(tokens(mark_pp(block(uic, "static const u_group_t U_GRP[] =")))):
        it = r[0]
        grps.append({"name": it[1][0][0][1], "nr": it[1][1][0][1], "f": it[2]})
    menus = []
    mtext = strip_comments(block(ui, "static const u_menu_t U_MENU[U_NMENU] ="))
    for m in re.finditer(r'\{"(\w+)",\s*\w+,\s*(\d+),\s*([^}]*)\}', mtext):
        g0 = re.match(r"\w+", m.group(3).strip()).group(0)
        if not g0.isdigit(): g0 = re.search(r"#define %s (\d+)u" % g0, ui).group(1)   # (MX_N)
        menus.append({"name": m.group(1), "nb": int(m.group(2)), "ng": int(g0),
                      "ng_shim": "U_NSHG" in m.group(3)})
    # rows to menus: the blocks by the menus' counts (the blocks past them, LEVEL and SHIMMER, are FX's: FX_BLK),
    # the groups' rows by the groups, the groups by the menus' counts
    blocks = [r for r in rows if r["blk"]]
    k = 0
    for mi, m in enumerate(menus):
        for _ in range(m["nb"]):
            blocks[k]["menu"] = m["name"]; k += 1
    for r in blocks[k:]:
        r["menu"] = menus[0]["name"]
    gi, gk = 0, 0
    for mi, m in enumerate(menus):
        n = m["ng"]
        shim_extra = 1 if m["ng_shim"] else 0
        for _ in range(n + shim_extra):
            if gi < len(grps): grps[gi]["menu"] = m["name"]; gi += 1
    edit_rows = [r for r in rows if not r["blk"]]
    j = 0
    for g in grps:
        for _ in range(g["nr"]):
            edit_rows[j]["group"] = g["name"]; edit_rows[j]["menu"] = g["menu"]
            if g["f"] and not edit_rows[j]["f"]: edit_rows[j]["f"] = g["f"]
            j += 1
    assert j == len(edit_rows), (j, len(edit_rows))

    # the matrix's short names, the shimmer's names and its extension
    mxs = re.search(r'static const char MX_SHORT\[\] = (.*?);', ui, re.S).group(1)
    mx_short = "".join(json.loads(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"', mxs.replace("\\0", "\\u0000"))).split("\x00")
    mx_dup = [[int(a), int(b)] for a, b in re.findall(r"\{(\d+),\s*(\d+)\}", re.search(r"MX_DUP\[\]\[2\] = \{(.*?)\};", ui).group(1))]
    # (1.00) the destinations past the panel's long names (ui.c MXD_L .. MXD_N - 1): the knob shows the short name,
    # one that repeats another (MX_DUP) that one's (value_text, mx_short)
    mxd_l, mxd_n = (int(re.search(r"#define %s (\d+)u" % n, ui).group(1)) for n in ("MXD_L", "MXD_N"))
    dup = dict(mx_dup)
    assert len(enums["EN_MXDST"]) == mxd_l and len(mx_short) == mxd_n, (len(enums["EN_MXDST"]), len(mx_short))
    enums["EN_MXDST"] += [mx_short[dup.get(d, d)] for d in range(mxd_l, mxd_n)]
    shx_tn = [json.loads(s) for s in re.findall(r'"[^"]*"', re.search(r"SHX_TN\[SHX_NT\] = \{(.*?)\};", ui).group(1))]
    defs = [int(x) for x in re.findall(r"\d+", re.search(r"#define SHX_DEFS ([^/]*)", app).group(1))]
    defs.append(int(re.search(r"#define SHX_GDEF (\d+)u", app).group(1)))   # (1.18C47: W GLIDE, the panel's 256 + SHX_GL)
    rb_txt = re.search(r"SHX_RB\[SHX_NR\] = \{(.*?)\};", app, re.S).group(1)
    rb = [sum(int(n) for n in re.findall(r"\d+", x)) for x in rb_txt.split(",")]   # ("128u + 65u")
    num = lambda n: int(re.search(r"#define %s (\d+)u" % n, app).group(1))
    init_txt = (FM1 / "init_patch.h").read_text()
    init = [int(x) for x in re.findall(r"\d+", block(init_txt, "static const uint8_t INIT_PATCH[256] ="))]
    assert len(init) == 256, len(init)
    ver = re.search(r'#define HYB_VERSION "([^"]+)"', (FM1 / "app.h").read_text())
    out = {"src": "cleaner/fm1/ui.c, app.c, init_patch.h" + (" " + ver.group(1) if ver else ""),
           "enums": {k[3:]: v for k, v in enums.items()}, "rows": rows, "groups": grps,
           "menus": [m["name"] for m in menus], "mxShort": mx_short, "mxDup": mx_dup,
           "shx": {"tn": shx_tn, "defs": defs, "rb": rb, "vd": num("SHX_VD"), "nt": num("SHX_NT"), "l3off": num("SHX_L3OFF"),
                   "tail": num("SHX_TAIL"), "swell": num("SHX_SWELL")},
           "init": init}
    js = json.dumps(out, separators=(",", ":"), ensure_ascii=True)
    if len(sys.argv) > 1:
        pathlib.Path(sys.argv[1]).write_text(js)
        n = sum(len(r["p"]) for r in rows)
        print(f"{sys.argv[1]}: {len(rows)} rows, {n} knobs, {len(enums)} lists, {len(js)} B")
    else:
        print(js)


if __name__ == "__main__":
    main()
