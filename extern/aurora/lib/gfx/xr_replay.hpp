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
};

// Render worker only. Encodes the frame's draws tagged `category`, in
// recording order, into `target` as one render pass.
void encode_xr_replay(const wgpu::CommandEncoder& cmd, detail::FramePacket& frame, XrCategory category,
                      const XrReplayTarget& target);

// Called on the render worker once per frame, after every pass of the frame
// is encoded and before the frame's packet is released and the present
// callback runs. Set by lib/xr.
using XrFrameHook = void (*)(const wgpu::CommandEncoder& cmd, detail::FramePacket& frame);
void set_xr_frame_hook(XrFrameHook hook) noexcept;
XrFrameHook xr_frame_hook() noexcept;

} // namespace aurora::gfx
