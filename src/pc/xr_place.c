/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "compat.h"
#include "pc/xr_place.h"

#ifdef AURORA_ENABLE_OPENXR

#include "pc/net.h"
#include "pc/pc.h"

#include <SDL3/SDL.h>
#include <aurora/xr.h>
#include <dolphin/pad.h>
#include <sysdolphin/baselib/controller.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PNG only, and private to this file: thp_jpeg.cpp has its own JPEG-only
 * copy. */
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "pc/stb_image.h"

/* Stages (grkind) placed this session. */
static uint8_t s_placed[32];
/* The stage to offer (-1: none), the passes left to offer it in, and the
 * one held (-1: none). */
static int s_pending = -1;
static int s_pending_passes;
static int s_holding = -1;
/* The how-to cards: showing now, and shown once this session already. */
static bool s_cards;
static bool s_cards_seen;
/* Buttons down in the newest pad sample last pass, and the ones that went
 * down during the hold (a release only counts for a press seen here). */
static uint32_t s_prev;
static uint32_t s_down;

static bool placed(int grkind) {
    return grkind >= 0 && grkind < 256 && (s_placed[grkind / 8] & (1u << (grkind % 8))) != 0;
}

static void mark_placed(int grkind) {
    if (grkind >= 0 && grkind < 256) {
        s_placed[grkind / 8] |= (uint8_t)(1u << (grkind % 8));
    }
}

static bool enabled(void) {
    const char* v = getenv("MELEE_XR_PLACE");
    return v == NULL || *v != '0';
}

/* resources/xr/<name>: next to the game, or in the APK's assets. */
static void load_image(int which, const char* name) {
    char path[512];
    snprintf(path, sizeof path, "resources/xr/%s", name);
    size_t size = 0;
    void* data = SDL_LoadFile(path, &size);
    if (data == NULL) {
        const char* base = SDL_GetBasePath();
        if (base != NULL) {
            snprintf(path, sizeof path, "%sresources/xr/%s", base, name);
            data = SDL_LoadFile(path, &size);
        }
    }
    if (data == NULL) {
        pc_log_line("xr: placing picture %s not found", name);
        return;
    }
    int w = 0, h = 0, n = 0;
    unsigned char* rgba = stbi_load_from_memory(data, (int)size, &w, &h, &n, 4);
    SDL_free(data);
    if (rgba == NULL) {
        pc_log_line("xr: placing picture %s unreadable", name);
        return;
    }
    aurora_xr_set_placing_image(which, w, h, rgba);
    stbi_image_free(rgba);
}

/* The buttons held on any connected port in the newest queued pad sample. */
static uint32_t newest_buttons(void) {
    const PadLibData* p = &HSD_PadLibData;
    if (p->qcount == 0 || p->queue == NULL) {
        return s_prev;
    }
    const int newest = p->qwrite != 0 ? p->qwrite - 1 : p->qnum - 1;
    uint32_t buttons = 0;
    for (int port = 0; port < 4; port++) {
        const PADStatus* s = &p->queue[newest].stat[port];
        if (s->err == 0) {
            buttons |= s->button;
        }
    }
    return buttons;
}

static void end_hold(void) {
    aurora_xr_set_placing(0);
    mark_placed(s_holding);
    s_holding = -1;
}

void pc_xr_place_stage_ready(int grkind) {
    if (s_holding < 0 && !placed(grkind)) {
        s_pending = grkind;
        /* A session still starting gets a moment, but a hold never comes
         * later than the fight's opening (Ready... GO). */
        s_pending_passes = 90;
    }
}

bool pc_xr_place_hold(void) {
    if (s_holding < 0) {
        if (s_pending < 0) {
            return false;
        }
        const int grkind = s_pending;
        /* Netplay: the peer starts the match on the agreed frame whatever
         * this side shows, so the stage keeps its default placement. */
        if (!enabled() || pc_net_active() || placed(grkind)) {
            s_pending = -1;
            return false;
        }
        if (!aurora_xr_active()) {
            if (--s_pending_passes <= 0) {
                s_pending = -1;
            }
            return false;
        }
        s_pending = -1;
        static bool loaded;
        if (!loaded) {
            loaded = true;
            load_image(0, "place-cards.png");
            load_image(1, "place-legend.png");
        }
        s_holding = grkind;
        s_cards = !s_cards_seen;
        s_cards_seen = true;
        s_prev = newest_buttons(); /* held coming in: not a press */
        s_down = 0;
        aurora_xr_set_placing(s_cards ? 2 : 1);
        pc_log_line("xr: holding stage %d before the fight for placing", grkind);
    }

    const uint32_t buttons = newest_buttons();
    const uint32_t released = s_prev & ~buttons;
    s_down |= buttons & ~s_prev;
    s_prev = buttons;
    const uint32_t done = released & s_down;
    s_down &= ~released;
    /* Nothing that came in while held reaches the game. */
    HSD_PadFlushQueue(HSD_PAD_FLUSH_QUEUE_THROWAWAY);

    /* A headset taken off (session not running) keeps the hold: the fight
     * shouldn't start unseen. A or Start on any pad still ends it. */
    if (done & PAD_BUTTON_Y) {
        s_cards = !s_cards;
        aurora_xr_set_placing(s_cards ? 2 : 1);
    }
    if (done & PAD_BUTTON_B) {
        aurora_xr_reset_stage();
        pc_log_line("xr: stage %d back at its default placement", s_holding);
    }
    if (done & (PAD_BUTTON_A | PAD_BUTTON_START)) {
        pc_log_line("xr: stage %d placed; the fight begins", s_holding);
        end_hold();
    }
    return true;
}

#endif
