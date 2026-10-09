#pragma once

#include "types.hpp"

#include <cstddef>
#include <string>

namespace aurora::gfx::detail {

struct FramePacket;

struct RecordedFrame {
  FramePacket* packet;
  size_t frameSlot;
};

void begin_recording(FramePacket& packet, size_t frameSlot);
RecordedFrame end_recording();
void shutdown_recording();
void increment_merged_draw_count() noexcept;

namespace testing {
void suppress_render_worker(bool suppress) noexcept;
void seed_offscreen_cache(uint32_t width, uint32_t height, wgpu::TextureFormat colorFormat,
                          wgpu::TextureFormat depthFormat);
}

} // namespace aurora::gfx::detail

namespace aurora::gfx {
struct ColorPassDescriptor {
  const char* label = nullptr;
  wgpu::TextureView colorView;
  wgpu::TextureView resolveView;
  wgpu::TextureFormat colorFormat = wgpu::TextureFormat::Undefined;
  wgpu::TextureView depthStencilView;
  wgpu::TextureFormat depthStencilFormat = wgpu::TextureFormat::Undefined;
  wgpu::Extent3D targetSize;
  uint32_t sampleCount = 1;
  wgpu::LoadOp colorLoadOp = wgpu::LoadOp::Clear;
  wgpu::StoreOp colorStoreOp = wgpu::StoreOp::Store;
  wgpu::Color clearColor{0.f, 0.f, 0.f, 0.f};
  bool hasDepth = false;
  wgpu::LoadOp depthLoadOp = wgpu::LoadOp::Undefined;
  wgpu::StoreOp depthStoreOp = wgpu::StoreOp::Undefined;
  float depthClearValue = 0.f;
  bool hasStencil = false;
  wgpu::LoadOp stencilLoadOp = wgpu::LoadOp::Undefined;
  wgpu::StoreOp stencilStoreOp = wgpu::StoreOp::Undefined;
  uint32_t stencilClearValue = 0;
};

void finish();
void begin_color_pass(const ColorPassDescriptor& desc);
void end_color_pass();
void queue_texture_copy(wgpu::TexelCopyTextureInfo src, wgpu::TexelCopyTextureInfo dst, wgpu::Extent3D size);
void begin_offscreen(uint32_t width, uint32_t height);
void end_offscreen();
bool has_normal_attachment() noexcept;
RenderTargetLayout get_render_target_layout() noexcept;
void clear_caches() noexcept;
/* Keep the EFB contents instead of clearing at the start of a frame. Used
   when the host pauses the game: nothing draws, so a cleared EFB would
   present as black. */
void set_preserve_frame_buffer(bool preserve) noexcept;

namespace tex_palette_conv {
struct ConvRequest;
}
void queue_palette_conv(tex_palette_conv::ConvRequest req);

Range push_verts(const uint8_t* data, size_t length, size_t alignment);
template <typename T>
Range push_verts(ArrayRef<T> data, size_t alignment) {
  return push_verts(reinterpret_cast<const uint8_t*>(data.data()), data.size() * sizeof(T), alignment);
}
Range push_indices(const uint8_t* data, size_t length, size_t alignment);
template <typename T>
Range push_indices(ArrayRef<T> data, size_t alignment) {
  return push_indices(reinterpret_cast<const uint8_t*>(data.data()), data.size() * sizeof(T), alignment);
}
Range push_uniform(const uint8_t* data, size_t length);
template <typename T>
Range push_uniform(const T& data) {
  return push_uniform(reinterpret_cast<const uint8_t*>(&data), sizeof(T));
}
Range push_storage(const uint8_t* data, size_t length);
// Into the storage pool at a chosen alignment (0 appends with no padding, so
// a merged draw's range stays contiguous), and the pool's current end.
Range push_storage_aligned(const uint8_t* data, size_t length, size_t alignment);
// Room for `length` bytes in the storage pool, to be written in place through
// `out` before anything else is pushed (not zeroed).
Range map_storage_aligned(size_t length, size_t alignment, uint8_t*& out);
size_t storage_tail() noexcept;
template <typename T>
Range push_storage(ArrayRef<T> data) {
  return push_storage(reinterpret_cast<const uint8_t*>(data.data()), data.size() * sizeof(T));
}
template <typename T>
Range push_storage(const T& data) {
  return push_storage(reinterpret_cast<const uint8_t*>(&data), sizeof(T));
}
Range push_texture_data(const uint8_t* data, uint32_t bytesPerRow, uint32_t rowsPerImage);

template <typename DrawData>
void push_draw_command(DrawData data);
template <typename DrawData>
DrawData* get_last_draw_command();
template <typename PipelineConfig>
PipelineRef pipeline_ref(const PipelineConfig& config);

void resolve_pass_into(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                       Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat = GX_TF_RGBA8);
uint32_t align_uniform(uint32_t value);
Vec2<uint32_t> get_render_target_size() noexcept;
void set_viewport(const Viewport& viewport) noexcept;
void set_scissor(const ClipRect& scissor) noexcept;
void push_debug_group(std::string label);
void insert_debug_marker(std::string label);
// AURORA_ENABLE_OPENXR: tag following commands; view3x4 (world camera only)
// may be null to keep the frame's current view.
void xr_set_category(XrCategory category, const float* view3x4);
// Placement for the following world draws in the 3D view (row-major 3x4,
// game world units, applied before the arena transform); null = none.
void xr_set_world_transform(const float* m3x4);
// Clip plane (game world, 4 floats) then its fade band (game units, 0 = hard)
// for the following world draws; null = none.
void xr_set_world_clip(const float* plane);
// Planes 2 * pair + 1 and 2 * pair + 2 for the current clip (pair 1 to 3;
// same 12-float layout).
void xr_set_world_clip_more(int pair, const float* planes);
// FIFO thread: the current draw is tagged World.
bool xr_recording_world() noexcept;
// FIFO thread: a world clip is set (its draws need the clipping pipelines).
bool xr_soft_clip_active() noexcept;
// FIFO thread: the current world clip as the eye shaders test it, in game
// camera space (planes kept where dot >= 0, against vec4(mv_pos, 1)), with
// each plane's fade band (game units, 0 = hard) and the draw's opacity. Unused
// planes are (0, 0, 0, 1). Null when no clip is set.
struct XrClipCamera {
  float planes[XrMaxClipPlanes][4];
  float fades[XrMaxClipPlanes];
  float opacity;
};
const XrClipCamera* xr_clip_camera() noexcept;
// The layout lib/xr replays world draws into with multiview (null = none),
// published once from the render worker and read by the FIFO thread.
void set_xr_multiview_layout(const RenderTargetLayout& layout) noexcept;
const RenderTargetLayout* xr_multiview_layout() noexcept;
// Render worker: encode_xr_replay is replaying with a view mask.
bool xr_multiview_replay() noexcept;
} // namespace aurora::gfx
