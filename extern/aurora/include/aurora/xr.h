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

#ifdef __cplusplus
}
#endif

#endif
