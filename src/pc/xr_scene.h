/* SPDX-License-Identifier: GPL-3.0-or-later */
/* What the game draws, for the XR build's 3D fights (docs/xr-3d-plan.md).
 * The fight camera's draws are re-drawn per eye in 3D, the HUD camera's on
 * a flat plane, everything else only in the normal flat frame. Stage parts
 * on background layers are left out of the 3D view unless listed.
 * No-ops unless built with AURORA_ENABLE_OPENXR. */
#ifndef PC_XR_SCENE_H
#define PC_XR_SCENE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef AURORA_ENABLE_OPENXR
/* After HSD_CObjSetCurrent of the main fight camera (cm/camera.c); `view` is
 * its view matrix (cobj->view_mtx). */
void pc_xr_world_camera(const float view[3][4]);
/* After HSD_CObjSetCurrent of the HUD camera (if/ifall.c). */
void pc_xr_hud_camera(void);
/* After the camera's HSD_CObjEndCurrent. */
void pc_xr_mono_camera(void);
struct HSD_JObj;
/* Around one stage part's draw (gr/grdisplay.c); `root` is the part's joint
 * tree. Returns true when the whole part is left out of the 3D view; hand
 * that to pc_xr_stage_part_end. Only acts under the fight camera. */
bool pc_xr_stage_part_begin(int grkind, int map_id, int layer, struct HSD_JObj* root);
void pc_xr_stage_part_end(bool hidden);
/* Mixed-reality staging (stage parts hidden, clipped and moved for the arena
 * over passthrough) is in effect: an XR session is presenting and
 * MELEE_XR_MODE is not "vr". A full-VR mode renders the stage whole; only
 * per-stage centering applies there. */
bool pc_xr_mixed_reality(void);
/* A stage is loading for a fight (gr/ground.c): once its floors are in, it
 * is offered for placing again (xr_place.h), the same stage or not. */
void pc_xr_stage_load(void);
/* An XR session is presenting the game, in either mode. */
bool pc_xr_presenting(void);
/* Switches between mixed reality and full VR (passthrough off, stage whole);
 * the gamepad's Select does this in XR. False, doing nothing, when no XR
 * session is presenting. */
bool pc_xr_toggle_mode(void);
/* MELEE_XR_CONTROL=<file>: once a second, the file's first line ("mr" or
 * "vr") picks the mode when it changes, so an unattended run can switch
 * without a gamepad. */
void pc_xr_poll_control(void);
/* AURORA_CONTROLLER_ADDED: in XR, the new gamepad takes port 1. */
void pc_xr_gamepad_added(int instance);
/* Around one joint's geometry (HSD_JObjDisp): leaves joints a part rule
 * hides out of the 3D view, and places joints a rule moves. Returns what
 * it changed; hand that to pc_xr_jobj_end. Cheap when no stage part with
 * joint rules is drawing. */
int pc_xr_jobj_begin(struct HSD_JObj* jobj);
/* Same, for one mesh (the joint's dobj_index'th DObj); hides only. */
int pc_xr_dobj_begin(struct HSD_JObj* jobj, int dobj_index);
void pc_xr_jobj_end(int changed);
/* Around one particle's draw (psDispParticles): leaves the stage's listed
 * particles out of the 3D view under mixed reality. `pos` is its position. */
bool pc_xr_particle_begin(int bank, int id, const float pos[3]);
void pc_xr_particle_end(bool hidden);
/* Around the draw of a fighter the flat camera can't see (ft/ftdrawcommon.c),
 * which the game skips: true when the 3D view draws it anyway, pulsing
 * translucent; then call pc_xr_fighter_offscreen_end after it. Its logic
 * and the off-screen damage are the game's own (MELEE_XR_OFFSCREEN=0: the
 * game's way). */
bool pc_xr_fighter_offscreen_begin(void);
void pc_xr_fighter_offscreen_end(void);
/* Around the magnifier bubble's draw (if/ifmagnify.c): true when it is kept
 * to the flat frame, off the XR HUD; then call pc_xr_magnify_end. */
bool pc_xr_magnify_begin(void);
void pc_xr_magnify_end(void);
#else
static inline bool pc_xr_presenting(void) { return false; }
static inline void pc_xr_stage_load(void) {}
static inline void pc_xr_world_camera(const float view[3][4]) { (void)view; }
static inline void pc_xr_hud_camera(void) {}
static inline void pc_xr_mono_camera(void) {}
struct HSD_JObj;
static inline bool pc_xr_stage_part_begin(int grkind, int map_id, int layer, struct HSD_JObj* root) {
    (void)grkind;
    (void)map_id;
    (void)layer;
    (void)root;
    return false;
}
static inline void pc_xr_stage_part_end(bool hidden) { (void)hidden; }
static inline bool pc_xr_mixed_reality(void) { return false; }
static inline bool pc_xr_toggle_mode(void) { return false; }
static inline void pc_xr_poll_control(void) {}
static inline void pc_xr_gamepad_added(int instance) { (void)instance; }
static inline int pc_xr_jobj_begin(struct HSD_JObj* jobj) {
    (void)jobj;
    return 0;
}
static inline int pc_xr_dobj_begin(struct HSD_JObj* jobj, int dobj_index) {
    (void)jobj;
    (void)dobj_index;
    return 0;
}
static inline void pc_xr_jobj_end(int changed) { (void)changed; }
static inline bool pc_xr_particle_begin(int bank, int id, const float pos[3]) {
    (void)bank;
    (void)id;
    (void)pos;
    return false;
}
static inline void pc_xr_particle_end(bool hidden) { (void)hidden; }
#endif

#ifdef __cplusplus
}
#endif

#endif
