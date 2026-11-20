/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 J0KERPACK */
/* The development engine on fm1-base: the app's half of the contract with the platform (fm1-base device/unity.c). */
#pragma once
#include <stdint.h>
#include "plat.h"

#define HYB_VERSION "1.19.5"            /* the firmware's version, shown in GLO > ABOUT: the one place to change it (the
                                          * release package script reads it from here for build_multi_ble.sh --release,
                                          * the page's --version and the file names). 1.19: the first public
                                          * release, 1.18C49's firmware with the version and ABOUT's strings changed.
                                          * 1.19.1: GLO's order, B CHANNEL under MIDI.
                                          * 1.19.3: SEQ B in SINGLE (MIDI only), DRUM OUT.
                                          * 1.19.4: A / B CHANNEL's IN and OUT, CLK OFFSET.
                                          * 1.19.5: CLK LOCK, phase lock to an external clock.
                                          * Before it the cleaner's test builds "1.18Cnn" ("C": the cleaner build of 1.18;
                                          * C3: the own reverb and shimmer; a build with the own reverb in the development engine's
                                          * place shows an "R" after it, app.c HYB_RVTAG) */
#define APP_NAME "INFEKT-FM1"            /* the product name (the splash, GLO > ABOUT; a former name until 0.97) */
#define APP_SPLASH                       /* the boot image (splash.c, 0.99): drawn if one is stored, else the name: */
#define APP_SPLASH_RGB RGB(232, 28, 20)  /* red on the image's dark background */
#define APP_SPLASH_BG RGB(14, 8, 10)
int app_splash(void);
#define APP_BLOCK 128u                   /* frames per audio block = the engine's block (2.9 ms at 44.1 kHz) */
#define FM1B_UAC_TAP_PLAY 1              /* (after 1.05) the USB audio input fed as the I2S plays each block (fm1-base
                                          * main_fm1.c uac_tap_play), not when its render ends: the renders' load (often
                                          * past the ~40 % the ring's band allows at the render) no longer runs it dry,
                                          * the recording's clicks */
#define FM1B_UAC_PAT 1                   /* (1.18C35) the USB audio input's packets 44 or 45 frames only (fm1-base usb.c
                                          * uac_packet: never 43 or 46, the sizes a 44.1 kHz clock gives; a host that
                                          * measures the rate from them, an iPhone, sees no steps) */
#define APP_CANVAS_PX (232u * 16u)      /* the largest canvas drawn (a row of knobs and the LCD strip in bands of 16: ui.c
                                          * UI_BAND; the profile line's 16 rows): pool */
#define APP_TOUCH                        /* velocity from the keys (lib/touch.h) */
#define APP_BTDEV                        /* Bluetooth MIDI devices (lib/btdev.h) */
#define APP_SYSEX                        /* single dumps and the bank upload page's handshake over USB, the user waves'
                                          * messages over USB and Bluetooth (app.c app_sysex, waves.c) */
#define APP_BLE_AT_BOOT (!hybrid_ble_off())   /* Bluetooth at power-on: as the player left it (REC held) */
int hybrid_ble_off(void);

/* the store: the settings, the panel calibration (the platform's), Bluetooth's address and pairing; until 1.18C29 the
 * user banks C, D, E (app.c ub_*): 9 objects a bank, 15 patches of 256 bytes an object (the ninth holds 8), 27 objects
 * of two 4 KiB sectors each from 0xA6000 up to the panel's at 0xDC000 (0xA6000..0xDBFFF, 216 KiB). Since 1.18C30 the
 * six user banks C .. H are the app's own bank store in 0xA5000..0xDBFFF (bankstore.c: 55
 * sectors, 54 logical ones and a spare), which reads the objects OBJ_UB0 .. OBJ_UB0 + 26 as its first 27 sectors where
 * they are: the platform no longer reads or writes them (their entries below stay, the numbers keep their meaning in
 * the sectors' headers); the arp sequencer's patterns
 * (steps 1-16) at 0x97000, the drum sequencer's (block 1) and the kit at 0x99000, the rest of the arp sequencer's
 * (steps 17-64) at 0x9B000 and 0x9D000, of the drum sequencer's (blocks 2-4) at 0x9F000, 0xA1000, 0xA3000
 * (0xA5000..0xA5FFF was free; the bank store's since 1.18C30). The pattern launcher's sets (1.18C27, OBJ_LSET) in the sector pair at 0xE5000 (felucca-bsp's
 * "autosave" pair, FL_AUTO_LO..FL_AUTO_HI, a store region of its flash policy; unused by this firmware before): the
 * update loader looks for a record in bytes 0xF00..0xF07 of each sector from 0xE0000 up, so the object stays shorter
 * than 3584 bytes (its payload from byte 256: those bytes never written, app.c LS_MAXB). The numbers and offsets of the objects never change (an object's number is in its
 * record): new ones go at the end */
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_BLE, OBJ_UB0, OBJ_SEQ = OBJ_UB0 + 27, OBJ_DRUM, OBJ_SEQX, OBJ_DRUMX = OBJ_SEQX + 2,
       OBJ_LSET = OBJ_DRUMX + 3, OBJ_NOBJ };   /* (OBJ_SEQ: the arp sequencer's 32 patterns' heads, app.c sq_*;
                                          * OBJ_DRUM: the drum sequencer's 16, dr_*, and after them the drum kit, kit_*;
                                          * OBJ_SEQX, OBJ_DRUMX: the rest of their steps, 16 and 6 patterns an object;
                                          * OBJ_LSET (1.18C27): the pattern launcher's sets, app.c ls_*) */
#define APP_STORE_MAP {0xFC000u, 0xDC000u, 0xDE000u,                                         \
    /* C */ 0xA6000u, 0xA8000u, 0xAA000u, 0xAC000u, 0xAE000u, 0xB0000u, 0xB2000u, 0xB4000u, 0xB6000u,  \
    /* D */ 0xB8000u, 0xBA000u, 0xBC000u, 0xBE000u, 0xC0000u, 0xC2000u, 0xC4000u, 0xC6000u, 0xC8000u,  \
    /* E */ 0xCA000u, 0xCC000u, 0xCE000u, 0xD0000u, 0xD2000u, 0xD4000u, 0xD6000u, 0xD8000u, 0xDA000u,  \
    /* the sequencer's patterns */ 0x97000u, /* the drum sequencer's */ 0x99000u,                       \
    /* the rest of their steps */ 0x9B000u, 0x9D000u, 0x9F000u, 0xA1000u, 0xA3000u,   \
    /* the launcher's sets */ 0xE5000u}

/* the contract: the platform calls these */
void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);   /* audio interrupt: n stereo frames, 24-bit */
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);
int app_sysex(const uint8_t *p, uint32_t n);   /* main loop: a SysEx frame from USB (the bytes between F0 and F7); 1 = ours */
