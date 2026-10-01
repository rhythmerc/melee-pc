#pragma once

// Re-drawing a recorded frame's tagged draws into other targets, for the
// OpenXR presenter (lib/xr, AURORA_ENABLE_OPENXR). Draws keep their recorded
// pipelines, buffers and bind groups; only the targets, viewport and GX bind
// group 3 (the per-eye projection) change. Targets must have the scene
// render-target layout (formats, attachment count, sample count) so the
// recorded pipelines stay valid; their size is free.

#include "frame_packet.hpp"

namespace aurora::gfx {

struct XrReplayTarget {
  RenderTargetLayout layout; // scene_render_target_layout(), any size
  wgpu::Extent3D size;
  std::array<wgpu::TextureView, MaxColorAttachments> colorViews;
  std::array<wgpu::TextureView, MaxColorAttachments> resolveViews; // MSAA only
  wgpu::TextureView depthView;
  wgpu::Color clearColor{0, 0, 0, 0};
  float clearDepth = 0.f;
  // World: one viewport covering the whole target, game viewports ignored.
  // HUD: the game's viewports and scissors, scaled to the target.
  bool fullViewport = true;
  wgpu::BindGroup xrBindGroup; // null: the game's projection
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
