#ifndef AURORA_XR_H
#define AURORA_XR_H

/* Headset controller input for XR builds (AURORA_ENABLE_OPENXR).
 * The XR thread reads the controllers through OpenXR each display frame; the
 * game thread merges the newest state into its pad input. */

#include <dolphin/pad.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Latest controller state as a GameCube pad for port 1. Returns false when
 * there is none (no XR session, or the app is not focused), in which case
 * *out is left untouched. Safe to call from any thread. */
bool aurora_xr_get_pad(PADStatus* out);

/* Draw categories for 3D fights (docs/xr-3d-plan.md). Everything drawn after
 * this call, until the next one, is tagged with the category.
 *   AURORA_XR_MONO   only the normal flat frame (the default every frame)
 *   AURORA_XR_WORLD  fight geometry, re-drawn per eye in 3D. Pass the world
 *                    camera's view matrix (world -> camera, GX Mtx layout);
 *                    NULL keeps the frame's current one.
 *   AURORA_XR_HUD    HUD, re-drawn onto its own flat plane
 * Call from the thread that issues GX commands. No-op unless an XR session
 * is presenting. */
enum { AURORA_XR_MONO = 0, AURORA_XR_WORLD = 1, AURORA_XR_HUD = 2 };

/* Lock-step pacing: when the headset runs at a multiple of 60 Hz, blocks
 * until it is time for the next game frame (every display frame at 60 Hz,
 * every other at 120 Hz) and returns true; the caller then skips its own
 * 60 Hz timer. Returns false at once when there is nothing to pace to (no
 * session, a non-multiple rate, AURORA_XR_LOCKSTEP=0). */
bool aurora_xr_pace(void);
void aurora_xr_camera(int category, const float view[3][4]);

#ifdef __cplusplus
}
#endif

#endif
