/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pointing at the menus with the headset's lasers (XR build).
 *
 * A laser on the virtual screen's picture is a pointer (aurora_xr_screen_
 * pointer). Menus that follow it move their highlight, or their cursor, to
 * what it points at, and a click (trigger or pinch) presses A on port 1
 * while it is held. The Back button beside the screen's bar, shown for
 * tracked hands, presses B the same way (aurora_xr_screen_back). A click on a following menu presses A only when it
 * starts over something to select; on any other menu it always does.
 *
 * The pointer leads until the pad steers (a stick or the D-pad), then the
 * pad does until the pointer moves away from where it was or clicks, so
 * a laser left resting on the screen never fights the stick.
 *
 * Only the A press goes through the pad, so what a menu does with it is
 * the pad's input as always; the highlight a menu moves is menu state that
 * no fight reads. Off in netplay, whose menus run on the exchanged pads.
 * MELEE_POINTER_MOUSE=1 drives it with the mouse in the window instead,
 * for testing on a desktop without a headset, its right button as Back. */
#ifndef PC_XR_POINTER_H
#define PC_XR_POINTER_H

#include <dolphin/pad.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct HSD_CObj;

#ifdef AURORA_ENABLE_OPENXR
/* Once a frame as port 1's pad is put together (keyboard.c), with what the
 * other sources gave it. Returns the buttons the pointer adds (A, B). */
u16 pc_xr_pointer_frame(const PADStatus* pad);
/* The pointer, in the frame's logical pixels (640 x 480, as GameCube
 * viewports and scissors measure it); false while no laser is on the
 * picture. `leads`: it steers the menu now, rather than the pad. */
bool pc_xr_pointer_at(float* x, float* y, bool* leads);
/* A menu following the pointer, each frame pc_xr_pointer_at gives one:
 * whether a click there would land on something it selects. */
void pc_xr_pointer_target(bool over);
/* Where a world point lands in the frame's logical pixels through `cobj`
 * (its widescreen widening included). False behind the camera. */
bool pc_xr_pointer_project(struct HSD_CObj* cobj, const float world[3], float* x, float* y);
/* The world point at logical pixel (x, y) through `cobj`, on the plane
 * z = `z`. False when that plane is edge-on. */
bool pc_xr_pointer_unproject(struct HSD_CObj* cobj, float x, float y, float z, float world[3]);
#else
static inline u16 pc_xr_pointer_frame(const PADStatus* pad) { (void)pad; return 0; }
static inline bool pc_xr_pointer_at(float* x, float* y, bool* leads)
{
    (void)x; (void)y; (void)leads;
    return false;
}
static inline void pc_xr_pointer_target(bool over) { (void)over; }
static inline bool pc_xr_pointer_project(struct HSD_CObj* cobj, const float world[3], float* x, float* y)
{
    (void)cobj; (void)world; (void)x; (void)y;
    return false;
}
static inline bool pc_xr_pointer_unproject(struct HSD_CObj* cobj, float x, float y, float z, float world[3])
{
    (void)cobj; (void)x; (void)y; (void)z; (void)world;
    return false;
}
#endif

#ifdef __cplusplus
}
#endif

#endif
