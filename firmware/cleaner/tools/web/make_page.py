#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 J0KERPACK
"""The InfeKt-FM1 page with a firmware in it: infekt-fm1.html (this directory) with the firmware package (and
m-bootloader's, optional) and the Editor tab's parameter table (make_params.py, from the firmware's ui.c) embedded,
one file that installs without a server or a network (opened by a double-click). The Guide tab's quickstart (German
and English) is made from the quickstart builder (not part of this release) (fragment.py: the same bodies as its printable PDF, build.py) and
embedded too. (Since 1.18C47w2 the page carries no boot image: the Installation tab's boot image card is gone; the
firmware's own splash is unchanged.)

Two variants from the same template: by default the page as before (no sound in the browser); --synth adds the web
synth: the engine as WebAssembly (wasm/build.sh, -Oz; or --wasm a module built before), gzipped
into #wsdata, and its dock, keyboard and worklet (websynth.inc.html). The template marks what only the synth page has:
<!--WS+--> .. <!--WS.--> and /*WS+*/ .. /*WS.*/ (kept with --synth, else dropped), /*WS+*/ A /*WS-*/ B /*WS.*/ (A with
--synth, else B); without --synth the page is the template's text without the synth's lines.

    make_page.py OUT.html --fwsc FIRMWARE.fwsc [--version 0.96] [--boot m-bootloader-1.2.fwsc --boot-license LICENSE.txt]
    make_page.py OUT.html                  (no firmware: the page asks for a .fwsc file on the Installation tab)
    make_page.py OUT.html ... --synth [--wasm ws_engine.wasm]   (with the sound in the browser)
    make_page.py OUT.html ... --factory A.syx B.syx              (the factory banks: the install's option, banks A, B)
    make_page.py OUT.html ... --waves DIR                        (Sete's wavetables: the install's option)

--factory embeds two bank files as they are (single dumps, each patch's extension frame after its single), offered on
the Installation tab as "Install factory banks" (written to banks A and B after the firmware) and in the patch manager.
Without it the page has no such option and carries no bank data (the template holds none). --waves embeds a folder's
single-cycle WAVs named "Wave NN.wav" (NN from 03; their author counts the sine as 1 and the triangle as 2, the
firmware as waves 0 and 1, so "Wave NN" is the firmware's wave NN - 1, shown "WAVE NN-1": the first user slot, 2, is
"Wave 03"), named from the folder's "Read me.txt" lines "Wave NN - Name" (else "Wave NN"); the sine and triangle files
are left out (waves 0 and 1 are the engine's own, not user slots). Offered as "Install Sete's wavetables" and played
by the web synth.

The package's identity is read from it (one marker byte after each of the first 20 blocks, as the page's productOf)
and must be this firmware's (FM-1_49..., before 1.19.5 FM-1_13...); the version, when not given, comes from the identity
(FM-1_49011905: 1.19.5, FM-1_13009600: 0.96).
m-bootloader is offered on the page only when its package is the one embedded here (its SHA-256 is checked there).
"""
import argparse, base64, gzip, hashlib, json, os, pathlib, re, subprocess, sys, tempfile

HERE = pathlib.Path(__file__).resolve().parent
SLOT = '<script id="fwdata" type="application/json">null</script>'
EDSLOT = '<script id="eddata" type="application/json">null</script>'
FACTSLOT = '<script id="factdata" type="application/json">null</script>'
WAVESLOT = '<script id="wavedata" type="application/json">null</script>'
QUICKSTART = pathlib.Path(os.environ.get('QUICKSTART_DIR') or HERE.parents[2] / 'quickstart')
# (QUICKSTART_DIR: another copy of the quickstart's sources for the Guide tab, e.g. a release's with its version)
GUIDESLOT = '<template id="guide-{}"></template>'
WSINC = '<!--WSINC-->'
WSSLOT = '<script id="wsdata" type="application/octet-stream">null</script>'
WSMARK = re.compile(r'(?:<!--WS\+-->|/\*WS\+\*/)(.*?)(?:(?:<!--WS--->|/\*WS-\*/)(.*?))?(?:<!--WS\.-->|/\*WS\.\*/)', re.S)


def variant(html, synth):
    """the template's synth-only parts kept (synth) or dropped, its alternatives chosen"""
    out = WSMARK.sub(lambda m: m[1] if synth else (m[2] or ''), html)
    if re.search(r'<!--WS[-+.]-->|/\*WS[-+.]\*/', out):
        raise SystemExit('make_page: an unpaired WS marker in the template')
    return out


def wasm_module(path=None):
    """the web synth's module: the given file, or built now (wasm/build.sh, -Oz)"""
    if path:
        return pathlib.Path(path).read_bytes()
    with tempfile.TemporaryDirectory() as d:
        out = pathlib.Path(d) / 'ws_engine.wasm'
        subprocess.run(['sh', str(HERE / 'wasm' / 'build.sh'), str(out), '-Oz'], check=True, capture_output=True)
        return out.read_bytes()


def product_of(raw):
    """the package identity: one marker byte after each of the first 20 blocks (the page's FW.productOf)"""
    if len(raw) < 20 * 0x30:
        raise SystemExit('not an FM-1 package (too short)')
    s = ''
    for i in range(20):
        m = raw[i * 0x30 + 0x2F]
        if m != 0x7D:
            s += chr((m - i - 1) & 0xFF)
    return s


def version_of(product):
    m = re.fullmatch(r'FM-1_(?:49|13)(\d\d)(\d\d)(\d\d)', product)   # (OWN_IDS: FM-1_49, the former FM-1_13)
    if not m:
        return ''
    return f'{int(m[1])}.{m[2]}' + (f'.{int(m[3])}' if m[3] != '00' else '')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out')
    ap.add_argument('--fwsc', help="the firmware package (tools/build.py's release .fwsc)")
    ap.add_argument('--version', help='the version shown (default: from the identity)')
    ap.add_argument('--boot', help="m-bootloader's package (an FM-1_099 package)")
    ap.add_argument('--boot-license', help="m-bootloader's licence text, shown on the page when it is embedded")
    ap.add_argument('--template', default=str(HERE / 'infekt-fm1.html'))
    ap.add_argument('--factory', nargs=2, metavar=('A.syx', 'B.syx'), help='the factory banks for banks A and B')
    ap.add_argument('--waves', metavar='DIR', help="Sete's wavetables: a folder of 'Wave NN.wav' (and 'Read me.txt')")
    ap.add_argument('--synth', action='store_true', help='the sound in the browser (the engine as WebAssembly)')
    ap.add_argument('--wasm', help='--synth: the module (wasm/build.sh); default: built now')
    ap.add_argument('--lang', choices=('de', 'en'), default='de', help='the language the page starts in')
    a = ap.parse_args()
    html = pathlib.Path(a.template).read_text(encoding='utf-8')
    if a.lang == 'en':                   # (the template starts in German; a stored choice still wins)
        for old, new in (('<html lang="de">', '<html lang="en">'),
                         ('let lang = store.get("infekt.lang") === "en" ? "en" : "de";',
                          'let lang = store.get("infekt.lang") === "de" ? "de" : "en";')):
            assert html.count(old) == 1, old
            html = html.replace(old, new)
    wasm = None
    if a.synth:
        if html.count(WSINC) != 1:
            raise SystemExit(f'{a.template}: the web synth\'s place is not there')
        html = html.replace(WSINC, (HERE / 'websynth.inc.html').read_text(encoding='utf-8').rstrip('\n'))
        if html.count(WSSLOT) != 1:
            raise SystemExit('websynth.inc.html: the wsdata slot is not there')
        wasm = wasm_module(a.wasm)
        if wasm[:4] != b'\0asm':
            raise SystemExit('--wasm: not a WebAssembly module')
        html = html.replace(WSSLOT, '<script id="wsdata" type="application/octet-stream">'
                            + base64.b64encode(gzip.compress(wasm, 9, mtime=0)).decode('ascii') + '</script>')
    elif a.wasm:
        raise SystemExit('--wasm needs --synth')
    html = variant(html, a.synth)
    if html.count(SLOT) != 1:
        raise SystemExit(f'{a.template}: the fwdata slot is not there (already filled?)')
    data = None
    if a.fwsc:
        raw = pathlib.Path(a.fwsc).read_bytes()
        product = product_of(raw)
        if not product.startswith(('FM-1_49', 'FM-1_13')):
            raise SystemExit(f'{a.fwsc}: identity {product!r} is not this firmware (FM-1_49..., FM-1_13...)')
        data = {'product': product, 'version': a.version or version_of(product),
                'pkg': base64.b64encode(raw).decode('ascii')}
        if a.boot:
            braw = pathlib.Path(a.boot).read_bytes()
            if product_of(braw) != 'FM-1_099':
                raise SystemExit(f'{a.boot}: not an m-bootloader package (FM-1_099)')
            data['boot'] = {'sha256': hashlib.sha256(braw).hexdigest(), 'pkg': base64.b64encode(braw).decode('ascii')}
            if a.boot_license:
                data['boot']['license'] = pathlib.Path(a.boot_license).read_text(encoding='utf-8')
    elif a.boot:
        raise SystemExit('--boot needs --fwsc')
    js = json.dumps(data, ensure_ascii=True).replace('</', '<\\/')
    html = html.replace(SLOT, f'<script id="fwdata" type="application/json">{js}</script>')
    # the factory banks: the files as they are (checked: single dumps of 267 bytes, 1..128 of them, no stray bytes)
    if html.count(FACTSLOT) != 1:
        raise SystemExit(f'{a.template}: the factdata slot is not there')
    fact = None
    if a.factory:
        fact = {'banks': []}
        for k, f in enumerate(a.factory):
            raw = pathlib.Path(f).read_bytes()
            n, i = 0, 0
            while i < len(raw):
                if raw[i] != 0xF0 or raw.find(b'\xF7', i) < 0:
                    raise SystemExit(f'{f}: not a SysEx file')
                j = raw.index(b'\xF7', i) + 1
                m = raw[i:j]
                n += len(m) == 267 and m[1:4] == b'\x00\x20\x33'
                i = j
            if not 1 <= n <= 128:
                raise SystemExit(f'{f}: {n} single dumps (1..128 expected)')
            fact['banks'].append({'name': 'Factory ' + 'AB'[k], 'file': pathlib.Path(f).name,
                                  'syx': base64.b64encode(raw).decode('ascii')})
    html = html.replace(FACTSLOT, '<script id="factdata" type="application/json">' + json.dumps(fact) + '</script>')
    # the wavetables: each 'Wave NN.wav' as it is, its slot and name
    if html.count(WAVESLOT) != 1:
        raise SystemExit(f'{a.template}: the wavedata slot is not there')
    wv = None
    if a.waves:
        d = pathlib.Path(a.waves)
        names = {}
        rm = d / 'Read me.txt'
        if rm.exists():
            for ln in rm.read_text(encoding='utf-8', errors='replace').splitlines():
                m = re.fullmatch(r'\s*Wave\s+(\d{1,2})\s*-\s*(.+?)\s*', ln)
                if m:
                    names[int(m[1])] = m[2]
        wv = {'waves': []}
        for f in sorted(d.glob('*.wav')):
            m = re.fullmatch(r'Wave\s*(\d{1,2})', f.stem, re.I)
            if not m:
                continue                 # (Sine.wav, Triangle.wav: waves 0 and 1, the engine's own)
            nn = int(m[1])
            if not 3 <= nn <= 64:
                raise SystemExit(f'{f}: wave {nn} is not a user slot (03..64)')
            raw = f.read_bytes()
            if raw[:4] != b'RIFF' or raw[8:12] != b'WAVE':
                raise SystemExit(f'{f}: not a WAV file')
            nm = re.sub(r'[^\x20-\x7E]', '?', names.get(nn, f'Wave {nn:02d}'))[:10]
            wv['waves'].append({'slot': nn - 1, 'name': nm, 'wav': base64.b64encode(raw).decode('ascii')})
        if not wv['waves']:
            raise SystemExit(f'{d}: no "Wave NN.wav" in it')
    html = html.replace(WAVESLOT, '<script id="wavedata" type="application/json">' + json.dumps(wv) + '</script>')
    # the Editor tab's parameters, from the firmware's sources (make_params.py: ui.c, app.c, init_patch.h)
    ed = subprocess.run([sys.executable, str(HERE / 'make_params.py')], check=True, capture_output=True, text=True).stdout.strip()
    if html.count(EDSLOT) != 1:
        raise SystemExit(f'{a.template}: the eddata slot is not there')
    html = html.replace(EDSLOT, '<script id="eddata" type="application/json">' + ed.replace('</', '<\\/') + '</script>')
    # the Guide tab: the quickstart in both languages (../quickstart/fragment.py)
    sys.path.insert(0, str(QUICKSTART))
    import fragment
    for lang in ('de', 'en'):
        slot = GUIDESLOT.format(lang)
        if html.count(slot) != 1:
            raise SystemExit(f'{a.template}: the guide-{lang} slot is not there')
        g = fragment.fragment(lang)
        if '</template' in g or '</script' in g:
            raise SystemExit(f'the {lang} guide would end its template early')
        html = html.replace(slot, f'<template id="guide-{lang}">{g}</template>')
    pathlib.Path(a.out).write_text(html, encoding='utf-8')
    what = f"{data['product']} ({data['version']}){' + m-bootloader' if 'boot' in data else ''}" if data else 'no firmware'
    if fact:
        what += ', factory banks'
    if wv:
        what += ', ' + str(len(wv['waves'])) + ' waves'
    print(f'{a.out}: {len(html.encode()) // 1024} KB, {what}'
          f'{", web synth " + str(len(wasm)) + " B" if wasm else ""}')


if __name__ == '__main__':
    main()
