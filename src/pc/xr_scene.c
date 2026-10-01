/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc/xr_scene.h"

#ifdef AURORA_ENABLE_OPENXR

#include "pc/pc.h"

#include <aurora/xr.h>

#include <stdlib.h>
#include <string.h>

/* gm/gmscene.h; its headers need the game's platform prelude. */
bool gm_GetDbPauseFlag(int bit);

/* Stage parts shown in 3D regardless of layer, or hidden regardless of it.
 * Keyed by GrKind (gr/forward.h) and the part's map_id (its index in the
 * stage's part table). Extend with MELEE_XR_PARTS while surveying stages:
 * "16:1,-16:4" shows part 1 and hides part 4 of stage 16. */
typedef struct {
    int grkind;
    int map_id;
    bool show;
} PartRule;

#define GRKIND_PSTADIUM 0x10 /* Gr_Kind_PStadium */
#define PSTYPE_DISPLAY 1     /* PsType_Display: the stadium's big screen */

static const PartRule s_builtin_rules[] = {
    {GRKIND_PSTADIUM, PSTYPE_DISPLAY, true},
};

#define MAX_RULES 64
static PartRule s_rules[MAX_RULES];
static int s_rule_count = -1;
/* Stage passes draw layers 2, 1, 0, 3 in that order. Layer 2 is the far
 * background (Final Destination: the space backdrop, parts 4-8); the stage
 * itself sits on layers 1 and 0 (FD: parts 1, 3 and 0, 2). Show everything
 * but layer 2 unless a rule says otherwise. */
static unsigned s_layer_mask = 0xB;
static bool s_log_parts;
static int s_category = AURORA_XR_MONO;

static void load_rules(void) {
    s_rule_count = 0;
    for (size_t i = 0; i < sizeof s_builtin_rules / sizeof s_builtin_rules[0]; i++) {
        s_rules[s_rule_count++] = s_builtin_rules[i];
    }
    const char* mask = getenv("MELEE_XR_STAGE_LAYERS");
    if (mask != NULL && mask[0] != '\0') {
        s_layer_mask = (unsigned)strtoul(mask, NULL, 0);
    }
    s_log_parts = getenv("MELEE_XR_STAGE_LOG") != NULL;
    const char* spec = getenv("MELEE_XR_PARTS");
    while (spec != NULL && *spec != '\0' && s_rule_count < MAX_RULES) {
        bool show = true;
        if (*spec == '-' || *spec == '+') {
            show = *spec == '+';
            spec++;
        }
        char* end = NULL;
        const long kind = strtol(spec, &end, 0);
        if (end == spec || *end != ':') {
            break;
        }
        spec = end + 1;
        const long id = strtol(spec, &end, 0);
        if (end == spec) {
            break;
        }
        s_rules[s_rule_count++] = (PartRule){(int)kind, (int)id, show};
        spec = *end == ',' ? end + 1 : end;
    }
}

static bool part_visible(int grkind, int map_id, int layer) {
    if (s_rule_count < 0) {
        load_rules();
    }
    bool visible = layer >= 0 && layer < 8 && (s_layer_mask & (1u << layer)) != 0;
    for (int i = 0; i < s_rule_count; i++) {
        if (s_rules[i].grkind == grkind && s_rules[i].map_id == map_id) {
            visible = s_rules[i].show; /* later rules (env) win */
        }
    }
    if (s_log_parts) {
        /* Once per (stage, part): enough to map a stage's layers. */
        static unsigned seen[64][4];
        const unsigned bucket = (unsigned)grkind & 63u, bit = (unsigned)map_id & 127u;
        if (!(seen[bucket][bit / 32] & (1u << (bit % 32)))) {
            seen[bucket][bit / 32] |= 1u << (bit % 32);
            pc_log_line("xr: stage %d part %d layer %d -> %s", grkind, map_id, layer,
                visible ? "3D" : "hidden");
        }
    }
    return visible;
}

void pc_xr_world_camera(const float view[3][4]) {
    /* Pause flags 1 and 2: a player paused the match (gm_DoPauseChecksAndRoutine). */
    aurora_xr_set_paused(gm_GetDbPauseFlag(1) || gm_GetDbPauseFlag(2));
    s_category = AURORA_XR_WORLD;
    aurora_xr_camera(AURORA_XR_WORLD, view);
}

void pc_xr_hud_camera(void) {
    s_category = AURORA_XR_HUD;
    aurora_xr_camera(AURORA_XR_HUD, NULL);
}

void pc_xr_mono_camera(void) {
    s_category = AURORA_XR_MONO;
    aurora_xr_camera(AURORA_XR_MONO, NULL);
}

bool pc_xr_stage_part_begin(int grkind, int map_id, int layer) {
    if (s_category != AURORA_XR_WORLD || part_visible(grkind, map_id, layer)) {
        return false;
    }
    /* Still drawn into the flat frame (the stadium screen's feed copies it),
     * just not re-drawn in 3D. */
    aurora_xr_camera(AURORA_XR_MONO, NULL);
    return true;
}

void pc_xr_stage_part_end(bool hidden) {
    if (hidden) {
        aurora_xr_camera(AURORA_XR_WORLD, NULL);
    }
}

#endif
