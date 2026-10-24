/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Platform hooks the LAN lobby needs from the Android Java side
 * (src/pc/android_compat.cpp). Every one of these is a no-op, or returns
 * NULL, on every other platform, so callers need no #ifdef. */
#ifndef PC_ANDROID_HOOKS_H
#define PC_ANDROID_HOOKS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hold a WifiManager.MulticastLock while LAN discovery runs. Without it the
 * Wi-Fi stack drops every multicast frame not addressed to this device, so
 * the mDNS group is joined, our announces go out, and no answer ever comes
 * back: pc_lan_discovery_unavailable() turns true after 5 s and the lobby
 * stays empty (docs/netcode-plan.md §8).
 *
 * Nesting is counted: the OS lock is taken on the 0 -> 1 acquire and dropped
 * on the 1 -> 0 release, so repeated pc_lan_start()/pc_lan_stop() cycles in
 * one process are safe, as is the atexit(pc_lan_stop) path. An unbalanced
 * release is ignored rather than letting MulticastLock.release() throw.
 * Safe to call from any thread. */
void pc_android_multicast_lock_acquire(void);
void pc_android_multicast_lock_release(void);

/* The user's device name ("Sian's Pixel 7") reduced to a DNS label, or NULL
 * when there is none to be had. gethostname() is no use on Android: stock
 * devices leave the kernel hostname at "localhost", so every instance would
 * announce localhost-<id>._meleepc._udp.local. and claim localhost.local.
 * The result is cached; the buffer is owned by the callee. */
const char* pc_android_device_name(void);

/* Tell the Android touch overlay whether the RmlUi launcher owns the screen.
 * The overlay is the only touch entry point, and its analog-stick capture
 * zone covers the lower-left quarter of the screen, so while the launcher is
 * up it swallowed the taps meant for "Choose disc" -- the launcher is
 * unreachable on a phone with no other input. Inert (all touches pass
 * through, nothing is drawn) while active. Safe to call from any thread. */
void pc_android_set_launcher_active(bool active);

/* Meta Quest hybrid builds (MELEE_XR=1, platforms/android/app/src/xr): the
 * launcher is a 2D panel (MeleeActivity) and the game runs in the immersive
 * MeleeXrActivity, in its own process.
 *
 * pc_android_is_xr_activity: true in the immersive activity's process.
 * pc_android_launch_xr: from the panel, start the immersive activity with
 *   this disc (path or content:// URI) and close the panel. False if it could
 *   not be asked to.
 * pc_android_launch_panel: from the immersive activity, reopen the launcher
 *   panel in the Home environment and close this activity. */
bool pc_android_is_xr_activity(void);
bool pc_android_launch_xr(const char* disc);
void pc_android_launch_panel(void);
/* The passthrough cameras' permission (HEADSET_CAMERA), for Roomtone
 * lighting, from the launcher panel. pc_android_headset_camera_request asks
 * (the system's prompt, unless already granted); _result is then -1 while
 * asking, 0 declined, 1 granted, 2 declined and Android won't ask again.
 * pc_android_open_app_settings opens Xelee's page in the system settings. */
bool pc_android_headset_camera_granted(void);
void pc_android_headset_camera_request(void);
int pc_android_headset_camera_result(void);
void pc_android_open_app_settings(void);
/* The updater (updater.cpp). pc_android_download: fetch url into path,
 * blocking (call from a worker thread); true when it all arrived.
 * pc_android_install_apk: hand the APK at path to the system installer,
 * which asks the player to confirm; false if it could not be asked. */
bool pc_android_download(const char* url, const char* path);
bool pc_android_install_apk(const char* path);

#ifdef __cplusplus
}
#endif

#endif
