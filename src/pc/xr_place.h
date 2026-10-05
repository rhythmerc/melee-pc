/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Placing a stage before its first fight of the session (XR build).
 *
 * The first time a stage's fight starts in a session, the game is held on
 * that first frame, still drawing, while the player moves, turns and scales
 * the stage with their hands; A or Start begins the fight, B puts the stage
 * back where it started, Y shows or hides the how-to cards (shown on the
 * session's first hold).
 *
 * The hold runs no simulation ticks at all: no game state changes, nothing
 * is consumed and no frame passes, so a replay or a recording can't tell.
 * It never happens in netplay, where the peer (VR or not, this build or
 * not) expects the match to start on the agreed frame. MELEE_XR_PLACE=0
 * turns it off. */
#ifndef PC_XR_PLACE_H
#define PC_XR_PLACE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef AURORA_ENABLE_OPENXR
/* Once per scene-loop pass, before the pass's ticks (gm/gmscene.c). True:
 * run no ticks this pass, only draw; the pads that came in are dropped. */
bool pc_xr_place_hold(void);
/* A stage's fight geometry is ready in the 3D view (xr_scene.c): offer the
 * hold for it next pass unless it was placed already this session. */
void pc_xr_place_stage_ready(int grkind);
#else
static inline bool pc_xr_place_hold(void) { return false; }
static inline void pc_xr_place_stage_ready(int grkind) { (void)grkind; }
#endif

#ifdef __cplusplus
}
#endif

#endif
