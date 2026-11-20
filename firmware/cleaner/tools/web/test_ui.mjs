// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 J0KERPACK
// The InfeKt-FM1 page under jsdom with a simulated FM-1 on a fake Web MIDI: the tabs (keys, remembered), the languages,
// the device display (identity, version, the app's protocols, a lost port, the update mode), Wavetables (a WAV sent,
// read back, cleared), Patches (512-byte singles converted, the preview with its hint, the arrow keys, an upload),
// the crash report, and an install end to end (the firmware, its loader, back as the new version) and its resume
// from the update mode (the Editor tab: test_editor.mjs). jsdom from the working directory's node_modules (npm install jsdom):
//   node test_ui.mjs PAGE.html     (PAGE: make_page.py's output with a release firmware embedded, FM-1_13xxyyzz: the
//   install ends on its identity and version, whichever it is)
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { webcrypto } from "node:crypto";
const { JSDOM, VirtualConsole } = createRequire(process.cwd() + "/")("jsdom");

const [html] = process.argv.slice(2);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let bad = 0;
const check = (what, ok) => { console.log((ok ? "ok   " : "FAIL ") + what); if (!ok) bad++; };
async function until(f, ms, step = 50) { const end = Date.now() + ms; while (Date.now() < end) { if (f()) return true; await sleep(step); } return !!f(); }

const vc = new VirtualConsole();
vc.on("jsdomError", (e) => { if (!/Not implemented/.test(e.message)) console.log("jsdom:", e.message); });
let dev = null;
const dom = new JSDOM(readFileSync(html, "utf8"), { runScripts: "dangerously", pretendToBeVisual: true, url: "http://localhost/infekt-fm1.html",
  virtualConsole: vc,
  beforeParse(w) {
    w.navigator.requestMIDIAccess = async () => dev.access;
    Object.defineProperty(w, "crypto", { value: webcrypto });
    w.TextEncoder = TextEncoder;               // (jsdom has none)
    w.HTMLDialogElement.prototype.showModal = function () { this.setAttribute("open", ""); };
    w.HTMLDialogElement.prototype.close = function (v) {
      if (v !== undefined) this.returnValue = v;
      this.removeAttribute("open"); this.dispatchEvent(new w.Event("close"));
    };
    w.Element.prototype.scrollIntoView = function () {};
    w.URL.createObjectURL = () => "blob:x"; w.URL.revokeObjectURL = () => {};
    w.HTMLAnchorElement.prototype.click = function () { w.downloads = (w.downloads || 0) + 1; };
    w.confirm = () => true;
  } });
const w = dom.window, d = w.document, $ = (id) => d.getElementById(id);
const FW = w.eval("FW"), BK = w.eval("BK"), WV = w.eval("WV");

// ---- the simulated FM-1: one port pair; the identity, the update protocol, the app's SysEx (patches, waves, log) ----
class FakeFM1 {
  constructor(name, ident) {
    this.sent = []; this.input = { name, state: "disconnected", connection: "closed", onmidimessage: null, open: async () => {}, close: async () => {} };
    this.output = { name, state: "disconnected", connection: "closed", open: async () => {}, close: async () => {}, send: (m) => this.rx(Array.from(m)) };
    this.access = { inputs: new Map([["i", this.input]]), outputs: new Map([["o", this.output]]), onstatechange: null, sysexEnabled: true };
    this.waves = new Map(); this.armed = 0; this.saved = 0; this.plan = []; this.reads = 0;
    this.buVer = 1;                      // (the patch hello's version: 3 from 1.18C30, six user banks)
    this.up(ident);
  }
  up(ident) { this.ident = ident; this.input.state = this.output.state = "connected"; this.ev(); }
  down() { this.input.state = this.output.state = "disconnected"; this.ev(); }
  ev() { const f = this.access.onstatechange; if (f) f({ port: this.input }); }
  tx(bytes) { setTimeout(() => this.input.state === "connected" && this.input.onmidimessage && this.input.onmidimessage({ data: Uint8Array.from(bytes) }), 2); }
  request(addr, len) {
    this.waiting = addr;
    const u = [0, 0x59, 0x30, 8, 0, 0, 0, addr & 0xFF, (addr >>> 8) & 0xFF, (addr >>> 16) & 0xFF, (addr >>> 24) & 0xFF, len & 0xFF, len >> 8, 0];
    let s = 0;
    for (let i = 6; i < 14; i++) s += u[i];
    this.tx([0xF0, ...FW.pack7([...u, ~s & 0xFF]), 0xF7]);
  }
  rx(m) {
    if (this.output.state !== "connected") throw new Error("disconnected");
    this.sent.push({ t: Date.now(), m });
    const j = m.join(), ota = /^ota-/.test(this.ident);
    if (j === FW.HS_QUERY.join()) {
      const t = [...this.ident].map((c) => c.charCodeAt(0));
      this.tx([0xF0, ...FW.pack7([0, 0x59, 0x11, 0, 0, 0, ...t, ...new Array(28 - t.length).fill(0)]), 0xF7]);
    } else if (j === FW.UPGRADE.join()) {
      if (this.deaf) return;             // (1.11 and older on the UC25HQ80: the update session never starts)
      this.queue = ota ? [[0, 512], [512, 512], [0xF0000000, 8]] : [[0, 512], [0xE0000000, 8]];
      this.request(...this.queue.shift());
    } else if (this.waiting != null && m[0] === 0xF0 && m.length > 20 && !(m[1] === 0x7D)) {
      const addr = this.waiting;
      this.waiting = null; this.reads++;
      if (addr >= 0xE0000000) {
        const next = this.plan.shift();
        setTimeout(() => this.down(), 20);
        if (next) setTimeout(() => this.up(next), 1200);
        return;
      }
      this.request(...this.queue.shift());
    } else if (ota) {
      return;                            // (the loader knows nothing else)
    } else if (m[0] === 0xF0 && m[1] === 0x7D && m[2] === 0x42 && m[3] === 0x55) {          // patches
      const c = m[4];
      if (c === 1) this.tx([0xF0, 0x7D, 0x42, 0x55, 0x41, this.buVer, 0xF7]);
      else if (c === 2 && m[5] >= (this.buVer >= 3 ? 6 : 3)) this.tx([0xF0, 0x7D, 0x42, 0x55, 0x42, 0x7F, 2, 0xF7]);   // (no such bank)
      else if (c === 2) { this.armed = 1; this.bank = m[5]; this.saved = 0; this.tx([0xF0, 0x7D, 0x42, 0x55, 0x42, m[5], 0, 0xF7]); }
      else if (c === 3 || c === 4) { this.armed = 0; this.tx([0xF0, 0x7D, 0x42, 0x55, 0x43, 0, this.saved >> 7, this.saved & 127, 0, 0xF7]); }
    } else if (m[0] === 0xF0 && m.length === 267 && m[1] === 0) {
      if (this.armed) this.saved++;
      setTimeout(() => this.tx([0xF0, 0x7D, 0x42, 0x55, 0x50, m[8], 0, this.armed ? 1 : 0, 0xF7]), 40);
    } else if (m[0] === 0xF0 && m[1] === 0x7D && m[2] === 0x56 && m[3] === 0x46) {          // waves
      const c = m[4], H = [0xF0, 0x7D, 0x56, 0x46];
      const crc3 = (c) => [(c >> 14) & 3, (c >> 7) & 0x7F, c & 0x7F];
      if (c === 0x01) {
        const bits = new Array(9).fill(0);
        for (const s of this.waves.keys()) bits[Math.floor((s - 2) / 7)] |= 1 << ((s - 2) % 7);
        this.tx([...H, 0x41, 1, 62, 2, ...bits, 0xF7]);
      } else if (c === 0x10) {
        const rec = WV.unpack7(Uint8Array.from(m.slice(6, m.length - 4)), WV.W.REC);
        this.waves.set(m[5], rec);
        this.tx([...H, 0x50, 0x10, m[5], 0, ...crc3(WV.crc16(rec)), 0xF7]);
      } else if (c === 0x13) {
        const rec = this.waves.get(m[5]) || WV.record([1, ...new Array(63).fill(0)], new Array(64).fill(0), "FACTORY");
        this.tx([...H, 0x53, m[5], this.waves.has(m[5]) ? 1 : 0, ...WV.pack7(Array.from(rec)), ...crc3(WV.crc16(rec)), 0xF7]);
      } else if (c === 0x11) { this.waves.delete(m[5]); this.tx([...H, 0x50, 0x11, m[5], 0, 0, 0, 0, 0xF7]); }
      else if (c === 0x12) { this.waves.clear(); this.tx([...H, 0x50, 0x12, 0, 0, 0, 0, 0, 0xF7]); }
    } else if (j === [0xF0, 0x7D, 0x4C, 0x47, 0xF7].join()) {                                // the crash log
      for (const line of ["crash: none", "boot 3", "end"]) this.tx([0xF0, 0x7D, 0x4C, 0x47, ...[...line].map((c) => c.charCodeAt(0)), 0xF7]);
    }
  }
}
dev = new FakeFM1("Viral-FM1", "FM-1_13009400");
const lcd = () => [1, 2, 3, 4].map((k) => $("d-l" + k).textContent).join(" | ");

// ---- the page as it opens ----
await sleep(300);
const PROD = w.infekt.I.product, VER = FW.versionOf(PROD).version, VRE = VER.replace(/\./g, "\\.");   // (the embedded package's)
check("five tabs, Installation shown", d.querySelectorAll(".tabs [role=tab]").length === 5 && !$("panel-install").hidden && $("panel-waves").hidden &&
      $("panel-editor").hidden && $("panel-guide").hidden);
check("German first", d.documentElement.lang === "de" && $("tab-install").textContent === "Installation" && /Verbinden/.test($("d-connect").textContent));
check(`the embedded firmware: ${$("i-fw1").textContent}`, /^FM-1_(49|13)\d{6}$/.test(PROD) && new RegExp("InfeKt-FM1 " + VRE).test($("i-fw1").textContent) &&
      !$("i-go").disabled && new RegExp(VRE).test($("i-go").textContent));
check("m-bootloader offered (its SHA-256 checked), its licence shown when embedded", !$("i-mbootopt").hidden &&
      $("f-bootlic").hidden === !JSON.parse($("fwdata").textContent).boot.license);
$("tab-install").dispatchEvent(new w.KeyboardEvent("keydown", { key: "ArrowRight", bubbles: true }));
check("ArrowRight: Wavetables, remembered", !$("panel-waves").hidden && $("tab-waves").getAttribute("aria-selected") === "true" &&
      w.localStorage.getItem("infekt.tab") === "waves" && $("tab-waves").tabIndex === 0 && $("tab-install").tabIndex === -1);
$("tab-waves").dispatchEvent(new w.KeyboardEvent("keydown", { key: "End", bubbles: true }));
// the Guide tab: the quickstart (../quickstart/fragment.py, embedded by make_page.py) in the page's language, dark, in a
// shadow root (its styles and the page's apart)
const gsec = () => { const r = $("g-host").shadowRoot; return r && r.querySelector("section.qs"); };
check("End: Guide (Anleitung), remembered, the page not wide", !$("panel-guide").hidden && $("panel-editor").hidden &&
      $("tab-guide").textContent === "Anleitung" && $("tab-guide").getAttribute("aria-selected") === "true" &&
      w.localStorage.getItem("infekt.tab") === "guide" && !d.body.classList.contains("wide") && d.activeElement === $("tab-guide"));
check("the guide in German, dark, in its own shadow root (none of it in the page's DOM)", gsec() && gsec().lang === "de" &&
      gsec().classList.contains("qs-dark") && /Bedienelemente/.test(gsec().textContent) && !d.querySelector("#panel-guide section.qs") &&
      gsec().querySelector("style") && $("g-none").hidden && /Installer-Ordner/.test($("panel-guide").querySelector(".guidepdf").textContent));
check("its SVGs' ids prefixed per language (qsde-)", gsec().querySelector("#qsde-bm") && !gsec().querySelector("#bm"));
$("tab-guide").dispatchEvent(new w.KeyboardEvent("keydown", { key: "ArrowRight", bubbles: true }));
check("ArrowRight from Guide: back to Installation", !$("panel-install").hidden && $("panel-guide").hidden);
$("tab-install").dispatchEvent(new w.KeyboardEvent("keydown", { key: "ArrowLeft", bubbles: true }));
check("ArrowLeft from Installation: Guide", !$("panel-guide").hidden);
$("tab-guide").dispatchEvent(new w.KeyboardEvent("keydown", { key: "ArrowLeft", bubbles: true }));
check("ArrowLeft: Editor (the page wider)", !$("panel-editor").hidden && d.body.classList.contains("wide"));
$("tab-editor").dispatchEvent(new w.KeyboardEvent("keydown", { key: "ArrowLeft", bubbles: true }));
check("ArrowLeft: Patches", !$("panel-patches").hidden && !d.body.classList.contains("wide"));
$("lang").click();
check("English", d.documentElement.lang === "en" && $("tab-install").textContent === "Install" && $("lang").textContent === "Deutsch" && /Connect/.test($("d-connect").textContent));
$("tab-guide").click();
check("English: the Guide tab, the guide in English", $("tab-guide").textContent === "Guide" && !$("panel-guide").hidden && gsec().lang === "en" &&
      gsec().classList.contains("qs-dark") && /Controls|controls/.test(gsec().textContent) && !/Bedienelemente/.test(gsec().textContent) &&
      gsec().querySelector("#qsen-bm") && /installer folder/.test($("panel-guide").querySelector(".guidepdf").textContent));
$("lang").click();
check("back to German: the guide too", gsec().lang === "de" && /Bedienelemente/.test(gsec().textContent));

// ---- the Installation tab since 1.18C47w2: the explanations behind "?" (each still in the page, inside a tooltip, in
// both languages), the licences behind the footer's one line, no boot image card, the Ko-fi card ----
const TIPS = {
  de: [/USB-Kabel direkt \(Datenkabel\) · Chrome\/Edge\/Brave · MIDI erlauben/, /Beim Schreiben USB-Kabel nicht abziehen\./,
       // (the page pass after 1.18C48: m-bootloader's text as a title, a list, the warning after a line: its facts)
       ["Auch m-bootloader installieren", "beim Einschalten PLAY: Rettungsmenü · REC: Update-Modus", "nur FM-1 mit dem ursprünglichen Flash-Chip (Puya)"],
       ["Offener Bootloader von Julius Vitkauskas (MIT)", "ersetzt den des Herstellers", "bleibt bei Updates", "„Zurück zur Original-Firmware“ stellt den originalen wieder her",
        "WIPE USER DATA löscht alle gespeicherten Daten", "Neuere FM-1 mit UC25HQ80-Flash (GLO > ABOUT: FLASH B36014): InfeKt-FM1 lehnt m-bootloader dort ab"],
       ["m-bootloader auf GitHub"],
       /Per USB · aus InfeKt-FM1, Safe Mode oder anderer Firmware · Gespeichertes kann verloren gehen\./,
       /Oder Hersteller-Updater \(Downloads → PC Software\):/, /Download-Seite des Herstellers/,
       /Nach Absturz nicht ausschalten \(Bericht übersteht nur Neustart\) · USB · lesen, kopieren, mit Beschreibung schicken\./,
       /Diese Ports werden nicht von selbst gefragt\. FM-1 dabei: „Probieren“\./,
       // (1.18C47w3) the other tabs: Wavetables, Patches, the patch manager, the Editor's launcher sets
       /Single-Cycle-WAVs \(jede Länge, Rate, Bittiefe\) → Wellen 2–63 · InfeKt-FM1 ab 0\.90 · USB oder Bluetooth-MIDI · freier Platz = Werkswelle/,
       /W12_glass\.wav → Platz 12 „glass“ · sonst der Reihe nach ab Platz unten/,
       /Bänke und Einzel-Patches ordnen: lokal ↔ FM-1, kopieren, verschieben, umbenennen, sichern/,
       /Links lokale Bänke \(geöffnete Dateien, Einzel-Patches, neue Bänke\), rechts die Bänke A–F des FM-1\. Ziehen mit der Maus kopiert zwischen den Seiten \(mit Shift: verschieben\) und ordnet innerhalb einer Seite um \(mit Strg: kopieren\); Dateien vom Desktop auf eine Liste ziehen öffnet sie\. Änderungen am FM-1 werden nur vorgemerkt \(gelb\) – erst Auf FM-1 schreiben speichert sie im Flash\./,
       /Klick wählt, Strg\/⌘-Klick oder Kästchen: mehrere, Shift: Bereich\. Ziel: ab dem gewählten Platz der anderen Seite, sonst freie Plätze\. Doppelklick oder F2: umbenennen, Entf: leeren, Strg\+Z: rückgängig\./,
       /Single-Dumps \(256 Byte\) aus \.syx\/\.mid → Bank A–F oder Edit-Puffer · 512-Byte-Singles des Nachfolgemodells werden umgewandelt · nur USB/,
       /Listet jeden Single-Dump; anderes wird ausgelassen\./,
       /▶ = in den Edit-Puffer \(nicht gespeichert; SAVE am FM-1 behält ihn\) · ↑ \/ ↓ vorige \/ nächste · Leertaste noch einmal/,
       /Gleicher USB-Port, dieser Kanal \(FM-1: GLO > MIDI > CHANNEL, ab Werk OMNI; IN PORTS USB an; nicht der DRUM CHANNEL, ab Werk 10: dort spielen die Drums\)/,
       /Patches aus dem FM-1 lesen und als Datei speichern · Bänke A–F oder der Edit-Puffer · nur USB/,
       /InfeKt-Format: jeder Patch als Single-Dump \(256 Byte\), dahinter eine eigene Nachricht mit der Erweiterung \(Shimmer\); andere Software überspringt sie\./,
       /Die 16 Sets des Pattern-Launchers \(Reihen, Lautstärken, Drum-Kit, Tempo, Patch A und B als Verweis\): als Datei sichern und in einen Slot laden\. Ein Import schreibt sofort in den Flash des FM-1 \(nur gestoppt\)\./],
  en: [/USB cable direct \(data cable\) · Chrome\/Edge\/Brave · allow MIDI/, /Do not unplug USB while writing\./,
       ["Also install m-bootloader", "at power-on PLAY: recovery menu · REC: update mode", "only FM-1s with the original flash chip (Puya)"],
       ["Open bootloader by Julius Vitkauskas (MIT)", "replaces the maker's", "stays through updates", "\"Back to the stock firmware\" restores the original",
        "WIPE USER DATA erases all saved data", "Newer FM-1s with the UC25HQ80 flash (GLO > ABOUT: FLASH B36014): InfeKt-FM1 refuses m-bootloader there"],
       ["m-bootloader on GitHub"],
       /Over USB · from InfeKt-FM1, safe mode or another firmware · saved data may be lost\./,
       /Or the maker's updater \(Downloads → PC Software\):/, /the maker's download page/,
       /After a crash don't switch off \(the report survives a restart only\) · USB · read, copy, send with a description\./,
       /These ports are not asked by themselves\. FM-1 among them: press Try\./,
       /Single-cycle WAVs \(any length, rate, bit depth\) → waves 2-63 · InfeKt-FM1 0\.90\+ · USB or Bluetooth MIDI · empty slot = factory wave/,
       /W12_glass\.wav → slot 12 "glass" · others in order from the slot below/,
       /Arrange banks and single patches: local ↔ FM-1, copy, move, rename, back up/,
       /Left: local banks \(opened files, single patches, new banks\); right: the FM-1's banks A–F\. Dragging copies between the sides \(with Shift: moves\) and rearranges within a side \(with Ctrl: copies\); dropping files from the desktop on a list opens them\. Changes to the FM-1 are only queued \(yellow\) – Write to FM-1 stores them in its flash\./,
       /Click selects, Ctrl\/⌘-click or the box: several, Shift: a range\. Target: from the slot selected on the other side, else free slots\. Double-click or F2: rename, Del: clear, Ctrl\+Z: undo\./,
       /Single dumps \(256 bytes\) from \.syx\/\.mid → bank A–F or edit buffer · the successor model's 512-byte singles are converted · USB only/,
       /Lists every single dump; anything else is left out\./,
       /▶ = into the edit buffer \(not saved; SAVE on the FM-1 keeps it\) · ↑ \/ ↓ previous \/ next · Space again/,
       /Same USB port, this channel \(FM-1: GLO > MIDI > CHANNEL, OMNI as shipped; IN PORTS USB on; not the DRUM CHANNEL, 10 as shipped: it plays the drums\)/,
       /Read patches from the FM-1 and save them as a file · banks A–F or the edit buffer · USB only/,
       /InfeKt format: each patch as a single dump \(256 bytes\), followed by its own message with the extension \(shimmer\); other software skips it\./,
       /The pattern launcher's 16 sets \(rows, volumes, drum kit, tempo, patch A and B as references\): save them as files, load a file into a slot\. An import writes the FM-1's flash at once \(stopped only\)\./],
};
const LIC = {
  de: [/InfeKt-FM1: Firmware für das FM-1 auf fm1-base und felucca-bsp \(aus Felucca von Leo Kuroshita\)\. FM-1 ist eine Marke ihres Inhabers\./,
       /Installer-Teil: MPL-2\.0, ohne Gewähr, aus fm1-base von Charles Vestal \(mit Teilen von Chris Farrell\):/,
       /Offline: alles in dieser einen Datei\./, /m-bootloader: Lizenz \(MIT\)/],
  en: [/InfeKt-FM1: firmware for the FM-1 on fm1-base and felucca-bsp \(from Felucca by Leo Kuroshita\)\. FM-1 is a trademark of its owner\./,
       /Installer part: MPL-2\.0, no warranty, from fm1-base by Charles Vestal \(with parts by Chris Farrell\):/,
       /Offline: everything in this one file\./, /m-bootloader: licence \(MIT\)/],
};
// (since the page pass after 1.18C48 the tooltips are laid out: a title, the facts as a list, a second section after a
// line; compared as normalised text: case, punctuation, quotes and the list's separators left out. An entry: a former
// explanation (its text, as the regular expression it was) or the facts it was split into)
const tipTxt = (e) => { const c = e.cloneNode(true); c.querySelectorAll("li, .tp, .th, .th2, .tl").forEach((x) => x.append(" · ")); return c.textContent.replace(/\s+/g, " "); };
const tipText = () => [...d.querySelectorAll(".tip .tipt")].map(tipTxt).join("\n");
const norm = (x) => x.toLowerCase().replace(/[„“”"'’]/g, "").replace(/[\s·;:,.()–—!?-]+/g, " ").trim();
const factsOf = (e) => (Array.isArray(e) ? e : [e.source.replace(/\\(.)/g, "$1")]);
const present = (tt, e) => factsOf(e).every((f) => norm(tt).includes(norm(f)));
const licText = () => [...$("f-lic").children].filter((e) => e.tagName !== "SUMMARY").map((e) => e.textContent.replace(/\s+/g, " ")).join("\n");
for (const L of ["de", "en"]) {
  if (d.documentElement.lang !== L) $("lang").click();
  const tt = tipText(), miss = TIPS[L].filter((e) => !present(tt, e));
  check(`${L}: every former explanation of the Installation, Wavetables, Patches tabs, the patch manager and the launcher sets inside a "?" tooltip (hidden until asked)${miss.length ? " MISSING " + miss.join(", ") : ""}`,
        !miss.length && [...d.querySelectorAll(".tipt")].every((e) => e.hidden) &&
        $("i-mbootopt").querySelector('.tipt a[href="https://github.com/jvitkauskas/m-bootloader"][target="_blank"]') &&
        [...d.querySelectorAll(".tipb")].every((b) => b.getAttribute("aria-expanded") === "false" && b.getAttribute("aria-controls") &&
          $(b.getAttribute("aria-controls")) && b.getAttribute("aria-label")));
  const lt = licText(), lmiss = LIC[L].filter((re) => !re.test(lt));
  check(`${L}: the footer one line, the attributions and licences behind it${lmiss.length ? " MISSING " + lmiss.join(", ") : ""}`,
        !lmiss.length && !$("f-lic").open && /fm1-base and felucca-bsp|fm1-base und felucca-bsp/.test($("f-lic").querySelector("summary").textContent) &&
        /MPL-2\.0/.test($("f-lic").querySelector("summary").textContent) && /m-bootloader \(MIT\)/.test($("f-lic").querySelector("summary").textContent) &&
        $("f-lic").querySelector('a[href="https://github.com/charlesvestal/fm1-base"]') && $("f-lic").contains($("f-bootlic")) &&
        (!JSON.parse($("fwdata").textContent).boot.license || $("f-bootlictext").textContent === JSON.parse($("fwdata").textContent).boot.license));
  check(`${L}: visible, short: the overwrite warnings (waves, upload), the queue until "write", the launcher import's flash`,
        /Überschreibt belegte Plätze ohne Nachfrage|Overwrites filled slots without asking/.test($("panel-waves").textContent) &&
        /Überschreibt belegte Plätze dieser Bank|Overwrites filled slots in that bank/.test($("panel-patches").textContent) &&
        [...d.querySelectorAll("#org-box .org-head h2 .hsub > span:first-child, #ls-box p.small > span:first-child")].map((e) => e.textContent).join(" | ")
          .match(/(Auf FM-1 schreiben|Write to FM-1).*(Flash|flash)/) &&
        !d.querySelector("#panel-waves .drop .small") && !d.querySelector("#panel-patches .drop .small"));
  check(`${L}: the Ko-fi card`, /SMK-37/.test($("panel-install").querySelector(".ic-kofi").textContent) &&
        $("panel-install").querySelector('.ic-kofi a[href="https://ko-fi.com/j0kerpack"][target="_blank"]'));
  check(`${L}: visible: the backup warning, the file chooser, the install button, progress and log`,
        /Zuerst sichern|Back up first/.test($("panel-install").querySelector(".ic-inst .note:not([hidden])").textContent) && $("i-fwfile") && $("i-go") &&
        $("i-bar") && $("i-log") && !$("i-fwfile").closest(".tipt") && !$("panel-install").querySelector(".ic-inst .note:not([hidden])").closest(".tipt"));
}
if (d.documentElement.lang !== "de") $("lang").click();
check("no boot image card, no boot image data", !$("i-bi") && !$("i-bisend") && !$("bidata") && !/Startbild|boot image/i.test($("panel-install").textContent));
w.location.hash = "#waves";
await sleep(50);
w.location.hash = "#guide";
await sleep(50);
check("#guide in the address: the Guide tab", !$("panel-guide").hidden && $("panel-waves").hidden);
w.infekt.showTab("patches");

// ---- the device ----
$("d-connect").click();
await until(() => w.infekt.DEV.state === "ok", 8000);
check(`connected: ${lcd()}`, /Verbunden/.test(lcd()) && /Viral-FM1/.test(lcd()) && /InfeKt-FM1 0\.94/.test(lcd()) && /Patches ja · Wellen ja/.test(lcd()));
check("the identity asked first (no other gear probed)", dev.sent[0].m.join() === FW.HS_QUERY.join());

// ---- Wavetables ----
w.infekt.showTab("waves");
const N = 600, saw = Array.from({ length: N }, (_, k) => 0.7 * (2 * k / N - 1));
const wav = WV.wavFloat(saw, 48000);
await w.infekt.wv.addFiles([{ name: "W12_saw.wav", arrayBuffer: async () => wav.buffer.slice(0) }]);
check("a WAV listed for slot 12", w.infekt.wv.waves.length === 1 && w.infekt.wv.waves[0].slot === 12 && d.querySelectorAll("#w-list .wave").length === 1);
$("w-send").click();
await until(() => /1 von 1/.test($("w-status").textContent), 4000);
check(`sent: ${$("w-status").textContent}`, dev.waves.has(12) && /1 von 1 Wellen gesendet/.test($("w-status").textContent));
await until(() => /12/.test($("w-bank").textContent), 2000);
check(`the FM-1's waves: ${$("w-bank").textContent}`, /Plätzen: 12/.test($("w-bank").textContent));
$("w-slot").value = "12"; $("w-dump1").click();
await until(() => w.downloads === 1, 3000);
check("read back as a WAV", w.downloads === 1 && /eigene Welle/.test($("w-log").textContent));
$("w-clear1").click();
await until(() => /Werkswelle/.test($("w-status").textContent), 3000);
check("slot 12 back to the factory wave", !dev.waves.has(12));

// ---- Patches: 512-byte singles of the bigger model, converted; the preview; an upload ----
w.infekt.showTab("patches");
function big(k) {
  const d1 = new Uint8Array(256).map((_, i) => (i * 7 + k * 13) % 100), b2 = new Uint8Array(256);
  [..."PATCH" + String(k).padStart(3, "0") + "  "].forEach((c, i) => (d1[240 + i] = c.charCodeAt(0)));
  d1[51] = 4; b2[30] = 1; b2[1] = 1; b2[2] = 80;
  const f = new Uint8Array(524);
  f.set([0xF0, 0, 0x20, 0x33, 0x01, 0, 0x10, 1, k]); f.set(d1, 9); f.set(b2, 266); f[523] = 0xF7;
  let s = 0; for (let i = 5; i < 265; i++) s += f[i];
  f[265] = s & 0x7F;
  return f;
}
const syx = Uint8Array.from([...Array.from({ length: 10 }, (_, k) => [...big(k)]).flat()]);
await w.infekt.bk.addFiles([{ name: "big.syx", arrayBuffer: async () => syx.buffer.slice(0) }]);
const S = w.infekt.bk.singles;
check(`${S.length} singles, all converted, tagged`, S.length === 10 && S.every((s) => s.conv) && d.querySelectorAll("#b-list .tag").length === 10 &&
      d.querySelectorAll("#b-list tr.notes").length === 10 && /größeren Nachfolgemodells, umgewandelt/.test($("b-log").textContent));
$("b-pvlen").value = "0.3"; $("b-pvwait").value = "0.2";
const rows = () => [...d.querySelectorAll("#b-list tbody tr:not(.notes)")];
let n0 = dev.sent.length;
rows()[3].querySelector("button.play").click();
await sleep(50);
check("the hint dialog before the first preview", $("b-warn").open && /UNDO am FM-1/.test($("b-warn").textContent));
$("b-warnyes").click();
await sleep(300);
const since = (k) => dev.sent.slice(k), singlesIn = (a) => a.filter((x) => x.m.length === 267);
let a = since(n0), ss = singlesIn(a);
check("CC 123, then the converted single into the edit buffer (prog 3)", ss.length === 1 && ss[0].m[8] === 3 && ss[0].m[7] === 0 &&
      a.findIndex((x) => x.m[0] === 0xB0 && x.m[1] === 123) < a.indexOf(ss[0]) && ss[0].m.slice(9, 265).every((v, i) => v === S[3].data[i]));
const on = a.filter((x) => (x.m[0] & 0xF0) === 0x90);
check("a note on C3 (48) on channel 1 after the answer", on.length === 1 && on[0].m[1] === 48 && on[0].t >= ss[0].t + 30);
await sleep(400);
check("the note off after the length", since(n0).some((x) => x.m[0] === 0x80 && x.m[1] === 48));
check(`the status: ${$("b-status").textContent}`, /im Edit-Puffer \(umgewandelt\)/.test($("b-status").textContent));
n0 = dev.sent.length;
d.body.focus();
d.dispatchEvent(new w.KeyboardEvent("keydown", { key: "ArrowDown", bubbles: true }));
await sleep(200);
check("ArrowDown: no dialog, row 4 sent", !$("b-warn").open && singlesIn(since(n0)).map((x) => x.m[8]).join() === "4" && rows()[4].classList.contains("cur"));
for (let k = 0; k < 3; k++) d.dispatchEvent(new w.KeyboardEvent("keydown", { key: "ArrowDown", bubbles: true }));
await sleep(500);
check("fast arrows: the last one wins (row 7)", singlesIn(since(n0)).at(-1).m[8] === 7);
$("b-pvmode").value = "off";
await sleep(600);
w.eval("void 0");
S.forEach((s, k) => (s.sel = k < 3)); w.infekt.bk.render();
check("Upload button counts", /3 in Bank A laden/.test($("b-upload").textContent));
n0 = dev.sent.length;
await w.infekt.bk.upload();
a = since(n0);
check("upload: arm, 3 singles to A001..A003, end", a.some((x) => x.m[4] === 2 && x.m[1] === 0x7D) && singlesIn(a).map((x) => x.m[8]).join() === "0,1,2" &&
      a.at(-1).m[4] === 3 && /Fertig: 3 in Bank A gespeichert/.test($("b-status").textContent));
const offs = (id) => [...$(id).options].filter((o) => o.disabled).map((o) => o.textContent).join("");
check(`a firmware before 1.18C30 (hello 1): banks D-F off (${offs("b-bank")}, ${offs("x-bank")})`,
      offs("b-bank") === "DEF" && offs("x-bank") === "DEF");
// (1.18C30) six user banks: the hello's version 3, an upload into bank F
dev.buVer = 3;
w.infekt.bk.link.close(); w.infekt.bk.link = null;
S.forEach((s, k) => (s.sel = k >= 3 && k < 5));
$("b-bank").value = "5"; w.infekt.bk.render();
check("Upload button: bank F", /2 in Bank F laden/.test($("b-upload").textContent));
n0 = dev.sent.length;
await w.infekt.bk.upload();
a = since(n0);
check("upload into F: armed for bank 5, 2 singles, end", a.some((x) => x.m[4] === 2 && x.m[1] === 0x7D && x.m[5] === 5) &&
      singlesIn(a).length === 2 && dev.bank === 5 && a.at(-1).m[4] === 3 && /Fertig: 2 in Bank F gespeichert/.test($("b-status").textContent));
check("hello 3: no bank off", offs("b-bank") === "" && offs("x-bank") === "");
// a firmware before it, bank F chosen before the page knew: not sent (the page says why), nothing written
dev.buVer = 1;
w.infekt.bk.link.close(); w.infekt.bk.link = null;
$("b-bank").value = "5";
n0 = dev.sent.length;
await w.infekt.bk.upload();
a = since(n0);
check(`bank F on an older firmware: not sent, said (${$("b-status").textContent}), still chosen, D-F off`,
      /gibt es erst ab Firmware 1\.18C30/.test($("b-status").textContent) && $("b-bank").value === "5" && offs("b-bank") === "DEF" &&
      !dev.sent.slice(n0).some((x) => x.m[4] === 2 && x.m[1] === 0x7D) && singlesIn(dev.sent.slice(n0)).length === 0);
$("b-bank").value = "0"; w.infekt.bk.render();
dev.buVer = 1;

// ---- the crash report ----
w.infekt.showTab("install");
$("i-reportgo").click();
await until(() => !$("i-report").hidden, 6000);
check("the crash report read", /FM-1_13009400 \(Viral-FM1\)/.test($("i-report").value) && /boot 3/.test($("i-report").value));
await sleep(100);
check("the patch link still answers after it (the dispatcher back)", !!(await w.infekt.bk.link.ask(BK.buMsg(1), (r) => r.cmd === 0x41, 1500)));

// ---- a lost port, and back ----
dev.down();
check(`the port lost: ${lcd()}`, /Getrennt/.test(lcd()));
dev.up("FM-1_13009400");
await until(() => w.infekt.DEV.state === "ok", 6000);
check("found again by itself", w.infekt.DEV.state === "ok");
// ---- added (1.19.5): the identity's prefix FM-1_49; a device with the former FM-1_13 (1.19) and one with the new
// prefix both this firmware, named with their versions ----
for (const [id, re] of [["FM-1_13011900", /InfeKt-FM1 1\.19(?!\.)/], ["FM-1_49011905", /InfeKt-FM1 1\.19\.5/], ["FM-1_4900", /InfeKt-FM1/]]) {
  dev.down(); dev.up(id);
  await until(() => w.infekt.DEV.state === "ok", 6000);
  check(`${id} found as this firmware: ${lcd()}`, w.infekt.DEV.state === "ok" && re.test(lcd()));
}
dev.down(); dev.up("FM-1_13009400");
await until(() => w.infekt.DEV.state === "ok", 6000);

// ---- an install end to end: 0.94 -> the loader -> the embedded one ----
dev.plan = ["ota-FM-1_1", PROD];
$("i-mboot").checked = false;
$("i-go").click();
await sleep(50);
check("the confirmation first, Install off until the backup is ticked", $("i-confirm").hasAttribute("open") && $("i-confirmgo").disabled);
$("i-bakok").checked = true; $("i-bakok").dispatchEvent(new w.Event("change"));
check("ticked: Install on", !$("i-confirmgo").disabled);
$("i-confirm").close("go");
await until(() => w.infekt.DEV.writing, 2000);
check(`writing: ${lcd()}`, /Schreibt/.test(lcd()) && $("d-connect").disabled);
await w.infekt.bk.upload();
check("the patches wait while it writes", /Firmware installiert/.test($("b-status").textContent));
await until(() => /Fertig/.test($("i-status").textContent), 30000, 100);
if (!/Fertig/.test($("i-status").textContent)) console.log($("i-alert").textContent + "\n" + $("i-log").textContent.slice(-1500));
check(`installed: ${$("i-status").textContent}, bar ${$("i-bar").value}`, /^Fertig/.test($("i-status").textContent) && +$("i-bar").value === 100 && !$("i-alert").textContent);
await until(() => w.infekt.DEV.state === "ok", 8000);
check(`the header shows the new version: ${lcd()}`, new RegExp("InfeKt-FM1 " + VRE).test(lcd()) && !w.infekt.DEV.writing);

// ---- the update mode (REC held while switching on with m-bootloader): found, then resumed ----
dev.down();
dev.up("ota-FM-1_1");
dev.plan = [PROD];
await w.infekt.scan();
check(`update mode: ${lcd()}`, /Update-Modus/.test(lcd()) && !$("i-updmode").hidden);
await w.infekt.bk.upload();
check("the patches refuse in update mode", /Update-Modus/.test($("b-status").textContent));
$("i-go").click(); await sleep(30);
$("i-bakok").checked = true; $("i-bakok").dispatchEvent(new w.Event("change"));
$("i-confirm").close("go");
await until(() => w.infekt.DEV.writing, 2000);   /* (the first install's "Fertig" is still there until it starts) */
await until(() => /Fertig/.test($("i-status").textContent) && !w.infekt.DEV.writing, 30000, 100);
await until(() => w.infekt.DEV.state === "ok", 8000);
check(`resumed from the update mode: ${$("i-status").textContent}; ${lcd()}`, /^Fertig/.test($("i-status").textContent) && new RegExp(VRE).test(lcd()) && $("i-updmode").hidden);

// ---- InfeKt 1.10 on a newer FM-1 (the UC25HQ80): the update request answered by nothing (0 requests): the rescue
// tools named, in both languages ----
dev.down();
dev.up("FM-1_13011000");
dev.deaf = true;
await w.infekt.scan();
check(`1.10 found: ${lcd()}`, /InfeKt-FM1 1\.10/.test(lcd()));
const nsent = dev.sent.length;
$("i-go").click(); await sleep(30);
$("i-bakok").checked = true; $("i-bakok").dispatchEvent(new w.Event("change"));
$("i-confirm").close("go");
await until(() => w.infekt.DEV.writing, 2000);
await until(() => !w.infekt.DEV.writing, 30000, 100);
const al = $("i-alert").textContent;
check(`stopped after 0 requests on 1.10: ${al.slice(0, 90)}...`, /0 Anfragen/.test(al) && /InfeKt-FM1 1\.10/.test(al) && /UC25HQ80/.test(al) &&
      /rescue\//.test(al) && /LIESMICH-RESCUE/.test(al) && !/\{d\}/.test(al) && !$("i-status").textContent &&
      dev.sent.slice(nsent).filter((x) => x.m.join() === FW.UPGRADE.join()).length === 1);
$("lang").click();
check("the same in English", /0 requests/.test($("i-alert").textContent) && /UC25HQ80/.test($("i-alert").textContent) &&
      /rescue\/ folder/.test($("i-alert").textContent) && /InfeKt-FM1 1\.10/.test($("i-alert").textContent));
$("lang").click();
dev.deaf = false;

console.log(bad ? bad + " FAILED" : "ui all ok");
process.exit(bad ? 1 : 0);
