// SPDX-License-Identifier: MPL-2.0
// Copyright (C) 2026 Charles Vestal
// The InfeKt-FM1 page's installer core (infekt-fm1.html's script "fwcore": fm1-base's fm1pkg.js and fm1ota.js) against
// a simulated FM-1 on a fake WebMIDI; a port of fm1-base's tests/installer_ports_test.mjs and installer_boot_test.mjs:
// a port with an unusual name is asked only once allowed; an update's loader and the restarted firmware are followed when
// they come back under other port names; the maker's Open firmware's identities; resume checks the FM-1 came back as
// the product; a damaged package is caught by its checksums; the m-bootloader step (installBoot). Added here: the port
// names of this firmware (its former name and InfeKt-FM1) are asked by themselves, and the identity's version.
//   node test_installer.mjs M-BOOTLOADER.fwsc        (m-bootloader-1.2.fwsc: an FM-1 package with an installer)
import fs from "fs";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

const here = fileURLToPath(new URL(".", import.meta.url));
const src = fs.readFileSync(here + "infekt-fm1.html", "utf8").match(/<script id="fwcore">([\s\S]*?)<\/script>/)[1];
const ctx = { module: { exports: {} }, setTimeout, clearTimeout, TextEncoder, console };
vm.runInNewContext(src, ctx);
const { Updater, pack7, unpack7, productOf, logicalImage, ufwEntries, checkUfw, versionOf, PORT_NAMES, parseIdentity, foreignOf, IS_FM1_ID, FLASH_PARTS, OLD_GATE } = ctx.module.exports;

let fails = 0;
const check = (ok, msg) => { console.log(`${msg.padEnd(66)} ${ok ? "ok" : "FAIL"}`); if (!ok) fails++; };
const raw = new Uint8Array(fs.readFileSync(process.argv[2]));
const image = logicalImage(raw);

// ==== the port names (installer_ports_test.mjs) ====
const HS = [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7].join();
const UP = [0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7].join();


// one WebMIDI port pair that a device can appear on and leave
function port(name) {
  const p = {
    input: { name, state: "disconnected", open: async () => {}, onmidimessage: null },
    output: { name, state: "disconnected", open: async () => {}, send: (b) => { if (p.output.state !== "connected") throw new Error("gone"); p.rx && p.rx(Array.from(b)); } },
    up(rx) { p.rx = rx; p.input.state = p.output.state = "connected"; },
    down() { p.rx = null; p.input.state = p.output.state = "disconnected"; },
  };
  return p;
}
function midi(ports) {
  return { inputs: new Map(ports.map((p, k) => ["i" + k, p.input])), outputs: new Map(ports.map((p, k) => ["o" + k, p.output])) };
}

// an FM-1: identity on a port; on UPGRADE it asks for one block then the finish address, drops off, and comes back
// as next() says (a port and an identity)
class Dev {
  constructor(p, ident, plan, flash = 0) { this.asked = 0; this.ups = 0; this.plan = plan; this.flash = flash; this.on(p, ident); }
  on(p, ident) {
    this.p = p; this.ident = ident;
    p.up((d) => this.rx(d));
  }
  tx(bytes) { const p = this.p; setTimeout(() => p.input.onmidimessage && p.input.onmidimessage({ data: Uint8Array.from(bytes) }), 1); }
  request(addr, len) {
    this.waiting = addr;
    const u = [0, 0x59, 0x30, 8, 0, 0, 0, addr & 0xFF, (addr >>> 8) & 0xFF, (addr >>> 16) & 0xFF, (addr >>> 24) & 0xFF, len & 0xFF, len >> 8, 0];
    let s = 0;
    for (let i = 6; i < 14; i++) s += u[i];
    this.tx([0xF0, ...pack7([...u, ~s & 0xFF]), 0xF7]);
  }
  rx(d) {
    const j = d.join();
    if (j === HS) {
      this.asked++;
      const t = [...this.ident].map((c) => c.charCodeAt(0)), z = new Array(28 - t.length).fill(0);
      if (this.flash) z.splice(24 - t.length, 3, this.flash >> 16, (this.flash >> 8) & 0xFF, this.flash & 0xFF);   /* felucca-bsp#29 */
      this.tx([0xF0, ...pack7([0, 0x59, 0x11, 0, 0, 0, ...t, ...z]), 0xF7]);
    } else if (j === UP) {
      this.ups++;
      this.queue = [[0, 512], [/^ota-/.test(this.ident) ? 0xF0000000 : 0xE0000000, 8]];
      this.request(...this.queue.shift());
    } else if (this.waiting != null) {
      const addr = this.waiting;
      this.waiting = null;
      if (addr >= 0xE0000000) {
        const next = this.plan.shift();
        setTimeout(() => this.p.down(), 20);
        if (next) setTimeout(() => this.on(next[0], next[1]), 1500);
        return;
      }
      this.request(...this.queue.shift());
    }
  }
}

// 1. a port with an unusual name: not asked, then found once allowed
{
  const a = port("Keyboard A"), b = port("Keyboard B");
  new Dev(a, "FM-1_7010300", []);
  b.up(() => {});                                        // other gear: answers nothing
  const up = new Updater(midi([a, b]));
  check(!(await up.find(null)), "an unusual port name is not asked");
  check(up.ports().join() === "Keyboard A,Keyboard B", "ports() lists every in/out pair");
  const id = await up.probe("Keyboard A");
  check(id && id.text === "FM-1_7010300", "Try: the FM-1 answers on it");
  const dev = await up.find(null);
  check(dev && dev.name === "Keyboard A", "allowed, it is found from then on");
  if (dev) dev.link.close();
}

// 2. an install whose loader and restarted firmware come back under other names
{
  const a = port("Keyboard A"), l = port("USB Audio Device"), c = port("Pad Controller");
  const PRODUCT = "FM-1_7010400";
  new Dev(a, "FM-1_7010300", [[l, "ota-FM-1_1"], [c, PRODUCT]]);
  const up = new Updater(midi([a, l, c]));
  up.allow("Keyboard A");
  const steps = [];
  let got = null, err = null;
  try { got = await up.install(image, PRODUCT, (k) => steps.push(k)); } catch (e) { err = e; }
  check(!err && got === PRODUCT, `followed to the loader and back (${err ? err.code + " " + err.message : got})`);
  check(steps.includes("loader") && steps.includes("done"), `steps: ${[...new Set(steps)].join()}`);
  check(up.baseline === null, "stops watching for new ports after the install");
  const other = await up.find(null);
  check(!other, "the new names are not asked after it");
}

// 3. the maker's Open firmware and its loader
{
  const a = port("FM1 Community");
  new Dev(a, "FM-1_8000014", []);
  const id = await new Updater(midi([a])).probe("FM1 Community");
  check(id && id.open && id.model === "FM-1", "FM-1_8000014 (on \"FM1 Community\") is the Open firmware");
  const b = port("FM1 Community");
  new Dev(b, "ota-FM1C_d017fb65e70aca97", []);
  const ota = await new Updater(midi([b])).find((i) => /^ota-/i.test(i.model));
  check(ota && ota.id.model === "ota-FM1C", "its update loader ota-FM1C_<hex> is found");
  if (ota) ota.link.close();
  const c = port("FM-1");
  new Dev(c, "FM-1_8000400", []);
  const other = await new Updater(midi([c])).probe("FM-1");
  check(other && !other.open, "another firmware FM-1_8000400 is not");
}

// 4. resume: the loader writes, the FM-1 has to come back as the product
for (const [back, want] of [["FM-1_7010400", "done"], ["FM-1_7010300", "mismatch"]]) {
  const l = port("FM-1 Update"), c = port("Pad Controller");
  new Dev(l, "ota-FM-1_1", [[c, back]]);
  let got = null, err = null;
  try { got = await new Updater(midi([l, c])).resume(image, null, "FM-1_7010400"); } catch (e) { err = e; }
  check(want === "done" ? got === true && !err : err && err.code === "mismatch", `resume, back as ${back}: ${want}`);
}

// 5. the package's checksums
{
  let ok = true;
  try { checkUfw(image); } catch (_) { ok = false; }
  const bad = image.slice(); bad[bad.length - 100] ^= 1;
  let caught = false;
  try { checkUfw(bad); } catch (_) { caught = true; }
  check(ok && caught, "checkUfw: the package passes, one flipped bit does not");
}

// ==== m-bootloader (installer_boot_test.mjs) ====
const ldr = ufwEntries(image)[100];
const PRODUCT = "FM-1_900123";

class FakeFM1 {
  constructor({ accept }) {
    this.accept = accept; this.served = 0; this.bad = 0; this.finished = false; this.reboots = 0;
    this.input = { name: "FM-1", state: "connected", open: async () => {}, onmidimessage: null };
    this.output = { name: "FM-1", state: "connected", open: async () => {}, send: (b) => this.rx(Array.from(b)) };
    this.access = { inputs: new Map([["i", this.input]]), outputs: new Map([["o", this.output]]) };
  }
  tx(bytes) { setTimeout(() => this.input.onmidimessage && this.input.onmidimessage({ data: Uint8Array.from(bytes) }), 1); }
  request(addr, len) {
    this.waiting = { addr, len };
    const u = [0, 0x59, 0x30, 8, 0, 0, 0, addr & 0xFF, (addr >>> 8) & 0xFF, (addr >>> 16) & 0xFF, (addr >>> 24) & 0xFF,
      len & 0xFF, (len >> 8) & 0xFF, 0];
    let s = 0;
    for (let i = 6; i < 14; i++) s += u[i];
    this.tx([0xF0, ...pack7([...u, ~s & 0xFF]), 0xF7]);
  }
  rx(d) {
    if (this.output.state !== "connected") throw new Error("disconnected");
    if (d.join() === [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7].join()) {
      const t = [...PRODUCT].map((c) => c.charCodeAt(0)), z = new Array(28 - t.length).fill(0);
      if (this.flash) z.splice(24 - t.length, 3, this.flash >> 16, (this.flash >> 8) & 0xFF, this.flash & 0xFF);
      this.tx([0xF0, ...pack7([0, 0x59, 0x11, 0, 0, 0, ...t, ...z]), 0xF7]);
    } else if (d.join() === [0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7].join()) {
      if (!this.accept) return;                          /* an older firmware: -7, it asks for nothing more */
      this.queue = [[0, 512], [512, 512], [ldr.off, 512], [ldr.off, 512], [ldr.off + 0x20, 512], [0xE0000000, 8]];
      this.request(...this.queue.shift());
    } else if (this.waiting) {
      const u = unpack7(d.slice(1, -1)), { addr, len } = this.waiting;
      const want = addr === 0xE0000000 ? [..."success"].map((c) => c.charCodeAt(0)).concat(0) : Array.from(image.subarray(addr, addr + len));
      if (u.slice(14, 14 + want.length).join() !== want.join()) this.bad++;
      this.waiting = null;
      this.served++;
      if (addr === 0xE0000000) { this.finished = true; this.reset(); return; }
      this.request(...this.queue.shift());
    }
  }
  reset() {                                              /* the installer writes the boot area, then the firmware */
    setTimeout(() => { this.input.state = this.output.state = "disconnected"; }, 50);
    setTimeout(() => { this.input.state = this.output.state = "connected"; this.reboots++; }, 4000);
  }
}

check(productOf(raw) === "FM-1_099" && ldr && ldr.len > 0, "the package: FM-1_099 with an installer");
{
  const dev = new FakeFM1({ accept: true }), steps = [];
  let got = null, err = null;
  try { got = await new Updater(dev.access).installBoot(image, PRODUCT, (k) => steps.push(k)); } catch (e) { err = e; }
  check(!err && got === PRODUCT, `installs, and the firmware comes back (${err ? err.code : got})`);
  check(dev.finished && dev.bad === 0 && dev.served === 6, "every read answered with the package, then 'success'");
  check(steps.join() === "bootstart,bootwrite,bootdone", `steps: ${steps.join()}`);
  check(dev.reboots === 1, "waited for the restart");
}
{
  const dev = new FakeFM1({ accept: true });
  dev.flash = 0xB36014;
  let err = null;
  try { await new Updater(dev.access).installBoot(image, PRODUCT); } catch (e) { err = e; }
  check(err && err.code === "bootflash" && err.detail === "UC25HQ80" && !dev.finished && dev.served === 0,
        "m-bootloader on an FM-1 that says UC25HQ80: bootflash, nothing asked");
}
{
  const dev = new FakeFM1({ accept: false });
  let err = null;
  try { await new Updater(dev.access).installBoot(image, PRODUCT); } catch (e) { err = e; }
  check(err && err.code === "bootrefused" && !dev.finished && dev.served === 0, "a firmware that refuses it: bootrefused, nothing asked");
}

// ---- added: this firmware's port names and its identity's version
{
  const up = new Updater(midi([port("Viral-FM1"), port("InfeKt-FM1"), port("Keyboard A")].map((p) => (p.up(() => {}), p))));
  check(up.askable("Viral-FM1") && up.askable("InfeKt-FM1") && up.askable("INFEKT-FM1 Bluetooth") && !up.askable("Keyboard A"),
        "Viral-FM1 / InfeKt-FM1 asked by their names, other gear not");
  check(PORT_NAMES.includes("viral") && PORT_NAMES.includes("infekt"), "PORT_NAMES has viral and infekt");
  check(versionOf("FM-1_13009500").version === "0.95" && versionOf("FM-1_13009601").version === "0.96.1" &&
        versionOf("FM-1_13010000").version === "1.00" && versionOf("FM-1_1300").family && !versionOf("FM-1_016").family,
        "the identity's version: FM-1_13009500 = 0.95, FM-1_1300 a dev build");
  check(versionOf("FM-1_49011905").version === "1.19.5" && versionOf("FM-1_13011900").version === "1.19" &&
        versionOf("FM-1_13011800").version === "1.18" && versionOf("FM-1_4900").family && versionOf("FM-1_49011905").family &&
        !versionOf("FM-1_4").family && !versionOf("FM-1_490119051").family && !versionOf("FM-1_0049011905").family,
        "1.19.5's prefix FM-1_49 and the former FM-1_13: both this firmware, the version from the six digits alone");
}
// ---- added (1.19.5): an update across the two prefixes: InfeKt 1.19 (FM-1_13011900) to 1.19.5 (FM-1_49011905) and
// back, through the loader, as any other install
for (const [from, to] of [["FM-1_13011900", "FM-1_49011905"], ["FM-1_49011905", "FM-1_13011900"]]) {
  const a = port("InfeKt-FM1"), l = port("FM-1 Update");
  const d = new Dev(a, from, [[l, "ota-FM-1_1"], [a, to]], 0xB36014);
  let got = null, err = null;
  try { got = await new Updater(midi([a, l])).install(image, to, () => {}); } catch (e) { err = e; }
  check(!err && got === to && d.ups > 0, `${from} -> ${to}: installed (${err ? err.code : got})`);
}
// ---- added: other gear that answers the identity request (the maker's USB chip's: an SMK-25 keyboard answers it with its
// own identity), and one that answers with a universal identity reply: never the FM-1, nothing but the request sent to it
const idFrame = (txt) => { const t = [...txt].map((c) => c.charCodeAt(0)); return [0xF0, ...pack7([0, 0x59, 0x11, 0, 0, 0, ...t, ...new Array(28 - t.length).fill(0)]), 0xF7]; };
const UNIVERSAL = [0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x47, 0x31, 0x00, 0x19, 0x00, 0x01, 0x00, 0x00, 0x00, 0xF7];
{
  const fm1 = ["FM-1_016", "FM-1_13010300", "FM-1_1300", "FM-1_13009500", "FM-1_099", "FM-1_8000014", "FM-1_900", "ota-FM-1_015",
               "ota-FM-1_900", "ota-FM-1_1", "ota-FM1C_d017fb65e70aca97", "FM1_OFFICIAL_OPEN", "FM1_COMMUNITY_3"];
  const other = ["SMK25V2_130", "ota-SMK-37 Pro_900", "MKE-P37_900", "ota-MKE-P37_900", "SMK-37_100", "FM-1", "FM-1_13x", "xFM-1_016", "FM-12_016"];
  check(fm1.every((x) => IS_FM1_ID(x) && parseIdentity(idFrame(x)) && !foreignOf(idFrame(x))), "the FM-1's identities: stock, loaders, Open, InfeKt / Viral");
  check(other.every((x) => !IS_FM1_ID(x) && !parseIdentity(idFrame(x)) && foreignOf(idFrame(x)) === x), "other gear's identities: not an FM-1 (SMK25V2_130, ...)");
  check(!parseIdentity(UNIVERSAL) && foreignOf(UNIVERSAL) === "MIDI ID 47", "a universal identity reply: other gear");
}
// ---- added (1.12): the flash chip's JEDEC ID after the text (felucca-bsp#29, bytes 24..26): the identity still the
// FM-1's (the text up to its first zero), the chip read; an unknown chip refused before anything is sent; m-bootloader
// not on the UC25HQ80
const idFlash = (txt, f) => { const a = idFrame(txt), u = unpack7(a.slice(1, -1)).slice(0, 34); u[30] = f >> 16; u[31] = (f >> 8) & 0xFF; u[32] = f & 0xFF; return [0xF0, ...pack7(u), 0xF7]; };
{
  const a = parseIdentity(idFlash("FM-1_13011200", 0xB36014)), b = parseIdentity(idFrame("FM-1_13011200"));
  check(a && a.text === "FM-1_13011200" && a.version === 13011200 && a.flash === 0xB36014 && !foreignOf(idFlash("FM-1_13011200", 0xB36014)),
        "an identity with the flash ID after it: the FM-1, the UC25HQ80");
  check(b && b.flash === 0 && FLASH_PARTS[0x856014] && FLASH_PARTS[0xB36014] && !FLASH_PARTS[0xEF4016], "no flash ID: 0; the known chips");
}
for (const [f, want] of [[0xEF4016, "flash"], [0xB36014, null]]) {
  const a = port("FM-1"), l = port("FM-1 Update");
  const PRODUCT = "FM-1_7010400";
  const d = new Dev(a, "FM-1_7010300", [[l, "ota-FM-1_1"], [a, PRODUCT]], f);
  const up = new Updater(midi([a, l]));
  let got = null, err = null;
  try { got = await up.install(image, PRODUCT, () => {}); } catch (e) { err = e; }
  check(want ? err && err.code === want && err.detail === "EF4016" && d.ups === 0 : !err && got === PRODUCT,
        want ? "an unknown flash chip: refused, nothing but the request sent" : `the UC25HQ80: installs (${err ? err.code : got})`);
}
// ---- added (1.14): InfeKt 1.11 and older on the UC25HQ80 never ask for the package (their update session ran on the
// Puya only): 0 requests from such an identity -> oldflash (the version as the detail); from 1.12 on, or after some
// requests, the usual "stopped"
check(OLD_GATE("FM-1_13011000") && OLD_GATE("FM-1_13011100") && OLD_GATE("FM-1_13009900") && !OLD_GATE("FM-1_13011200") &&
      !OLD_GATE("FM-1_13011300") && !OLD_GATE("FM-1_13011400") && !OLD_GATE("FM-1_016") && !OLD_GATE("FM-1_900123"),
      "OLD_GATE: InfeKt 0.99, 1.10, 1.11 yes; 1.12+, stock, fm1-base no");
for (const [ident, want, det] of [["FM-1_13011000", "oldflash", "1.10"], ["FM-1_13011200", "stopped", ""]]) {
  const a = port("InfeKt-FM1");
  let ups = 0;
  a.up((d) => {
    if (d.join() === HS) {
      const t = [...ident].map((c) => c.charCodeAt(0));
      setTimeout(() => a.input.onmidimessage && a.input.onmidimessage({ data: Uint8Array.from([0xF0, ...pack7([0, 0x59, 0x11, 0, 0, 0, ...t, ...new Array(28 - t.length).fill(0)]), 0xF7]) }), 1);
    } else if (d.join() === UP) ups++;                      // (the update request: nothing asked back)
  });
  let err = null;
  try { await new Updater(midi([a])).install(image, "FM-1_13011400", () => {}); } catch (e) { err = e; }
  check(err && err.code === want && (err.detail || "") === det && ups === 1,
        `${ident} that asks nothing after the request: ${want}${det ? " (" + det + ")" : ""} (${err ? err.code : "no error"})`);
}
class Kbd {                                              // other gear: answers the identity request, takes everything
  constructor(p, reply) { this.got = []; p.up((d) => { this.got.push(d.join()); if (d.join() === HS) setTimeout(() => p.input.onmidimessage && p.input.onmidimessage({ data: Uint8Array.from(reply) }), 1); }); }
  get onlyAsked() { return this.got.every((j) => j === HS); }
}
{
  const k = port("SINCO SMK25-Private"), u = port("USB-MIDI Keyboard");
  const kb = new Kbd(k, idFrame("SMK25V2_130")), ub = new Kbd(u, UNIVERSAL);
  const up = new Updater(midi([k, u]));
  check(!(await up.find(null)) && up.foreign.get("SINCO SMK25-Private") === "SMK25V2_130" && up.foreign.get("USB-MIDI Keyboard") === "MIDI ID 47",
        "only keyboards that answer: no FM-1, their answers named");
  check(!(await up.probe("SINCO SMK25-Private")), "Try on the keyboard: not an FM-1");
  let err = null;
  try { await up.install(image, "FM-1_7010400", () => {}); } catch (e) { err = e; }
  check(err && err.code === "notfound" && kb.onlyAsked && ub.onlyAsked, "install with only keyboards: notfound, nothing but the request sent");
}
for (const prefer of [true, false]) {
  // the keyboards first in the map, the FM-1 chosen by hand (the verified pair) or found automatically; across the update:
  // a keyboard in its own update mode ("ota-SMK-37 Pro_900") is there too and is not taken as the FM-1's loader
  const k = port("SINCO SMK25-Private"), u = port("USB-MIDI Keyboard"), ko = port("SINCO Update");
  const a = port("InfeKt-FM1 MIDI 1"), l = port("FM-1 Update");
  const gear = [new Kbd(k, idFrame("SMK25V2_130")), new Kbd(u, UNIVERSAL), new Kbd(ko, idFrame("ota-SMK-37 Pro_900"))];
  const PRODUCT = "FM-1_7010400";
  new Dev(a, "FM-1_7010300", [[l, "ota-FM-1_1"], [a, PRODUCT]]);
  const up = new Updater(midi([k, u, ko, a, l]));
  if (prefer) { up.allow("InfeKt-FM1 MIDI 1"); up.prefer = { input: a.input, output: a.output }; }
  let got = null, err = null;
  try { got = await up.install(image, PRODUCT, () => {}); } catch (e) { err = e; }
  check(!err && got === PRODUCT && gear.every((g) => g.onlyAsked),
        `install ${prefer ? "to the chosen FM-1" : "automatic"} beside answering keyboards, across the loader (${err ? err.code + " " + err.message : got})`);
}
console.log(fails ? "INSTALLER TEST FAILED" : "installer test passed");
process.exit(fails ? 1 : 0);
