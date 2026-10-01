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
/* Around one stage part's draw (gr/grdisplay.c). Returns true when the part
 * is left out of the 3D view; hand that to pc_xr_stage_part_end. Only acts
 * under the fight camera. */
bool pc_xr_stage_part_begin(int grkind, int map_id, int layer);
void pc_xr_stage_part_end(bool hidden);
#else
static inline void pc_xr_world_camera(const float view[3][4]) { (void)view; }
static inline void pc_xr_hud_camera(void) {}
static inline void pc_xr_mono_camera(void) {}
static inline bool pc_xr_stage_part_begin(int grkind, int map_id, int layer) {
    (void)grkind;
    (void)map_id;
    (void)layer;
    return false;
}
static inline void pc_xr_stage_part_end(bool hidden) { (void)hidden; }
#endif

#ifdef __cplusplus
}
#endif

#endif
