/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "compat.h"
#include "pc/xr_scene.h"

#ifdef AURORA_ENABLE_OPENXR

#include "pc/pc.h"
#include "pc/xr_place.h"

#include <aurora/xr.h>
#include <dolphin/pad.h>
#include <SDL3/SDL_gamepad.h>
#include <melee/gm/gmscene.h>
#include <melee/gr/ground.h>
#include <melee/gr/stage.h>
#include <melee/mp/forward.h>
#include <melee/mp/mplib.h>
#include <melee/mp/types.h>
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/jobj.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Stage parts shown in 3D regardless of layer, or hidden regardless of it.
 * Keyed by GrKind (gr/forward.h) and the part's map_id (its index in the
 * stage's part table). A rule can name one joint of the part instead, by
 * its index in a depth-first walk of the part's joint tree; it then applies
 * to that joint and everything under it, or to one of its meshes (DObjs,
 * counted from 0). Later rules win. Extend with MELEE_XR_PARTS while
 * surveying stages: "16:1,-16:4,-16:1/7,-16:2/2.1" shows part 1, hides part
 * 4, hides joint 7 of part 1, and hides mesh 1 of joint 2 of part 2, all of
 * stage 16. MELEE_XR_JOINT_LOG lists every part's joints once. */
typedef struct {
    int grkind;
    int map_id;
    int jobj; /* -1: the whole part */
    int dobj; /* -1: the whole joint */
    bool show;
} PartRule;

#define GRKIND_PSTADIUM 0x10 /* Gr_Kind_PStadium */
#define PSTYPE_DISPLAY 1     /* PsType_Display: the stadium's big screen */

static const PartRule s_builtin_rules[] = {
    {GRKIND_PSTADIUM, PSTYPE_DISPLAY, -1, -1, true},
    /* The big screen's part also carries the city: joint 11 is the 3D
     * buildings, joint 12 the sky, mountains and city lights. */
    {GRKIND_PSTADIUM, PSTYPE_DISPLAY, 11, -1, false},
    {GRKIND_PSTADIUM, PSTYPE_DISPLAY, 12, -1, false},
    /* Part 2 joint 2 is the stadium bowl: stands, walls, entrance tunnels,
     * a dark floor that read as a black void under the arena, and the
     * light rings beside the big screen. */
    {GRKIND_PSTADIUM, 2, 2, -1, false},
    /* Part 2 joint 3 is the stage body: meshes 28-31 are the column under
     * the platform, which reached down to the old floor. */
    {GRKIND_PSTADIUM, 2, 3, 28, false},
    {GRKIND_PSTADIUM, 2, 3, 29, false},
    {GRKIND_PSTADIUM, 2, 3, 30, false},
    {GRKIND_PSTADIUM, 2, 3, 31, false},

    /* Survey of 2026-10-01 (desktop dumps; layer 2 is already out). Only the
     * obvious stage/background splits; the rest are listed in
     * docs/quest-xr.md. */
    /* Fountain of Dreams: the vortex, pink sea and rings. */
    {0x0C, 1, -1, -1, false},
    /* Kongo Jungle: the jungle and cliffs around the waterfall. */
    {0x04, 6, -1, -1, false},
    /* Kongo Jungle: the flat river behind the waterfall (joints 7 and 9) and
     * the surface layers over it, leaving the waterfall at the front. */
    {0x04, 4, 5, -1, false},
    {0x04, 4, 7, -1, false},
    {0x04, 4, 9, -1, false},
    {0x04, 4, 10, -1, false},
    {0x04, 4, 26, -1, false},
    {0x04, 4, 35, -1, false},
    /* Corneria: the terrain under the Great Fox, and the coastline that
     * scrolls past about a minute in. */
    {0x0E, 4, -1, -1, false},
    {0x0E, 8, -1, -1, false},
    {0x0E, 9, -1, -1, false},
    /* Yoshi's Story: the cardboard sea and hills (part 3 joints 13-21). */
    {0x0A, 3, 13, -1, false},
    {0x0A, 3, 14, -1, false},
    {0x0A, 3, 15, -1, false},
    {0x0A, 3, 16, -1, false},
    {0x0A, 3, 17, -1, false},
    {0x0A, 3, 18, -1, false},
    {0x0A, 3, 19, -1, false},
    {0x0A, 3, 20, -1, false},
    {0x0A, 3, 21, -1, false},
    /* Great Bay: land, sky and mountains. The sea (part 4) stays: it hides
     * the turtle's and the stilts' underwater parts. */
    {0x06, 3, -1, -1, false},
    /* Peach's Castle: everything around the castle is in part 3 with it.
     * Hidden: the red grounds (joint 8's own meshes), the bridge and stairs
     * (13), the hills, path and fence (25), the warp medallion (26), and
     * the trees and hedge (28's children), keeping the roof flags (65, 74,
     * 83, 92). The castle body stays whole. */
    {0x02, 3, 8, 0, false},
    {0x02, 3, 8, 1, false},
    {0x02, 3, 8, 2, false},
    {0x02, 3, 13, -1, false},
    {0x02, 3, 25, -1, false},
    {0x02, 3, 26, -1, false},
    {0x02, 3, 28, -1, false},
    {0x02, 3, 65, -1, true},
    {0x02, 3, 74, -1, true},
    {0x02, 3, 83, -1, true},
    {0x02, 3, 92, -1, true},
    /* Jungle Japes: the jungle, sky and moon (part 4) and the birds far
     * behind (part 5) go; the river (part 6, on the far layer) comes back,
     * bounded to the stage below. */
    {0x05, 4, -1, -1, false},
    {0x05, 5, -1, -1, false},
    {0x05, 6, -1, -1, true},
    /* Venom: the canyon walls and floor around the Great Fox. */
    {0x0F, 7, -1, -1, false},
    /* Kongo Jungle 64: the jungle (part 3 joint 7) and the sky (joint 34). */
    {0x1E, 3, 7, -1, false},
    {0x1E, 3, 34, -1, false},
    /* Yoshi's Island: the sky (part 1 joint 28). The clouds far behind are
     * pulled in (s_moves). The tall slanted rock at the right is still
     * shown: it is mesh 30 of joint 10 with more of it that mesh rules
     * don't reach. */
    {0x0B, 1, 28, -1, false},
    /* Brinstar: the cave, its walls, stalactites, pillars and chains. */
    {0x08, 1, -1, -1, false},
    /* Onett: everything is in part 5. Hidden: the town behind (joint 14) and
     * the hills (23); the rest is boxed in by clip planes (s_clips). */
    {0x14, 5, 14, -1, false},
    {0x14, 5, 23, -1, false},
    /* Fourside: the city (part 6 joint 2) around the three buildings you
     * fight on (joints 11, 24 and 31 of the same part). */
    {0x15, 6, 2, -1, false},
    /* Mushroom Kingdom: everything is in part 3. Hidden: the sky and hills
     * (joint 46). The mushroom poles and clouds just behind (32) stay; the
     * ground's deep blocks are cut (s_clips). */
    {0x18, 3, 46, -1, false},
};

/* Joints moved in the 3D view only: the joint and everything under it are
 * scaled by `scale` about `pivot` (game units) and the pivot is put at
 * `to`. */
typedef struct {
    int grkind;
    int map_id;
    int jobj;
    float pivot[3];
    float to[3];
    float scale;
} MoveRule;

/* Pokémon Stadium's big screen stands far behind the stage (its frame,
 * joint 2, is based at z -215). Pull it in to just behind the stage and
 * shrink it. Moved at joint 1, the parent of everything the screen shows:
 * the frame (2), the picture (3), and the overlays the game unhides during
 * a transformation (4-10). MELEE_XR_JUMBOTRON="scale,x,y,z" overrides. */
static MoveRule s_moves[16] = {
    {GRKIND_PSTADIUM, PSTYPE_DISPLAY, 1, {0, -30, -215}, {0, -10, -75}, 0.55f},
    /* Great Bay: the sea, shrunk to the stage's footprint and moved under
     * it (it only extended toward the player). */
    {0x06, 4, 2, {0, 0, 0}, {10, 0, -80}, 0.56f},
    /* Yoshi's Island: the clouds (part 1 joints 20-23, 25-27, 29, with the
     * Bullet Bill and the sign), from hundreds of units behind and to the
     * sides to just behind the stage, a little above the back pillars'
     * trees: shrunk to 0.3 about the middle of the cloud field. */
#define YI_CLOUDS(j) {0x0B, 1, j, {100, 90, -350}, {0, 70, -150}, 0.3f}
    YI_CLOUDS(20), YI_CLOUDS(21), YI_CLOUDS(22), YI_CLOUDS(23),
    YI_CLOUDS(25), YI_CLOUDS(26), YI_CLOUDS(27), YI_CLOUDS(29),
#undef YI_CLOUDS
};
static int s_move_count = -1; /* built-ins, then MELEE_XR_MOVE entries */
#define MOVE_COUNT s_move_count

/* Stage parts cut below a height in the 3D view only (game units; the stage
 * stays whole in the flat frame). Fighters and items are never clipped. */
typedef struct {
    int grkind;
    int map_id;
    float plane[4]; /* kept where a*x + b*y + c*z + d >= 0 (game world) */
    float fade;     /* > 0: dissolve across this many units (soft clip) */
} ClipRule;

/* Cut below y / behind z (toward -z, away from the player) / in front of z
 * / left of x / right of x. Up to four rules per part combine. */
#define CLIP_BELOW(gk, part, y, fade) {gk, part, {0.f, 1.f, 0.f, -(y)}, fade}
#define CLIP_BEHIND(gk, part, z, fade) {gk, part, {0.f, 0.f, 1.f, -(z)}, fade}
#define CLIP_FRONT(gk, part, z, fade) {gk, part, {0.f, 0.f, -1.f, (z)}, fade}
#define CLIP_LEFT(gk, part, x, fade) {gk, part, {1.f, 0.f, 0.f, -(x)}, fade}
#define CLIP_RIGHT(gk, part, x, fade) {gk, part, {-1.f, 0.f, 0.f, (x)}, fade}

static const ClipRule s_clips[] = {
    CLIP_BELOW(0x0C, 3, -80.f, 0.f), /* Fountain of Dreams: the pole under the ornament */
    /* Kongo Jungle: the waterfall down to the floating rock, fading out, and
     * cut short behind the stage. */
    CLIP_BELOW(0x04, 4, -70.f, 35.f),
    CLIP_BEHIND(0x04, 4, -90.f, 15.f),
    CLIP_BELOW(0x0A, 3, -40.f, 0.f), /* Yoshi's Story: the pillar under the Shy Guys' path */
    CLIP_BELOW(0x06, 1, 0.f, 0.f),   /* Great Bay: the turtle below the waterline */
    CLIP_BELOW(0x06, 2, 0.f, 0.f),   /* Great Bay: the pier's stilts, rocks and screw */
    /* Great Bay: the sea bounded to the stage's footprint. */
    CLIP_LEFT(0x06, 4, -320.f, 10.f),
    CLIP_RIGHT(0x06, 4, 220.f, 10.f),
    CLIP_FRONT(0x06, 4, 220.f, 10.f),
    CLIP_BELOW(0x0B, 1, -40.f, 0.f), /* Yoshi's Island: the ground, halfway down */
    /* Jungle Japes: the river bounded to a rectangle just past the piers. */
    CLIP_LEFT(0x05, 6, -125.f, 8.f),
    CLIP_RIGHT(0x05, 6, 125.f, 8.f),
    CLIP_FRONT(0x05, 6, 45.f, 8.f),
    CLIP_BEHIND(0x05, 6, -100.f, 8.f),
    /* Onett: the block you fight on, with its lot, the clothesline between
     * the poles over the right house (it has collision) and the road in
     * front, cut out of the town. */
    CLIP_LEFT(0x14, 5, -140.f, 6.f),
    CLIP_RIGHT(0x14, 5, 150.f, 6.f),
    CLIP_BEHIND(0x14, 5, -70.f, 6.f),
    CLIP_FRONT(0x14, 5, 70.f, 6.f),
    CLIP_BELOW(0x18, 3, -40.f, 15.f), /* Mushroom Kingdom: the ground's deep blocks */
    /* Brinstar: only the stretch of the acid's river around the stage (it
     * runs the length of the cave). LevelRule hides it while it is low. */
    CLIP_LEFT(0x08, 8, -120.f, 12.f),
    CLIP_RIGHT(0x08, 8, 125.f, 12.f),
    CLIP_FRONT(0x08, 8, 70.f, 15.f),
    CLIP_BEHIND(0x08, 8, -90.f, 15.f),
};
#define CLIP_COUNT ((int)(sizeof s_clips / sizeof s_clips[0]))

/* Where each stage sits in the arena by default: the game point placed at
 * the arena position (its main floor, centered on where the fight happens)
 * and its size against the arena's shared scale (1: Final Destination's
 * ~170-unit floor is about 1 m wide at the default 0.006 m/unit). Sizes
 * meet halfway between keeping the game's proportions and making every
 * stage's floors as wide as Final Destination's, sqrt(171 / floor width),
 * so Temple still reads as big and Fountain as small, but both fit in view
 * at the default distance. The player's grabs place only the stage being
 * played, for the rest of the session. Stages not listed sit at the world
 * origin at scale 1. MELEE_XR_CENTER="x,y,z" and MELEE_XR_STAGE_SCALE
 * override for the stage being played. */
typedef struct {
    int grkind;
    float x, y, z;
    float scale;
} StagePlacement;

static const StagePlacement s_placements[] = {
    {0x02, 0.f, 84.f, 0.f, 0.8f},    /* Peach's Castle: the roof, high above the origin */
    {0x04, 5.f, 0.f, 0.f, 1.f},      /* Kongo Jungle */
    {0x05, 0.f, 0.f, 0.f, 0.9f},     /* Jungle Japes */
    {0x06, -10.f, 0.f, 0.f, 0.85f},  /* Great Bay: its floors reach further left */
    {0x07, 0.f, 0.f, 0.f, 0.55f},    /* Hyrule Temple: smaller than the rule gives, to fit in view */
    {0x08, 0.f, 0.f, 0.f, 1.1f},     /* Brinstar */
    {0x0A, 0.f, 0.f, 0.f, 1.15f},    /* Yoshi's Story */
    {0x0B, 0.f, 0.f, 0.f, 0.9f},     /* Yoshi's Island */
    {0x0C, 0.f, 0.f, 0.f, 1.15f},    /* Fountain of Dreams */
    {0x0D, 0.f, 0.f, 0.f, 0.9f},     /* Green Greens */
    {0x0E, -10.f, 285.f, 0.f, 0.8f}, /* Corneria: the Great Fox's deck, far above the origin */
    {0x0F, 0.f, 0.f, 0.f, 0.95f},    /* Venom */
    {0x10, 0.f, 0.f, 0.f, 1.f},      /* Pokemon Stadium */
    {0x14, 0.f, 0.f, 0.f, 0.8f},     /* Onett */
    {0x15, 0.f, 0.f, 0.f, 0.8f},     /* Fourside */
    {0x18, 0.f, 0.f, 0.f, 0.9f},     /* Mushroom Kingdom */
    {0x1B, 0.f, 0.f, 0.f, 1.1f},     /* Flat Zone */
    {0x1C, 0.f, 0.f, 0.f, 1.05f},    /* Dream Land */
    {0x1D, 0.f, 0.f, 0.f, 1.f},      /* Yoshi's Island 64 */
    {0x1E, 0.f, 0.f, 0.f, 1.f},      /* Kongo Jungle 64 */
    {0x24, 0.f, 0.f, 0.f, 1.1f},     /* Battlefield */
    {0x25, 0.f, 0.f, 0.f, 1.f},      /* Final Destination */
};
#define PLACEMENT_COUNT ((int)(sizeof s_placements / sizeof s_placements[0]))

/* Stage parts hidden in the 3D view while one of their joints (by index in
 * a depth-first walk, as in PartRule) sits below a height (its translation,
 * game units). For hazards that only matter once they come up. */
typedef struct {
    int grkind;
    int map_id;
    int jobj;
    float min_y;  /* below: hidden */
    float full_y; /* above: solid; in between it dissolves in */
} LevelRule;

static const LevelRule s_levels[] = {
    /* Brinstar: the acid (joint 1's height is its level, about -250 to 90;
     * the surface sits about 90 below). It starts far below, out of bounds,
     * and fades in as it rises to just under the stage. */
    {0x08, 8, 1, -50.f, -10.f},
};
#define LEVEL_COUNT ((int)(sizeof s_levels / sizeof s_levels[0]))

static HSD_JObj* nth_joint(HSD_JObj* jobj, int* n) {
    for (; jobj != NULL; jobj = jobj->next) {
        if ((*n)-- == 0) {
            return jobj;
        }
        HSD_JObj* found = nth_joint(jobj->child, n);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

/* 1: shown; 0: hidden; in between, how far a rising part has faded in. */
static float level_opacity(int grkind, int map_id, HSD_JObj* root) {
    for (int i = 0; i < LEVEL_COUNT; i++) {
        if (s_levels[i].grkind != grkind || s_levels[i].map_id != map_id || root == NULL) {
            continue;
        }
        int n = s_levels[i].jobj;
        /* The root alone: its siblings are other parts. */
        HSD_JObj* j = n == 0 ? root : (n--, nth_joint(root->child, &n));
        if (j != NULL && getenv("MELEE_XR_LEVEL_LOG") != NULL) {
            static unsigned calls;
            if (calls++ % 120 == 0) {
                pc_log_line("xr: stage %d part %d joint %d at y %.1f (shown from %.1f)", grkind, map_id,
                            s_levels[i].jobj, j->translate.y, s_levels[i].min_y);
            }
        }
        if (j != NULL) {
            const LevelRule* l = &s_levels[i];
            float lo = l->min_y, hi = l->full_y;
            const char* band = getenv("MELEE_XR_LEVEL_FADE"); /* surveying: "min,full" */
            if (band != NULL) {
                sscanf(band, "%f,%f", &lo, &hi);
            }
            const float t = hi > lo ? (j->translate.y - lo) / (hi - lo) : (j->translate.y >= lo ? 1.f : 0.f);
            return t <= 0.f ? 0.f : t >= 1.f ? 1.f : t;
        }
    }
    return 1.f;
}

#define MAX_RULES 256
static PartRule s_rules[MAX_RULES];
static int s_rule_count = -1;
/* Stage passes draw layers 2, 1, 0, 3 in that order. Layer 2 is the far
 * background (Final Destination: the space backdrop, parts 4-8); the stage
 * itself sits on layers 1 and 0 (FD: parts 1, 3 and 0, 2). Show everything
 * but layer 2 unless a rule says otherwise. */
static unsigned s_layer_mask = 0xB;
static bool s_log_parts;
static bool s_log_joints;
static int s_category = AURORA_XR_MONO;

/* The stage part being drawn, when it has joint rules: the joints with
 * anything hidden or moved, sorted by pointer for bsearch, with whether
 * the whole joint is hidden, a mask of their hidden meshes, and their move
 * (-1: none). */
#define MAX_HIDDEN_JOINTS 1024
#define MAX_DOBJS 256 /* Onett's town joint alone has 100 meshes */
typedef struct {
    uint64_t w[MAX_DOBJS / 64];
} MeshMask;
typedef struct {
    HSD_JObj* jobj;
    bool all; /* the whole joint */
    MeshMask dobjs;
    int move;
} HiddenJoint;

static bool mask_test(const MeshMask* m, int i) {
    return i >= 0 && i < MAX_DOBJS && (m->w[i / 64] >> (i % 64) & 1);
}

static void mask_set(MeshMask* m, int i, bool on) {
    if (i < 0 || i >= MAX_DOBJS) {
        return;
    }
    const uint64_t bit = (uint64_t) 1 << (i % 64);
    m->w[i / 64] = on ? m->w[i / 64] | bit : m->w[i / 64] & ~bit;
}

static bool mask_any(const MeshMask* m) {
    for (int k = 0; k < MAX_DOBJS / 64; k++) {
        if (m->w[k] != 0) {
            return true;
        }
    }
    return false;
}
enum { CHANGED_MONO = 1, CHANGED_MOVE = 2 };
static bool s_joint_mode;
static HiddenJoint s_hidden_joints[MAX_HIDDEN_JOINTS];
static int s_hidden_joint_count;

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
    s_log_joints = getenv("MELEE_XR_JOINT_LOG") != NULL;
    s_move_count = 0;
    while (s_move_count < (int)(sizeof s_moves / sizeof s_moves[0]) && s_moves[s_move_count].scale != 0.f) {
        s_move_count++;
    }
    /* MELEE_XR_MOVE="gk:part:joint:scale:px:py:pz:tx:ty:tz;...": try moves. */
    for (const char* m = getenv("MELEE_XR_MOVE"); m != NULL && *m != '\0';) {
        MoveRule r;
        if (s_move_count < (int)(sizeof s_moves / sizeof s_moves[0]) &&
            sscanf(m, "%d:%d:%d:%f:%f:%f:%f:%f:%f:%f", &r.grkind, &r.map_id, &r.jobj, &r.scale, &r.pivot[0],
                &r.pivot[1], &r.pivot[2], &r.to[0], &r.to[1], &r.to[2]) == 10) {
            s_moves[s_move_count++] = r;
        }
        m = strchr(m, ';');
        m = m != NULL ? m + 1 : NULL;
    }
    const char* jumbo = getenv("MELEE_XR_JUMBOTRON");
    float js, jx, jy, jz;
    if (jumbo != NULL && sscanf(jumbo, "%f,%f,%f,%f", &js, &jx, &jy, &jz) == 4) {
        for (int i = 0; i < MOVE_COUNT; i++) {
            if (s_moves[i].grkind == GRKIND_PSTADIUM && s_moves[i].map_id == PSTYPE_DISPLAY) {
                s_moves[i].scale = js;
                s_moves[i].to[0] = jx;
                s_moves[i].to[1] = jy;
                s_moves[i].to[2] = jz;
            }
        }
    }
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
        long joint = -1, mesh = -1;
        if (*end == '/') {
            spec = end + 1;
            joint = strtol(spec, &end, 0);
            if (end == spec) {
                break;
            }
            if (*end == '.') {
                spec = end + 1;
                mesh = strtol(spec, &end, 0);
                if (end == spec || mesh < 0 || mesh > 31) {
                    break;
                }
            }
        }
        s_rules[s_rule_count++] = (PartRule){(int)kind, (int)id, (int)joint, (int)mesh, show};
        spec = *end == ',' ? end + 1 : end;
    }
}

static bool part_visible(int grkind, int map_id, int layer) {
    if (s_rule_count < 0) {
        load_rules();
    }
    bool visible = layer >= 0 && layer < 8 && (s_layer_mask & (1u << layer)) != 0;
    for (int i = 0; i < s_rule_count; i++) {
        if (s_rules[i].grkind == grkind && s_rules[i].map_id == map_id && s_rules[i].jobj < 0) {
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

static bool has_joint_rules(int grkind, int map_id) {
    for (int i = 0; i < s_rule_count; i++) {
        if (s_rules[i].grkind == grkind && s_rules[i].map_id == map_id && s_rules[i].jobj >= 0) {
            return true;
        }
    }
    for (int i = 0; i < MOVE_COUNT; i++) {
        if (s_moves[i].grkind == grkind && s_moves[i].map_id == map_id) {
            return true;
        }
    }
    return false;
}

/* Depth-first, in the order HSD_JObjDispAll draws: a joint, its children,
 * then its next sibling. */
static void walk_joints(HSD_JObj* jobj, int grkind, int map_id, int depth, bool visible, int move, int* index,
    bool log) {
    for (; jobj != NULL; jobj = jobj->next) {
        const int idx = (*index)++;
        bool vis = visible;
        int mv = move;
        for (int i = 0; i < MOVE_COUNT; i++) {
            if (s_moves[i].grkind == grkind && s_moves[i].map_id == map_id && s_moves[i].jobj == idx) {
                mv = i;
            }
        }
        MeshMask dobjs = {{0}}; /* meshes hidden while the joint is shown */
        for (int i = 0; i < s_rule_count; i++) {
            const PartRule* r = &s_rules[i];
            if (r->grkind != grkind || r->map_id != map_id || r->jobj != idx) {
                continue;
            }
            if (r->dobj < 0) {
                vis = r->show;
                dobjs = (MeshMask){{0}};
            } else {
                mask_set(&dobjs, r->dobj, !r->show);
            }
        }
        const bool some = mask_any(&dobjs);
        if ((!vis || some || mv >= 0) && s_hidden_joint_count < MAX_HIDDEN_JOINTS) {
            s_hidden_joints[s_hidden_joint_count++] = (HiddenJoint){jobj, !vis, dobjs, mv};
        }
        if (log) {
            int meshes = 0;
            if (union_type_dobj(jobj)) {
                for (HSD_DObj* d = jobj->u.dobj; d != NULL; d = d->next) {
                    meshes++;
                }
            }
            pc_log_line("xr: stage %d part %d joint %d depth %d %d meshes%s at %.0f,%.0f,%.0f -> %s", grkind,
                map_id, idx, depth, meshes, (jobj->flags & JOBJ_HIDDEN) ? " (hidden)" : "", jobj->mtx[0][3],
                jobj->mtx[1][3], jobj->mtx[2][3], vis ? (some ? "3D, some meshes hidden" : "3D") : "hidden");
        }
        walk_joints(jobj->child, grkind, map_id, depth + 1, vis, mv, index, log);
    }
}

static int compare_joint(const void* a, const void* b) {
    const uintptr_t x = (uintptr_t)((const HiddenJoint*)a)->jobj, y = (uintptr_t)((const HiddenJoint*)b)->jobj;
    return x < y ? -1 : x > y;
}

static const HiddenJoint* find_joint(HSD_JObj* jobj) {
    if (!s_joint_mode || s_hidden_joint_count == 0) {
        return NULL;
    }
    const HiddenJoint key = {jobj, 0, -1};
    return bsearch(&key, s_hidden_joints, (size_t)s_hidden_joint_count, sizeof s_hidden_joints[0], compare_joint);
}

/* MELEE_XR_JOINT_LOG: each part's joints, once, on the part's 60th draw so
 * its matrices are set up. */
static bool joint_log_due(int grkind, int map_id) {
    static unsigned char draws[64][128];
    unsigned char* n = &draws[(unsigned)grkind & 63u][(unsigned)map_id & 127u];
    if (*n > 60) {
        return false;
    }
    return ++*n == 60;
}

static int s_center_grkind = -1;
static bool s_clip_active;

static int s_xr_mode = -1; /* 1 mixed reality, 0 full VR */

static void xr_mode_init(void) {
    if (s_xr_mode < 0) {
        const char* m = getenv("MELEE_XR_MODE");
        s_xr_mode = m == NULL || strcmp(m, "vr") != 0;
        aurora_xr_set_passthrough(s_xr_mode == 1);
    }
}

bool pc_xr_mixed_reality(void) {
    xr_mode_init();
    return s_xr_mode == 1 && aurora_xr_active();
}

bool pc_xr_toggle_mode(void) {
    if (!aurora_xr_active()) {
        return false;
    }
    xr_mode_init();
    s_xr_mode = !s_xr_mode;
    aurora_xr_set_passthrough(s_xr_mode == 1);
    pc_log_line("xr: %s", s_xr_mode ? "mixed reality" : "full VR");
    return true;
}

/* One player wears the headset, and netplay sends port 1, so an external
 * gamepad plays as player one in XR: the newest one connected takes the
 * port, and when port 1 has none (a pad paired before the session began, or
 * port 1's pad unplugged), the lowest-port pad moves there. The headset's
 * own controllers merge into port 1's virtual pad (keyboard.c) either way.
 * Only pads aurora tracks count: SDL also lists devices aurora skips (Quest's
 * own input shows up as a gamepad and still holds SDL's first player slot). */
static void xr_gamepad_to_port1(s32 index) {
    /* Through PAD so controller_ports.dat agrees and aurora's saved
     * preferences don't move it back on the next hotplug. */
    PADSetPortForIndex((u32)index, 0);
    pc_log_line("xr: gamepad '%s' on port 1", PADGetName(0));
}

void pc_xr_gamepad_added(int instance) {
    if (!aurora_xr_active() || instance < 0) {
        return;
    }
    SDL_Gamepad* pad = SDL_GetGamepadFromID((SDL_JoystickID)instance);
    const int port = pad != NULL ? SDL_GetGamepadPlayerIndex(pad) : 0;
    if (port > 0 && port < PAD_CHANMAX) {
        const s32 index = PADGetIndexForPort((u32)port);
        if (index >= 0) {
            xr_gamepad_to_port1(index);
        }
    }
}

static void xr_gamepad_fill_port1(void) {
    if (!aurora_xr_active() || PADGetIndexForPort(0) >= 0) {
        return;
    }
    for (u32 port = 1; port < PAD_CHANMAX; port++) {
        const s32 index = PADGetIndexForPort(port);
        if (index >= 0) {
            xr_gamepad_to_port1(index);
            return;
        }
    }
}

void pc_xr_poll_control(void) {
    xr_gamepad_fill_port1();
    static const char* path;
    static int frames;
    static char last[16];
    if (frames++ % 60 != 0) {
        return;
    }
    if (path == NULL) {
        path = getenv("MELEE_XR_CONTROL");
        if (path == NULL) {
            path = "";
        }
    }
    FILE* f = *path != '\0' ? fopen(path, "r") : NULL;
    if (f == NULL) {
        return;
    }
    char cmd[16] = {0};
    if (fgets(cmd, sizeof cmd, f) != NULL) {
        cmd[strcspn(cmd, "\r\n ")] = '\0';
    }
    fclose(f);
    if (strcmp(cmd, last) == 0) {
        return;
    }
    strcpy(last, cmd);
    const int want = strcmp(cmd, "mr") == 0 ? 1 : strcmp(cmd, "vr") == 0 ? 0 : -1;
    xr_mode_init();
    if (want >= 0 && want != s_xr_mode) {
        pc_xr_toggle_mode();
    }
}

/* The highest floor within the blast zones (world units), for the HUD;
 * NAN when the stage has none or its collision isn't loaded yet. The walk
 * is in mplib.c: the collision data is in the disc's byte order, which this
 * file isn't built to read on every platform (clang on Android). */
static int s_top_grkind = -1;

static float stage_top(void) {
    float top, min_x, max_x;
    if (!mpLib_FloorExtent(Stage_GetBlastZoneLeftOffset(), Stage_GetBlastZoneRightOffset(),
                           Stage_GetBlastZoneBottomOffset(), Stage_GetBlastZoneTopOffset(), &top, &min_x, &max_x)) {
        return NAN;
    }
    pc_log_line("xr: stage %d top floor at y %.1f; floors from x %.1f to %.1f", stage_info.grkind, top, min_x,
                max_x);
    return top;
}

bool pc_xr_stage_part_begin(int grkind, int map_id, int layer, HSD_JObj* root) {
    if (s_category != AURORA_XR_WORLD) {
        return false;
    }
    if (grkind != s_center_grkind) {
        s_center_grkind = grkind;
        float c[3] = {0.f, 0.f, 0.f};
        float scale = 1.f;
        for (int i = 0; i < PLACEMENT_COUNT; i++) {
            if (s_placements[i].grkind == grkind && grkind != 0) {
                c[0] = s_placements[i].x;
                c[1] = s_placements[i].y;
                c[2] = s_placements[i].z;
                scale = s_placements[i].scale;
            }
        }
        const char* env = getenv("MELEE_XR_CENTER");
        if (env != NULL) {
            sscanf(env, "%f,%f,%f", &c[0], &c[1], &c[2]);
        }
        env = getenv("MELEE_XR_STAGE_SCALE");
        if (env != NULL) {
            scale = (float) atof(env);
        }
        aurora_xr_set_stage(grkind, c[0], c[1], c[2], scale);
        s_top_grkind = -1;
    }
    if (grkind != s_top_grkind) {
        /* Until the stage's collision is in (then once per stage). */
        const float top = stage_top();
        aurora_xr_set_stage_top(top);
        if (!isnan(top)) {
            s_top_grkind = grkind;
            pc_xr_place_stage_ready(grkind);
        }
    }
    s_clip_active = false;
    if (!pc_xr_mixed_reality()) {
        return false; /* full VR: the whole stage, unmoved and unclipped */
    }
    if (getenv("MELEE_XR_CLIP_LOG") != NULL) {
        static unsigned n;
        if (n++ < 40) {
            pc_log_line("xr: part begin stage %d part %d layer %d", grkind, map_id, layer);
        }
    }
    float planes[4][4];
    float fades[4];
    int n_planes = 0;
    for (int i = 0; i < CLIP_COUNT && n_planes < 4; i++) {
        if (s_clips[i].grkind == grkind && s_clips[i].map_id == map_id && grkind != 0) {
            memcpy(planes[n_planes], s_clips[i].plane, sizeof planes[0]);
            fades[n_planes++] = s_clips[i].fade;
        }
    }
    /* MELEE_XR_CLIP="<grkind>:<map_id>:<y>[:<fade>],...": try clip heights;
     * MELEE_XR_CLIPZ the same for a cut behind z; MELEE_XR_CLIPP
     * "<grkind>:<map_id>:<a>:<b>:<c>:<d>[:<fade>];..." any plane (kept where
     * ax+by+cz+d >= 0). Any env plane replaces the built-in ones (up to 4). */
    float env_planes[4][4];
    float env_fades[4] = {0.f, 0.f, 0.f, 0.f};
    int env_n = 0;
    const char* env = getenv("MELEE_XR_CLIP");
    while (env != NULL && *env != '\0') {
        int gk, id;
        float y, fade = 0.f;
        if (sscanf(env, "%d:%d:%f:%f", &gk, &id, &y, &fade) >= 3 && gk == grkind && id == map_id && env_n < 4) {
            const float plane[4] = {0.f, 1.f, 0.f, -y};
            memcpy(env_planes[env_n], plane, sizeof plane);
            env_fades[env_n++] = fade;
        }
        env = strchr(env, ',');
        env = env != NULL ? env + 1 : NULL;
    }
    for (const char* z = getenv("MELEE_XR_CLIPZ"); z != NULL && *z != '\0';) {
        int gk, id;
        float v, fade = 0.f;
        if (sscanf(z, "%d:%d:%f:%f", &gk, &id, &v, &fade) >= 3 && gk == grkind && id == map_id && env_n < 4) {
            const float plane[4] = {0.f, 0.f, 1.f, -v};
            memcpy(env_planes[env_n], plane, sizeof plane);
            env_fades[env_n++] = fade;
        }
        z = strchr(z, ',');
        z = z != NULL ? z + 1 : NULL;
    }
    for (const char* q = getenv("MELEE_XR_CLIPP"); q != NULL && *q != '\0';) {
        int gk, id;
        float pl[4], fade = 0.f;
        if (sscanf(q, "%d:%d:%f:%f:%f:%f:%f", &gk, &id, &pl[0], &pl[1], &pl[2], &pl[3], &fade) >= 6 &&
            gk == grkind && id == map_id && env_n < 4) {
            memcpy(env_planes[env_n], pl, sizeof pl);
            env_fades[env_n++] = fade;
        }
        q = strchr(q, ';');
        q = q != NULL ? q + 1 : NULL;
    }
    if (env_n > 0) {
        memcpy(planes, env_planes, sizeof planes);
        memcpy(fades, env_fades, sizeof fades);
        n_planes = env_n;
    }
    const float opacity = level_opacity(grkind, map_id, root);
    if (n_planes > 0 || (opacity > 0.f && opacity < 1.f)) {
        aurora_xr_world_clips4_fade((const float(*)[4])planes, fades, n_planes, opacity);
        s_clip_active = true;
    }
    const bool visible = part_visible(grkind, map_id, layer) && opacity > 0.f;
    const bool log = s_log_joints && joint_log_due(grkind, map_id);
    if (has_joint_rules(grkind, map_id) || log) {
        s_hidden_joint_count = 0;
        int index = 0;
        walk_joints(root, grkind, map_id, 0, visible, -1, &index, log);
        qsort(s_hidden_joints, (size_t)s_hidden_joint_count, sizeof s_hidden_joints[0], compare_joint);
        s_joint_mode = true;
        return false;
    }
    if (visible) {
        return false;
    }
    /* Not re-drawn in 3D. The flat frame still draws it where a pass is
     * read (the stadium screen's feed copies it), and drops it elsewhere. */
    aurora_xr_camera(AURORA_XR_HIDDEN, NULL);
    return true;
}

void pc_xr_stage_part_end(bool hidden) {
    s_joint_mode = false;
    if (s_clip_active) {
        aurora_xr_world_clip(NULL);
        s_clip_active = false;
    }
    if (hidden) {
        aurora_xr_camera(AURORA_XR_WORLD, NULL);
    }
}

/* Stage particles left out of the 3D view, by GrKind and the particle's
 * bank and id (HSD_Particle bank/idnum; id -1 is the whole bank, and ids
 * number instances, so most rules want that). Extend with MELEE_XR_PTCL
 * "<grkind>:<bank>:<id>,..."; MELEE_XR_PTCL_LOG lists each one drawn. */
typedef struct {
    int grkind;
    int bank;
    int id;
} ParticleRule;

static const ParticleRule s_particles[] = {
    /* Kongo Jungle: the river's splashes (bank 30 is the stage's own), left
     * floating once the river is hidden. */
    {0x04, 30, -1},
};
#define PARTICLE_COUNT ((int)(sizeof s_particles / sizeof s_particles[0]))

/* MELEE_XR_PTCL, parsed once: this runs for every particle drawn. */
#define PARTICLE_ENV_MAX 32
static ParticleRule s_env_particles[PARTICLE_ENV_MAX];
static int s_env_particle_count = -1;

static void load_particle_rules(void) {
    s_env_particle_count = 0;
    for (const char* q = getenv("MELEE_XR_PTCL"); q != NULL && *q != '\0';) {
        int g, b, n;
        if (sscanf(q, "%d:%d:%d", &g, &b, &n) == 3 && s_env_particle_count < PARTICLE_ENV_MAX) {
            s_env_particles[s_env_particle_count++] = (ParticleRule){g, b, n};
        }
        q = strchr(q, ',');
        q = q != NULL ? q + 1 : NULL;
    }
}

static bool particle_rule(int grkind, int bank, int id) {
    if (s_env_particle_count < 0) {
        load_particle_rules();
    }
    for (int i = 0; i < PARTICLE_COUNT; i++) {
        const ParticleRule* r = &s_particles[i];
        if (r->grkind == grkind && r->bank == bank && (r->id < 0 || r->id == id)) {
            return true;
        }
    }
    for (int i = 0; i < s_env_particle_count; i++) {
        const ParticleRule* r = &s_env_particles[i];
        if (r->grkind == grkind && r->bank == bank && (r->id < 0 || r->id == id)) {
            return true;
        }
    }
    return false;
}

bool pc_xr_particle_begin(int bank, int id, const float pos[3]) {
    if (s_category != AURORA_XR_WORLD || s_center_grkind <= 0) {
        return false;
    }
    static int log = -1, hide_all = -1;
    if (log < 0) {
        log = getenv("MELEE_XR_PTCL_LOG") != NULL;
        /* MELEE_XR_PTCL_TEST_HIDE=1: every particle out of 3D and the flat
         * frame, to measure what particles cost (a test knob). */
        hide_all = getenv("MELEE_XR_PTCL_TEST_HIDE") != NULL;
    }
    if (hide_all) {
        aurora_xr_camera(AURORA_XR_HIDDEN, NULL);
        return true;
    }
    if (log) {
        static unsigned char seen[256][64];
        unsigned char* f = &seen[bank & 255][(id & 511) >> 3];
        if (!(*f & (1u << (id & 7)))) {
            *f |= 1u << (id & 7);
            pc_log_line("xr: stage %d particle %d:%d at %.0f,%.0f,%.0f", s_center_grkind, bank, id, pos[0], pos[1],
                        pos[2]);
        }
    }
    if (!pc_xr_mixed_reality() || !particle_rule(s_center_grkind, bank, id)) {
        return false;
    }
    aurora_xr_camera(AURORA_XR_HIDDEN, NULL);
    return true;
}

void pc_xr_particle_end(bool hidden) {
    if (hidden) {
        aurora_xr_camera(AURORA_XR_WORLD, NULL);
    }
}

int pc_xr_jobj_begin(HSD_JObj* jobj) {
    const HiddenJoint* h = find_joint(jobj);
    if (h == NULL) {
        return 0;
    }
    if (h->all) {
        aurora_xr_camera(AURORA_XR_HIDDEN, NULL);
        return CHANGED_MONO;
    }
    if (h->move < 0) {
        return 0;
    }
    const MoveRule* m = &s_moves[h->move];
    const float s = m->scale;
    const float t[3][4] = {
        {s, 0, 0, m->to[0] - s * m->pivot[0]},
        {0, s, 0, m->to[1] - s * m->pivot[1]},
        {0, 0, s, m->to[2] - s * m->pivot[2]},
    };
    aurora_xr_world_transform(t);
    return CHANGED_MOVE;
}

int pc_xr_dobj_begin(HSD_JObj* jobj, int dobj_index) {
    const HiddenJoint* h = find_joint(jobj);
    /* A wholly hidden joint is already out of the 3D view. */
    if (h == NULL || h->all || !mask_test(&h->dobjs, dobj_index)) {
        return 0;
    }
    aurora_xr_camera(AURORA_XR_HIDDEN, NULL);
    return CHANGED_MONO;
}

void pc_xr_jobj_end(int changed) {
    if (changed & CHANGED_MONO) {
        aurora_xr_camera(AURORA_XR_WORLD, NULL);
    }
    if (changed & CHANGED_MOVE) {
        aurora_xr_world_transform(NULL);
    }
}

#endif
