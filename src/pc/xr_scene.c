/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "compat.h"
#include "pc/xr_scene.h"

#ifdef AURORA_ENABLE_OPENXR

#include "pc/pc.h"

#include <aurora/xr.h>
#include <melee/gm/gmscene.h>
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/jobj.h>

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
    /* Part 2 joint 2 is the stadium bowl. Meshes 14-27 are the stands,
     * walls, entrance tunnels (25) and dark floor (27, a black void under
     * the arena); the light rings beside the big screen (0-13) stay. */
    {GRKIND_PSTADIUM, 2, 2, 14, false},
    {GRKIND_PSTADIUM, 2, 2, 15, false},
    {GRKIND_PSTADIUM, 2, 2, 16, false},
    {GRKIND_PSTADIUM, 2, 2, 17, false},
    {GRKIND_PSTADIUM, 2, 2, 18, false},
    {GRKIND_PSTADIUM, 2, 2, 19, false},
    {GRKIND_PSTADIUM, 2, 2, 20, false},
    {GRKIND_PSTADIUM, 2, 2, 21, false},
    {GRKIND_PSTADIUM, 2, 2, 22, false},
    {GRKIND_PSTADIUM, 2, 2, 23, false},
    {GRKIND_PSTADIUM, 2, 2, 24, false},
    {GRKIND_PSTADIUM, 2, 2, 25, false},
    {GRKIND_PSTADIUM, 2, 2, 26, false},
    {GRKIND_PSTADIUM, 2, 2, 27, false},
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
static bool s_log_joints;
static int s_category = AURORA_XR_MONO;

/* The stage part being drawn, when it has joint rules: the joints with
 * anything hidden, sorted by pointer for bsearch, with a mask of their
 * hidden meshes (all bits: the whole joint). */
#define MAX_HIDDEN_JOINTS 1024
#define ALL_DOBJS 0xFFFFFFFFu
typedef struct {
    HSD_JObj* jobj;
    unsigned dobjs;
} HiddenJoint;
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
    return false;
}

/* Depth-first, in the order HSD_JObjDispAll draws: a joint, its children,
 * then its next sibling. */
static void walk_joints(HSD_JObj* jobj, int grkind, int map_id, int depth, bool visible, int* index, bool log) {
    for (; jobj != NULL; jobj = jobj->next) {
        const int idx = (*index)++;
        bool vis = visible;
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
        if (hidden != 0 && s_hidden_joint_count < MAX_HIDDEN_JOINTS) {
            s_hidden_joints[s_hidden_joint_count++] = (HiddenJoint){jobj, hidden};
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
        walk_joints(jobj->child, grkind, map_id, depth + 1, vis, index, log);
    }
}

static int compare_joint(const void* a, const void* b) {
    const uintptr_t x = (uintptr_t)((const HiddenJoint*)a)->jobj, y = (uintptr_t)((const HiddenJoint*)b)->jobj;
    return x < y ? -1 : x > y;
}

static unsigned hidden_dobjs(HSD_JObj* jobj) {
    if (!s_joint_mode || s_hidden_joint_count == 0) {
        return 0;
    }
    const HiddenJoint key = {jobj, 0};
    const HiddenJoint* h =
        bsearch(&key, s_hidden_joints, (size_t)s_hidden_joint_count, sizeof s_hidden_joints[0], compare_joint);
    return h != NULL ? h->dobjs : 0;
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

bool pc_xr_stage_part_begin(int grkind, int map_id, int layer, HSD_JObj* root) {
    if (s_category != AURORA_XR_WORLD) {
        return false;
    }
    const bool visible = part_visible(grkind, map_id, layer);
    const bool log = s_log_joints && joint_log_due(grkind, map_id);
    if (has_joint_rules(grkind, map_id) || log) {
        s_hidden_joint_count = 0;
        int index = 0;
        walk_joints(root, grkind, map_id, 0, visible, &index, log);
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
    if (hidden) {
        aurora_xr_camera(AURORA_XR_WORLD, NULL);
    }
}

bool pc_xr_jobj_begin(HSD_JObj* jobj) {
    if (hidden_dobjs(jobj) != ALL_DOBJS) {
        return false;
    }
    aurora_xr_camera(AURORA_XR_MONO, NULL);
    return true;
}

bool pc_xr_dobj_begin(HSD_JObj* jobj, int dobj_index) {
    const unsigned hidden = hidden_dobjs(jobj);
    /* A wholly hidden joint is already out of the 3D view. */
    if (hidden == ALL_DOBJS || dobj_index > 31 || !(hidden & (1u << dobj_index))) {
        return false;
    }
    aurora_xr_camera(AURORA_XR_MONO, NULL);
    return true;
}

void pc_xr_jobj_end(bool hidden) {
    if (hidden) {
        aurora_xr_camera(AURORA_XR_WORLD, NULL);
    }
}

#endif
