/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <atomic>
#include <filesystem>
#include <string>

namespace launcher {
struct DiscInfo {
    bool supported = false;
    std::string message;
};
enum class VerifyState { Verified, Mismatch, Canceled, Error };
struct Verification {
    VerifyState state = VerifyState::Error;
    std::string message;
};
struct Preferences {
    std::string disc;
    std::string net_name = "PLAYER";
    std::string net_target;  // empty hosts our own connect code
    int net_delay = -1;      // auto; otherwise 0..4 frames
    int net_port = 0;        // operating-system allocated

    bool vsync = true;
    bool fullscreen = false;
    float scale = 1.0f;
#if defined(__ANDROID__)
    float render_scale = 1.0f, volume = 1.0f;
    int msaa = 1, anisotropy = 1;
#else
    float render_scale = 0.0f, volume = 1.0f;
    int msaa = 1, anisotropy = 16;
#endif
    int widescreen = 0;
    int filter_mode = 0;
    int backend = 0;
    bool mute = false, fps = false, reverb = true;
    bool check_updates = true;
    bool custom_textures = true;
    bool unlock_all = false;
    int hud_mode = 0;
    bool frozen_stadium = false;
    bool free_camera = false;
    bool ucf = true;             // UCF 0.84 rules (dashback, shield drop, SDI, tumble, cardinals)
    bool slippi_fixes = true;    // Slippi's other gameplay rules: no wobbling, neutral spawns, freeze-glitch fix
    bool cstick_1p = true;       // the C-stick attacks in the 1P modes too (a common Gecko code)
    float music_volume = 1.0f;
    float sfx_volume = 1.0f;
    bool spatial_audio = true;  // fighters' and items' sounds placed (src/pc/spatial_audio.h)
    bool stage_sounds = true;   // our own stage ambience (src/pc/stage_ambience.h); needs spatial audio
    // Quest: the eyes' resolution in percent of the headset's recommended
    // size (80-130), or 0 for dynamic resolution; and Application SpaceWarp.
    int xr_resolution = 0;
    bool xr_spacewarp = true;
    // Quest: fixed foveated rendering, 0 (off) to 3 (high): the edges of each
    // eye drawn at a lower density (AuroraConfig::xrFoveation).
    int xr_foveation = 0;
    int xr_lighting = 0; // XR: 0 the game's own; 1 Roomtone, the room's light (needs the headset cameras)
    // Quest, mixed reality: the stage's contact shadow on the real surface.
    bool xr_shadow = true;
    bool xr_place_hold = true; // XR: the stage is placed before each fight (off: fights start right away)
    uint64_t install_id = 0;  // random once per install (LAN host election); 0 = not yet
};
DiscInfo inspect_disc(const std::string& path);
Verification verify_disc(
    const std::string& path, std::atomic_bool& cancel, std::atomic_uint& progress);
Preferences load_preferences(const std::filesystem::path& path);
bool save_preferences(
    const std::filesystem::path& path, const Preferences& prefs, std::string& error);
}  // namespace launcher
