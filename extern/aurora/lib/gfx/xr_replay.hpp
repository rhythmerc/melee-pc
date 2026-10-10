#pragma once

// Re-drawing a recorded frame's tagged draws into other targets, for the
// OpenXR presenter (lib/xr, AURORA_ENABLE_OPENXR). Draws keep their recorded
// pipelines, buffers and bind groups; only the targets, viewport and GX bind
// group 3 (the per-eye projection) change. Targets must have the scene
// render-target layout (formats, attachment count, sample count) so the
// recorded pipelines stay valid; their size is free.

#include "frame_packet.hpp"

namespace aurora::gfx {

// One replay of the frame's draws within the pass: its GX bind group 3 (the
// projection; null = the game's own), one per world transform index (0 =
// none; a null entry falls back to 0), and, for fullViewport targets, the
// rectangle it draws into (zero size = the whole target).
struct XrReplayView {
  std::array<wgpu::BindGroup, XrMaxTransforms> xrBindGroups{};
  float x = 0.f, y = 0.f, width = 0.f, height = 0.f;
};

struct XrReplayTarget {
  RenderTargetLayout layout; // scene_render_target_layout(), any size
  wgpu::Extent3D size;
  std::array<wgpu::TextureView, MaxColorAttachments> colorViews;
  std::array<wgpu::TextureView, MaxColorAttachments> resolveViews; // MSAA only
  wgpu::TextureView depthView;
  wgpu::Color clearColor{0, 0, 0, 0};
  float clearDepth = 0.f;
  wgpu::StoreOp depthStore = wgpu::StoreOp::Store; // Discard when nothing reads it after
  wgpu::StoreOp colorStore = wgpu::StoreOp::Store; // Discard for MSAA attachments resolved in the pass
  // Dynamic resolution: the pass only touches this top-left rectangle (Dawn
  // RenderPassRenderAreaRect), so tiles outside it cost nothing. 0: all.
  uint32_t renderAreaWidth = 0, renderAreaHeight = 0;
  // World: each view's rectangle, game viewports ignored.
  // HUD: the game's viewports and scissors, scaled to the target.
  bool fullViewport = true;
  // The draws are replayed once per view, all in one render pass (both eyes
  // side by side in one image keep everything in tile memory).
  std::array<XrReplayView, 2> views{};
  uint32_t viewCount = 1;
  // Runs inside the pass after the replays (e.g. coverage-to-alpha draws).
  void (*finish)(const wgpu::RenderPassEncoder& pass, void* user) = nullptr;
  void* finishUser = nullptr;
  const wgpu::PassTimestampWrites* timestampWrites = nullptr; // XR GPU timing
  uint32_t* drawCount = nullptr; // incremented per draw encoded (all views)
  // Multiview (Dawn fork): the pass draws every view in the mask at once, into
  // 2D-array attachments with one layer per view, using each world draw's
  // multiview twin pipeline (gx::DrawData::xrPipeline). Use one view then.
  uint32_t viewMask = 0;
  // The passes to replay (null: all the frame's sealed passes).
  const std::vector<const detail::RenderPass*>* passes = nullptr;
};

// XR early eyes: when a frame's world ends (the first switch from world or
// hidden draws to others in the EFB pass), the pass is split there and the
// render worker, right after encoding the world part, calls the early hook
// with the passes so far. If it encoded the eyes into `cmd` (true), the frame
// submits `cmd` at once, calls `submitted`, and goes on in a new encoder: the
// eyes reach the GPU before the rest of the frame is recorded and translated.
// Frames split only while set_xr_early_eyes is on, which also makes their
// uploads skip the staging buffer (FramePacket::directUploads).
using XrEarlyHook = bool (*)(const wgpu::CommandEncoder& cmd, detail::FramePacket& frame,
                             const detail::XrEarlyEyes& early);
using XrEarlySubmitted = void (*)(detail::FramePacket& frame);
void set_xr_early_hooks(XrEarlyHook hook, XrEarlySubmitted submitted) noexcept;
void set_xr_early_eyes(bool on) noexcept;
bool xr_early_eyes() noexcept;
// Render worker: the early eyes' step, queued by recording.cpp after the
// world part's pass.
void run_xr_early(detail::FramePacket& frame, const detail::XrEarlyEyes& early);

// Render worker only. Encodes the frame's draws tagged `category`, in
// recording order, into `target` as one render pass.
void encode_xr_replay(const wgpu::CommandEncoder& cmd, detail::FramePacket& frame, XrCategory category,
                      const XrReplayTarget& target);

// Called on the render worker once per frame, after every pass of the frame
// is encoded and before the frame's packet is released and the present
// callback runs. Set by lib/xr.
using XrFrameHook = void (*)(const wgpu::CommandEncoder& cmd, detail::FramePacket& frame);

// Render worker. While on, the normal frame leaves out world-tagged draws in
// passes no EFB copy or snapshot reads: nothing shows the flat frame during a
// 3D fight, and the eye replays draw the world themselves. Passes are encoded
// as they are recorded, so lib/xr sets this from the previous frame.
void set_xr_drop_flat_world(bool drop) noexcept;
void set_xr_frame_hook(XrFrameHook hook) noexcept;
// Timestamp writes for the first render pass of each frame (XR whole-frame
// GPU timing); returns null for every later pass of the frame.
using XrFirstPassTiming = const wgpu::PassTimestampWrites* (*)();
void set_xr_first_pass_timing(XrFirstPassTiming fn) noexcept;
// Game thread, after a lock-step pacing tick: the tick's display frame, which
// the frames recorded from now on carry (FramePacket::xrTickFrame).
void set_xr_game_frame_tick(uint64_t displayFrame) noexcept;
uint64_t xr_game_frame_tick() noexcept;
XrFrameHook xr_frame_hook() noexcept;

} // namespace aurora::gfx
