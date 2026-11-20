// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 J0KERPACK
// The Installation tab in Chromium (since 1.18C47w2): the "?" tooltips (a mouse over one shows its text and leaving
// hides it; a click keeps it open, a second click, Esc or a click outside closes it; Enter on the focused button; a tap
// on a phone-wide window), nothing on the page moves when one opens, the text stays inside the window; the layout: on a
// wide window the install card in a row with "Back to the stock firmware" (top and bottom flush), the status card
// (progress, the status line, the log) beside "Report a problem" and the Ko-fi card (its top flush with the first, its
// bottom with the second); 390 px wide: one column, no sideways scrolling; the footer's one line opens the licences.
// Playwright from the working directory's node_modules or the global ones:
//   node test_install_view.mjs PAGE.html
import { createRequire } from "node:module";
import { resolve } from "node:path";
const { chromium } = createRequire(process.cwd() + "/")("playwright");

const [html] = process.argv.slice(2);
let bad = 0;
const check = (what, ok) => { console.log((ok ? "ok   " : "FAIL ") + what); if (!ok) bad++; };
const url = "file://" + resolve(html);
const browser = await chromium.launch();

async function open(width, lang, touch = false) {
  const ctx = await browser.newContext({ viewport: { width, height: 900 }, hasTouch: touch, isMobile: touch });
  await ctx.addInitScript((l) => { try { localStorage.setItem("infekt.lang", l); localStorage.removeItem("infekt.tab"); } catch (_) { /* */ } }, lang);
  const p = await ctx.newPage();
  const errs = [];
  p.on("pageerror", (e) => errs.push(e.message));
  await p.goto(url);
  await p.waitForFunction(() => !document.getElementById("i-mbootopt").hidden || !JSON.parse(document.getElementById("fwdata").textContent)?.boot, null, { timeout: 5000 });
  await p.evaluate(() => document.fonts.ready);
  await p.waitForTimeout(400);                 // (the header's picture decoded: the layout settled)
  return { ctx, p, errs };
}
const rects = (p) => p.evaluate(() => Object.fromEntries([".ic-inst", ".ic-stock", ".ic-stat", ".ic-rep", ".ic-kofi"].map((s) => {
  const r = document.querySelector(s).getBoundingClientRect();
  const y = window.scrollY;   // (page coordinates: a hover or a tap may scroll the window)
  return [s.slice(4), { top: Math.round(r.top + y), bottom: Math.round(r.bottom + y), left: Math.round(r.left), right: Math.round(r.right) }];
})));
const tipState = (p) => p.evaluate(() => [...document.querySelectorAll("#panel-install .tip")].map((w) => {
  const t = w.querySelector(".tipt"), r = t.getBoundingClientRect();
  return { open: !t.hidden, exp: w.querySelector(".tipb").getAttribute("aria-expanded"), l: r.left, r: r.right, t: r.top, b: r.bottom };
}));
const openIdx = (s) => s.map((x, i) => (x.open ? i : -1)).filter((i) => i >= 0);

for (const lang of ["de", "en"]) {
  const { ctx, p, errs } = await open(1280, lang);
  const R = await rects(p);
  // (the page pass after 1.18C48: the install card beside "stock" and "report", the status card beside Ko-fi)
  check(`${lang} 1280: install card beside "stock" and "report": top flush with the one, bottom with the other (${JSON.stringify([R.inst, R.stock, R.rep])})`,
        R.inst.top === R.stock.top && R.inst.bottom === R.rep.bottom && R.stock.bottom < R.rep.top && R.inst.right < R.stock.left);
  check(`${lang} 1280: the status card beside Ko-fi, flush, under the install card`,
        R.stat.top === R.kofi.top && R.stat.bottom === R.kofi.bottom && R.stat.top > R.inst.bottom && R.stat.right < R.kofi.left);
  const tips = p.locator("#panel-install .tip .tipb");
  const n = await p.evaluate(() => [...document.querySelectorAll("#panel-install .tip")].filter((t) => !t.closest("#i-factopt, #i-wavesopt")).length);
  check(`${lang}: ${n} "?" on the tab (intro, the hardware versions (1.18C48), m-bootloader, port list, stock, report; the build options' own besides)`, n === 6);
  // a mouse over the m-bootloader "?": shown, nothing moves; away: hidden
  const mb = p.locator("#i-mbootopt .tipb");
  await mb.hover();
  await p.waitForTimeout(100);
  let s = await tipState(p);
  const R2 = await rects(p);
  const k = openIdx(s);
  check(`${lang}: hover shows the m-bootloader text, aria-expanded, inside the window, nothing moved`, k.length === 1 && s[k[0]].exp === "true" &&
        s[k[0]].l >= 0 && s[k[0]].r <= 1280 && JSON.stringify(R2) === JSON.stringify(R));
  await p.mouse.move(5, 5);
  await p.waitForTimeout(400);
  s = await tipState(p);
  check(`${lang}: the mouse away: hidden again`, !openIdx(s).length);
  // a click keeps it; the mouse away: still open; a click outside: closed
  await mb.click();
  await p.mouse.move(5, 5);
  await p.waitForTimeout(400);
  s = await tipState(p);
  check(`${lang}: a click keeps it open after the mouse leaves`, openIdx(s).length === 1);
  await p.mouse.click(640, 20);
  await p.waitForTimeout(100);
  s = await tipState(p);
  check(`${lang}: a click outside closes it`, !openIdx(s).length);
  // a link inside it can be reached: the mouse from the "?" to the link (through the gap) keeps it open
  await mb.hover();
  await p.waitForTimeout(100);
  const link = p.locator("#i-mbootopt .tipt a");
  await link.hover();
  await p.waitForTimeout(300);
  s = await tipState(p);
  check(`${lang}: the mouse into the text (to its link): still open`, openIdx(s).length === 1 && await link.isVisible());
  await p.mouse.move(5, 5);
  await p.waitForTimeout(400);
  // the keyboard: focus, Enter opens, Esc closes and the focus is back on the "?"
  await p.locator("#panel-install .ic-stock .tipb").focus();
  await p.keyboard.press("Enter");
  await p.waitForTimeout(100);
  s = await tipState(p);
  const stockTip = await p.evaluate(() => document.querySelector(".ic-stock .tipt").textContent);
  check(`${lang}: Enter on the focused "?" opens it (the stock firmware's details, the maker's link)`, openIdx(s).length === 1 &&
        /Safe Mode|safe mode/.test(stockTip) && await p.locator('.ic-stock .tipt a[href="https://www.m-vave.com/download"]').isVisible());
  await p.keyboard.press("Escape");
  await p.waitForTimeout(100);
  s = await tipState(p);
  check(`${lang}: Esc closes it, the focus back on its "?"`, !openIdx(s).length &&
        await p.evaluate(() => document.activeElement && document.activeElement.matches(".ic-stock .tipb")));
  // a second click on the same "?" closes it; another "?" replaces it
  const rp = p.locator(".ic-rep .tipb");
  await rp.click();
  await rp.click();
  await p.waitForTimeout(100);
  check(`${lang}: a second click closes it`, !openIdx(await tipState(p)).length);
  await rp.click();
  await mb.click();
  await p.waitForTimeout(100);
  s = await tipState(p);
  check(`${lang}: one open at a time`, openIdx(s).length === 1 && s[openIdx(s)[0]].exp === "true");
  await p.keyboard.press("Escape");
  // the footer: one line; its licences open on a click
  const fh = await p.evaluate(() => document.querySelector("#f-lic > summary").getBoundingClientRect().height);
  await p.locator("#f-lic > summary").click();
  const fo = await p.evaluate(() => ({ open: document.getElementById("f-lic").open, txt: document.getElementById("f-lic").innerText,
                                       boot: document.getElementById("f-bootlic").hidden === !JSON.parse(document.getElementById("fwdata").textContent)?.boot?.license }));
  check(`${lang}: the footer one line (${fh}px), a click shows the attributions (fm1-base, Felucca, MPL-2.0, m-bootloader's licence when embedded)`,
        fh < 24 && fo.open && /Leo Kuroshita/.test(fo.txt) && /Charles Vestal/.test(fo.txt) && /Chris Farrell/.test(fo.txt) &&
        /MPL-2\.0/.test(fo.txt) && /github\.com\/charlesvestal\/fm1-base/.test(fo.txt) && fo.boot);
  check(`${lang}: no script errors`, !errs.length);
  await ctx.close();
}

// a phone: one column, no sideways scrolling, a tap opens a "?" inside the window, a tap outside closes it
for (const lang of ["de", "en"]) {
  const { ctx, p, errs } = await open(390, lang, true);
  const R = await rects(p);
  const sw = await p.evaluate(() => document.documentElement.scrollWidth);
  check(`${lang} 390: one column (install, status, stock, report, Ko-fi), no sideways scrolling (${sw})`, sw <= 390 &&
        R.inst.bottom < R.stat.top && R.stat.bottom < R.stock.top && R.stock.bottom < R.rep.top && R.rep.bottom < R.kofi.top);
  const before = await rects(p);
  await p.locator("#i-mbootopt .tipb").tap();
  await p.waitForTimeout(150);
  let s = await tipState(p);
  const k = openIdx(s);
  check(`${lang} 390: a tap opens it, inside the window (${k.length && [s[k[0]].l, s[k[0]].r].join("..")}), nothing moved`, k.length === 1 &&
        s[k[0]].l >= 0 && s[k[0]].r <= 390 && s[k[0]].t >= 0 && s[k[0]].b <= 900 && JSON.stringify(await rects(p)) === JSON.stringify(before));
  await p.touchscreen.tap(200, 30);
  await p.waitForTimeout(150);
  s = await tipState(p);
  check(`${lang} 390: a tap outside closes it`, !openIdx(s).length);
  check(`${lang} 390: no script errors`, !errs.length);
  await ctx.close();
}
for (const lang of ["de", "en"]) {   // (1.18C48, a page of its own: the Patches tab visited leaves the install checks as they were)
  const { ctx, p } = await open(1280, lang);
  {   // (1.18C48) the Patches tab: "2 Preview" and "3 Upload into a bank" end flush, the export card's controls in two rows,
      // the tab no taller than measured (1280: 785 px, a few px of room)
    await p.click("#tab-patches");
    await p.waitForTimeout(150);
    const Q = await p.evaluate(() => {
      const g = [...document.querySelectorAll("#b-grid > .panel")].map((e) => e.getBoundingClientRect());
      const mids = [...document.querySelectorAll("#x-panel .row button:not([hidden]), #x-panel select, #x-panel input")]
        .filter((e) => e.offsetParent && !e.closest(".tip")).map((e) => { const r = e.getBoundingClientRect(); return (r.top + r.bottom) / 2; }).sort((a, b) => a - b);
      const rows = { size: mids.filter((m, i) => !i || m - mids[i - 1] > 14).length };   // (centres within 14 px: one row)
      return { pv: [Math.round(g[1].top), Math.round(g[1].bottom)], up: [Math.round(g[2].top), Math.round(g[2].bottom)], xrows: rows.size,
               tab: Math.round(document.getElementById("panel-patches").getBoundingClientRect().height) };
    });
    check(`${lang} 1280: Patches "Preview" and "Upload" flush (${JSON.stringify(Q)})`, Q.pv[0] === Q.up[0] && Q.pv[1] === Q.up[1]);
    check(`${lang} 1280: the export card's controls in two rows, the tab at most 800 px (${Q.xrows}, ${Q.tab})`, Q.xrows <= 2 && Q.tab <= 800);
  }
  await ctx.close();
}
// (the page pass after 1.18C48) the whole page's layout: each tab no taller at 1280 px than measured then (a few px of
// room: a regression guard), the cards of a row ending flush, the Editor on one Full HD screen (1920 x 940 and 900: no
// page scrolling, no panel scrolling inside in any sub-tab), 1366 x 768 without sideways scrolling, 390 px likewise
const TABH = { install: 600, waves: 395, patches: 645, org: 1092 };   // (measured 1280 x 900: 592, 387 / 368, 637, 1084 / 1066 px; +8)
const lay = (p) => p.evaluate(() => {
  const vis = (e) => e.offsetParent !== null;
  const rows = [...document.querySelectorAll("#panel-waves .grid > .panel, #b-grid > .panel, .org-cols > .panel")].filter(vis).map((e) => e.getBoundingClientRect());
  const bad = rows.filter((r) => rows.some((o) => Math.abs(o.top - r.top) < 1 && Math.abs(o.bottom - r.bottom) >= 1)).length;
  const tab = [...document.querySelectorAll(".tabpanel")].find((t) => !t.hidden);
  return { h: Math.round(tab.getBoundingClientRect().height), unflush: bad, sw: document.documentElement.scrollWidth };
});
for (const lang of ["de", "en"]) {
  const { ctx, p, errs } = await open(1280, lang);
  for (const [k, go] of [["install", "#tab-install"], ["waves", "#tab-waves"], ["patches", "#tab-patches"], ["org", "#org-open"]]) {
    await p.click(go);
    await p.waitForTimeout(200);
    const L = await lay(p);
    check(`${lang} 1280: ${k}: ${L.h} px (at most ${TABH[k]}), the cards of a row flush, no sideways scrolling`, L.h <= TABH[k] && !L.unflush && L.sw <= 1280);
  }
  await p.click("#org-open");
  check(`${lang} 1280: no script errors (layout)`, !errs.length);
  await ctx.close();
  for (const [w, h] of [[1920, 940], [1920, 900], [1366, 768]]) {
    const ctx2 = await browser.newContext({ viewport: { width: w, height: h } });
    await ctx2.addInitScript((l) => { try { localStorage.setItem("infekt.lang", l); localStorage.removeItem("infekt.tab"); } catch (_) { /* */ } }, lang);
    const q = await ctx2.newPage();
    await q.goto(url);
    await q.click("#tab-editor");
    await q.waitForTimeout(300);
    const E = await q.evaluate(() => {
      const pans = [...document.querySelectorAll(".edpan")], inner = [], taller = [];
      for (const sec of pans) {
        if (sec.scrollHeight > sec.clientHeight + 1) inner.push(sec.id);
        const sp = [...sec.querySelectorAll(".subpanel")], cur = sp.findIndex((x) => !x.hidden), h0 = sec.getBoundingClientRect().height;
        sp.forEach((x, i) => { sp.forEach((y, k) => (y.hidden = k !== i)); if (sec.getBoundingClientRect().height > h0 + 1 || sec.scrollHeight > sec.clientHeight + 1) taller.push(sec.id + "/" + i); });
        sp.forEach((y, k) => (y.hidden = k !== cur));
      }
      const hd = document.querySelector(".edhead"), ctl = hd.querySelector(".edctl").getBoundingClientRect(), lcd = hd.querySelector(".edlcd").getBoundingClientRect();
      return { doc: document.documentElement.scrollHeight, sw: document.documentElement.scrollWidth, n: pans.length, inner, taller,
               oneRow: Math.abs(ctl.top - lcd.top) < 12 && hd.getBoundingClientRect().height < 60, mx: document.querySelectorAll("#e-matrix .mxrow").length };
    });
    if (w === 1920) {
      check(`${lang} ${w}x${h}: the Editor on one screen (${E.doc} px), its head in one row, ${E.n} panels and the matrix's ${E.mx} rows, no panel scrolling inside (${E.inner.join() || "none"}), every sub-tab fits (${E.taller.join() || "all"})`,
            E.doc <= h && E.sw <= w && E.oneRow && E.n === 7 && E.mx === 9 && !E.inner.length && !E.taller.length);
    } else {
      check(`${lang} ${w}x${h}: the Editor without sideways scrolling, no panel scrolling inside (${E.doc} px)`, E.sw <= w && !E.inner.length);
    }
    await ctx2.close();
  }
}
await browser.close();
console.log(bad ? bad + " FAILED" : "install view all ok");
process.exit(bad ? 1 : 0);
