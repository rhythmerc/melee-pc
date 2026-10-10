#pragma once

#include "resources.hpp"
#include "frame_packet.hpp"

#include <optional>

namespace aurora::gfx::detail {

inline constexpr size_t FrameSlotCount = 2;
#ifdef __EMSCRIPTEN__
// The browser renderer submits inline and yields between VI ticks. One mapped
// staging set bounds GPU work in flight and avoids five 87 MiB allocations.
inline constexpr size_t StagingBufferCount = 1;
#elif defined(__ANDROID__)
// One more than frames in flight, plus one for XR early eyes, which switch a
// frame to a second staging buffer at the world's end (split_staging).
inline constexpr size_t StagingBufferCount = FrameSlotCount + 2; // 4 staging buffers on mobile
#else
inline constexpr size_t StagingBufferCount = FrameSlotCount + 3; // 5 staging buffers on desktop
#endif
inline constexpr uint64_t StagingBufferSize = UniformBufferSize + VertexBufferSize + IndexBufferSize +
                                              StorageBufferSize + (UseTextureBuffer ? TextureUploadSize : 0);

const wgpu::Buffer& staging_buffer(size_t slot);
struct FramePacket;
// XR early eyes. Recording thread: moves the rest of `frame`'s uploads to a
// second mapped staging buffer at the same offsets, so the first can be
// unmapped and its copies submitted while recording goes on; false (nothing
// changed) when none is free right now. Render worker: unmaps the first.
bool split_staging(FramePacket& frame);
void unmap_first_staging(FramePacket& frame);

struct RegisteredDrawType {
  DrawCallback draw = nullptr;
  void* userdata = nullptr;
};

struct RegisteredEncoderTaskType {
  EncoderTaskCallback callback = nullptr;
  void* userdata = nullptr;
  EncoderTaskCompletionCallback afterSubmit = nullptr;
};

std::optional<RegisteredDrawType> find_runtime_draw_type(DrawTypeId id);
std::optional<RegisteredEncoderTaskType> find_runtime_encoder_task_type(EncoderTaskId id);

} // namespace aurora::gfx::detail

namespace aurora::gfx {
void initialize();
void shutdown();
bool begin_frame();
void end_frame(EndFrameCallback callback);
// Game thread: the frame's commands are all issued (before the FIFO drains);
// the next end_frame's packet carries the time (XR pacing probe).
void mark_game_frame_done() noexcept;
uint32_t current_frame() noexcept;
void after_submit() noexcept;
void gpu_synchronize();
void after_present() noexcept;
float calculate_fps() noexcept;
} // namespace aurora::gfx
