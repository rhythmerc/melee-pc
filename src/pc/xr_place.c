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

/* PNG (the cards) and JPEG (the clips), private to this file:
 * thp_jpeg.cpp has its own JPEG-only copy. */
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "pc/stb_image.h"

/* The stage to offer next pass (-1: none) and the one held (-1: none). */
static int s_pending = -1;
static int s_holding = -1;
/* The how-to cards are showing (they open with every hold; Y toggles). */
static bool s_cards;
/* Buttons down in the newest pad sample last pass, and the ones that went
 * down during the hold (a release only counts for a press seen here). */
static uint32_t s_prev;
static uint32_t s_down;

static bool enabled(void) {
    const char* v = getenv("MELEE_XR_PLACE");
    return v == NULL || *v != '0';
}

/* resources/xr/<name>: next to the game, or in the APK's assets. SDL_free
 * it; NULL (logged) when it isn't there. */
static void* load_file(const char* name, size_t* size) {
    char path[512];
    snprintf(path, sizeof path, "resources/xr/%s", name);
    void* data = SDL_LoadFile(path, size);
    if (data == NULL) {
        const char* base = SDL_GetBasePath();
        if (base != NULL) {
            snprintf(path, sizeof path, "%sresources/xr/%s", base, name);
            data = SDL_LoadFile(path, size);
        }
    }
    if (data == NULL) {
        pc_log_line("xr: placing picture %s not found", name);
    }
    return data;
}

/* RGBA8; stbi_image_free it. */
static unsigned char* load_rgba(const char* name, int* w, int* h) {
    size_t size = 0;
    void* data = load_file(name, &size);
    if (data == NULL) {
        return NULL;
    }
    int n = 0;
    unsigned char* rgba = stbi_load_from_memory(data, (int)size, w, h, &n, 4);
    SDL_free(data);
    if (rgba == NULL) {
        pc_log_line("xr: placing picture %s unreadable", name);
    }
    return rgba;
}

static void load_image(int which, const char* name) {
    int w = 0, h = 0;
    unsigned char* rgba = load_rgba(name, &w, &h);
    if (rgba != NULL) {
        aurora_xr_set_placing_image(which, w, h, rgba);
        stbi_image_free(rgba);
    }
}

/* place-clips.txt: one clip a line, "set name frames cols frame_w frame_h
 * fps x y w h" (tools/xr_cards/make_cards.py), set being controllers or
 * hands, whose clips go in slots 0-2 and 3-5 in the order move, turn,
 * scale; its atlas is place-clip-<set>-<name>.jpg. */
static void load_clips(void) {
    size_t size = 0;
    char* text = load_file("place-clips.txt", &size);
    if (text == NULL) {
        return;
    }
    char* copy = malloc(size + 1);
    memcpy(copy, text, size);
    copy[size] = '\0';
    SDL_free(text);
    static const char* const names[3] = {"move", "turn", "scale"};
    for (char* line = strtok(copy, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        char set[16], name[32];
        int frames, cols, fw, fh, x, y, w, h;
        float fps;
        if (sscanf(line, "%15s %31s %d %d %d %d %f %d %d %d %d", set, name, &frames, &cols, &fw, &fh, &fps, &x, &y,
                   &w, &h) != 11) {
            continue;
        }
        int which = -1;
        for (int i = 0; i < 3; i++) {
            if (strcmp(name, names[i]) == 0) {
                which = (strcmp(set, "hands") == 0 ? 3 : 0) + i;
            }
        }
        if (which < 0) {
            continue;
        }
        char file[80];
        snprintf(file, sizeof file, "place-clip-%s-%s.jpg", set, name);
        int aw = 0, ah = 0;
        unsigned char* rgba = load_rgba(file, &aw, &ah);
        if (rgba != NULL) {
            aurora_xr_set_placing_clip(which, aw, ah, rgba, frames, cols, fw, fh, fps, x, y, w, h);
            stbi_image_free(rgba);
        }
    }
    free(copy);
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
    s_holding = -1;
}

void pc_xr_place_stage_ready(int grkind) {
    if (s_holding < 0) {
        s_pending = grkind;
    }
}

bool pc_xr_place_hold(void) {
    if (s_holding < 0) {
        if (s_pending < 0) {
            return false;
        }
        const int grkind = s_pending;
        s_pending = -1;
        /* Only with a headset already presenting: an XR build playing flat
         * (no runtime, no session yet) never waits for one. Netplay: the
         * peer starts the match on the agreed frame whatever this side
         * shows, so the stage keeps its default placement. */
        if (!enabled() || !aurora_xr_active() || pc_net_active()) {
            return false;
        }
        static bool loaded;
        if (!loaded) {
            loaded = true;
            load_image(0, "place-cards.png");
            load_image(1, "place-legend.png");
            load_image(2, "place-cards-hands.png");
            load_image(3, "place-legend-hands.png");
            load_clips();
        }
        s_holding = grkind;
        s_cards = true;
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
