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

/* Where a laser points on the virtual screen's picture (menus), in the
 * game frame's own coordinates: 0..1 across the whole frame, x right, y
 * down. `pressed`: the trigger or a pinch is held, having started on the
 * picture. Returns false when no laser is on the picture (also during a 3D
 * fight, or while the screen is being moved); the outputs are then left
 * untouched. Any thread. */
bool aurora_xr_screen_pointer(float* x, float* y, bool* pressed);

/* Whether a hand is pressing the Back button beside the virtual screen's
 * bar (shown while hands, not controllers, are tracked): B, for menus. Any
 * thread. */
bool aurora_xr_screen_back(void);

/* Draw categories for 3D fights (docs/xr-3d-plan.md). Everything drawn after
 * this call, until the next one, is tagged with the category.
 *   AURORA_XR_MONO   only the normal flat frame (the default every frame)
 *   AURORA_XR_WORLD  fight geometry, re-drawn per eye in 3D. Pass the world
 *                    camera's view matrix (world -> camera, GX Mtx layout);
 *                    NULL keeps the frame's current one.
 *   AURORA_XR_HUD    HUD, re-drawn onto its own flat plane
 *   AURORA_XR_HIDDEN fight geometry left out of 3D. Only in the flat frame,
 *                    and dropped from it with the world while nothing reads
 *                    the pass (an EFB copy, such as a stage's screen feed).
 *                    6: 3 to 5 are the transform and clip markers below.
 * Call from the thread that issues GX commands. No-op unless an XR session
 * is presenting. */
enum { AURORA_XR_MONO = 0, AURORA_XR_WORLD = 1, AURORA_XR_HUD = 2, AURORA_XR_HIDDEN = 6 };

/* Lock-step pacing: when the headset runs at a multiple of 60 Hz, blocks
 * until it is time for the next game frame (every display frame at 60 Hz,
 * every other at 120 Hz) and returns true; the caller then skips its own
 * 60 Hz timer. Returns false at once when there is nothing to pace to (no
 * session, a non-multiple rate, AURORA_XR_LOCKSTEP=0). */
bool aurora_xr_pace(void);

/* Whether the fight on screen is paused. Call every frame the fight camera
 * draws. While paused, the controllers show lasers and the grips grab the
 * arena to move, turn and scale it instead of pressing Z. */
void aurora_xr_set_paused(bool paused);

/* The fight is held before it starts so the player can place the stage:
 * the hands grab the arena as in a pause. 0: not held; 1: held, showing
 * the button legend; 2: held, showing the how-to cards and the legend.
 * Any thread. */
void aurora_xr_set_placing(int mode);

/* Puts the current stage back at its default placement for this session.
 * Any thread. */
void aurora_xr_reset_stage(void);

/* The pictures shown while placing (aurora_xr_set_placing): 0 the how-to
 * cards, 1 the button legend, for Touch controllers; 2 and 3 the same for
 * tracked hands, shown instead while a hand is tracked. `rgba` is width x height straight-alpha
 * RGBA8, copied before returning. Once each, before the first placing. */
void aurora_xr_set_placing_image(int which, int width, int height, const unsigned char* rgba);

/* A looping clip shown over a picture on the cards: 0-2 on the
 * controllers' (image 0), 3-5 on the hands' (image 2).
 * `rgba` is a width x height atlas of `frames` frameW x frameH frames,
 * `cols` to a row, played at `fps` forward, then back. x, y, w, h: where it
 * goes on the cards, in their pixels. Opaque. Copied; once each. */
void aurora_xr_set_placing_clip(int which, int width, int height, const unsigned char* rgba, int frames, int cols,
                                int frameW, int frameH, float fps, int x, int y, int w, int h);

/* Whether the room shows behind the game (passthrough): on for mixed
 * reality, off for full VR, where passthrough is stopped, not just hidden.
 * On by default. Any thread. */
void aurora_xr_set_passthrough(bool on);

/* The stage now playing (any id the game keys its stages by), the game
 * point (world units) that sits at the arena position, for stages whose
 * action happens far from the world origin, and its size against the
 * default arena scale (AURORA_XR_ARENA_SCALE). The arena goes back to where
 * the player last put this stage this session, or else to the default
 * placement at this stage's size. Placing one stage never moves another.
 * Any thread. */
void aurora_xr_set_stage(int stage, float x, float y, float z, float scale);
/* Moves the current stage's center (the game point at the arena position)
 * without touching where the arena is: for stages whose camera roams a
 * course bigger than the arena, which then slides through it. Any thread. */
void aurora_xr_set_stage_center(float x, float y, float z);
/* Zooms the 3D view of the current stage about its center by `zoom`, on top
 * of the arena's scale, leaving where the player put the stage as it is
 * (1: none; aurora_xr_set_stage resets it). For a stretch of a course worth
 * a closer look. Any thread. */
void aurora_xr_set_stage_zoom(float zoom);
/* Keeps the HUD over the 3D view in mixed reality too, for the current
 * stage (aurora_xr_set_stage resets it): for stages whose scenery reaches
 * the HUD's height, which would otherwise hide it. Any thread. */
void aurora_xr_set_hud_on_top(bool on);

/* The height (world units) of the stage's highest floor, so the HUD can
 * float clear of it. NaN: unknown (the HUD keeps a fixed height). Any
 * thread. */
void aurora_xr_set_stage_top(float y);

/* True while an OpenXR session is presenting the game. Any thread. */
bool aurora_xr_active(void);
void aurora_xr_camera(int category, const float view[3][4]);

/* Extra placement for the following AURORA_XR_WORLD draws in the 3D view
 * only: a world-space transform (GX Mtx layout, game units) applied before
 * the arena's, e.g. to pull a far background piece in. NULL = none. Up to
 * three distinct transforms a frame; the rest draw unmoved. Same thread and
 * FIFO ordering as aurora_xr_camera. */
void aurora_xr_world_transform(const float m[3][4]);

/* Clip plane for the following AURORA_XR_WORLD draws in the 3D view only, in
 * game world units: geometry where a*x + b*y + c*z + d < 0 is cut away (e.g.
 * {0, 1, 0, -y0} keeps everything above y0). Applies with or without an
 * aurora_xr_world_transform, before it. NULL = none. Needs multiview;
 * otherwise ignored. */
void aurora_xr_world_clip(const float plane[4]);
/* Same, faded: geometry within `fade` units above the plane dissolves out
 * (ordered dither) instead of ending in a hard edge. */
void aurora_xr_world_clip_soft(const float plane[4], float fade);
/* Two planes at once (both must pass); plane2 may be NULL. */
void aurora_xr_world_clips(const float plane1[4], float fade1, const float plane2[4], float fade2);
/* Up to AURORA_XR_MAX_CLIPS planes (all must pass), each with its fade
 * band; count 0 = none, past the maximum = the first ones. (Named for its
 * first limit, four.) */
enum { AURORA_XR_MAX_CLIPS = 8 };
void aurora_xr_world_clips4(const float planes[][4], const float fades[], int count);
/* The same, with the clipped geometry also dissolved as a whole: opacity 1
 * is solid, lower values dither more of it away (0: none left). With count
 * 0 and opacity below 1, only the dissolve applies. */
void aurora_xr_world_clips4_fade(const float planes[][4], const float fades[], int count, float opacity);

#ifdef __cplusplus
}
#endif

#endif
