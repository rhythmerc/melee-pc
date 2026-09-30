#pragma once

// OpenXR presentation for aurora (AURORA_ENABLE_OPENXR).
//
// The game keeps rendering exactly as it does for a window. Instead of the
// surface texture, the present pass targets a texture shared with a second
// Vulkan device that the OpenXR runtime created (Dawn can neither adopt a
// foreign VkDevice nor expose its own). An XR thread copies each finished
// frame into a quad-layer swapchain -- a virtual screen floating in front of
// the player -- composited over passthrough where the runtime supports
// XR_FB_passthrough.
//
// Design notes and measurements: xr-toolchain/prototypes/dawn-xr-bridge.
//
// Threading: begin_frame/end_frame run on the render worker (the thread that
// owns Dawn). Every other OpenXR and bridge-device call happens on the XR
// thread. Only plain file descriptors cross between them.

#include <webgpu/webgpu_cpp.h>

#include <cstdint>
#include <vector>

namespace aurora::xr {

// XR presentation compiled in and requested (AURORA_XR=1, or on by default in
// builds configured with AURORA_XR_DEFAULT_ON). Cheap; safe from any thread.
bool wanted() noexcept;

// Adds the Dawn features the handoff needs when the adapter has them.
void add_required_features(const wgpu::Adapter& adapter, std::vector<wgpu::FeatureName>& features) noexcept;

// True once the session is up and frames go to the headset.
bool active() noexcept;

// Size of the virtual screen's texture. Fixed on the first call from the
// aspect the game is presented at (4:3, or 16:9 in widescreen), with a
// height of AURORA_XR_SCREEN_HEIGHT (default 1080): roughly what a 1.6 m
// screen at 1.5 m covers on a Quest 3. The window's size is irrelevant; an
// immersive Quest app's window spans the whole display panel.
void screen_size(uint32_t contentWidth, uint32_t contentHeight, uint32_t& width, uint32_t& height) noexcept;

// Render worker only. Returns the texture to draw the presented frame into,
// or null when XR is unavailable (the caller falls back to the window
// surface). The first call initializes OpenXR. width/height are the size the
// presented frame is laid out for; the texture is created at that size.
wgpu::Texture begin_frame(uint32_t width, uint32_t height) noexcept;

// Render worker only, after the queue submit that finished the frame
// returned by begin_frame. Hands it to the XR thread.
void end_frame() noexcept;

void shutdown() noexcept;

} // namespace aurora::xr
