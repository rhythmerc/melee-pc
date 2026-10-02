/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "compat.h"
#include "pc/xr_scene.h"

#ifdef AURORA_ENABLE_OPENXR

#include "pc/pc.h"

#include <aurora/xr.h>
#include <melee/gm/gmscene.h>
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/jobj.h>

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
    /* Venom: the canyon walls and floor around the Great Fox. */
    {0x0F, 7, -1, -1, false},
    /* Kongo Jungle 64: the jungle (part 3 joint 7) and the sky (joint 34). */
    {0x1E, 3, 7, -1, false},
    {0x1E, 3, 34, -1, false},
    /* Yoshi's Island: the sky (part 1 joint 28). */
    {0x0B, 1, 28, -1, false},
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
};
#define CLIP_COUNT ((int)(sizeof s_clips / sizeof s_clips[0]))

/* The game point placed at the arena's center, for stages whose action is
 * far from the world origin. */
typedef struct {
    int grkind;
    float x, y, z;
} CenterRule;

static const CenterRule s_centers[] = {
    {0x0E, 40.f, 255.f, 0.f}, /* Corneria: the Great Fox flies far above the origin */
};
#define CENTER_COUNT ((int)(sizeof s_centers / sizeof s_centers[0]))

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
 * anything hidden or moved, sorted by pointer for bsearch, with a mask of
 * their hidden meshes (all bits: the whole joint) and their move (-1:
 * none). */
#define MAX_HIDDEN_JOINTS 1024
#define ALL_DOBJS 0xFFFFFFFFu
typedef struct {
    HSD_JObj* jobj;
    unsigned dobjs;
    int move;
} HiddenJoint;
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
        unsigned dobjs = 0; /* meshes hidden while the joint is shown */
        for (int i = 0; i < s_rule_count; i++) {
            const PartRule* r = &s_rules[i];
            if (r->grkind != grkind || r->map_id != map_id || r->jobj != idx) {
                continue;
            }
            if (r->dobj < 0) {
                vis = r->show;
                dobjs = 0;
            } else if (r->show) {
                dobjs &= ~(1u << r->dobj);
            } else {
                dobjs |= 1u << r->dobj;
            }
        }
        const unsigned hidden = vis ? dobjs : ALL_DOBJS;
        if ((hidden != 0 || mv >= 0) && s_hidden_joint_count < MAX_HIDDEN_JOINTS) {
            s_hidden_joints[s_hidden_joint_count++] = (HiddenJoint){jobj, hidden, mv};
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
                jobj->mtx[1][3], jobj->mtx[2][3], vis ? (dobjs ? "3D, some meshes hidden" : "3D") : "hidden");
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

bool pc_xr_mixed_reality(void) {
    static int mode = -1; /* 1 mixed reality, 0 full VR */
    if (mode < 0) {
        const char* m = getenv("MELEE_XR_MODE");
        mode = m == NULL || strcmp(m, "vr") != 0;
    }
    return mode == 1 && aurora_xr_active();
}

bool pc_xr_stage_part_begin(int grkind, int map_id, int layer, HSD_JObj* root) {
    if (s_category != AURORA_XR_WORLD) {
        return false;
    }
    if (grkind != s_center_grkind) {
        s_center_grkind = grkind;
        float c[3] = {0.f, 0.f, 0.f};
        for (int i = 0; i < CENTER_COUNT; i++) {
            if (s_centers[i].grkind == grkind && grkind != 0) {
                c[0] = s_centers[i].x;
                c[1] = s_centers[i].y;
                c[2] = s_centers[i].z;
            }
        }
        const char* env = getenv("MELEE_XR_CENTER");
        if (env != NULL) {
            sscanf(env, "%f,%f,%f", &c[0], &c[1], &c[2]);
        }
        aurora_xr_set_arena_center(c[0], c[1], c[2]);
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
    if (n_planes > 0) {
        aurora_xr_world_clips4((const float(*)[4])planes, fades, n_planes);
        s_clip_active = true;
    }
    const bool visible = part_visible(grkind, map_id, layer);
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
    /* Still drawn into the flat frame (the stadium screen's feed copies it),
     * just not re-drawn in 3D. */
    aurora_xr_camera(AURORA_XR_MONO, NULL);
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

int pc_xr_jobj_begin(HSD_JObj* jobj) {
    const HiddenJoint* h = find_joint(jobj);
    if (h == NULL) {
        return 0;
    }
    if (h->dobjs == ALL_DOBJS) {
        aurora_xr_camera(AURORA_XR_MONO, NULL);
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
    if (h == NULL || h->dobjs == ALL_DOBJS || dobj_index > 31 || !(h->dobjs & (1u << dobj_index))) {
        return 0;
    }
    aurora_xr_camera(AURORA_XR_MONO, NULL);
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
