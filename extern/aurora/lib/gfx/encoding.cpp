#include "encoding.hpp"
#ifdef AURORA_ENABLE_OPENXR
#include "xr_replay.hpp"
#endif

#include "frame.hpp"

#include "clear.hpp"
#include "depth_peek.hpp"
#include "pipeline_cache.hpp"
#include "tex_copy_conv.hpp"
#include "tex_palette_conv.hpp"
#include "../gx/gx.hpp"
#include "../gx/pipeline.hpp"
#ifdef AURORA_ENABLE_RMLUI
#include "../rmlui/pipeline.hpp"
#endif
#include "../webgpu/gpu.hpp"
#include "../webgpu/gpu_prof.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <tracy/Tracy.hpp>

namespace aurora::gfx {
#if defined(AURORA_ENABLE_OPENXR) && !defined(__EMSCRIPTEN__)
// Render worker only; set_xr_drop_flat_world.
static bool g_xrDropFlatWorld = false;
// set_xr_first_pass_timing (any thread).
static std::atomic<XrFirstPassTiming> g_xrFirstPassTiming{nullptr};
#endif
using namespace detail;
using webgpu::g_device;
using webgpu::g_queue;

namespace {
constexpr Module Log{"aurora::gfx"};
PipelineRef g_currentPipeline;

void apply_viewport(const wgpu::RenderPassEncoder& pass, const Viewport& vp) {
  const float minDepth = gx::UseReversedZ ? 1.f - vp.zfar : vp.znear;
  const float maxDepth = gx::UseReversedZ ? 1.f - vp.znear : vp.zfar;
  pass.SetViewport(vp.left, vp.top, vp.width, vp.height, minDepth, maxDepth);
}

void apply_scissor(const wgpu::RenderPassEncoder& pass, const ClipRect& sc, const wgpu::Extent3D& size) {
  const auto x = std::clamp(static_cast<uint32_t>(sc.x), 0u, size.width);
  const auto y = std::clamp(static_cast<uint32_t>(sc.y), 0u, size.height);
  const auto w = std::clamp(static_cast<uint32_t>(sc.width), 0u, size.width - x);
  const auto h = std::clamp(static_cast<uint32_t>(sc.height), 0u, size.height - y);
  pass.SetScissorRect(x, y, w, h);
}

DrawContext make_draw_context(const RenderPass& passInfo) {
  auto& res = resources();
  return {
      .device = g_device,
      .queue = g_queue,
      .vertexBuffer = res.vertexBuffer,
      .indexBuffer = res.indexBuffer,
      .uniformBuffer = res.uniformBuffer,
      .storageBuffer = res.storageBuffer,
      .layout = passInfo.target_layout(),
  };
}

void render_custom_draw(const CustomDrawCommand& draw, const wgpu::RenderPassEncoder& pass,
                        const RenderPass& passInfo) {
  const auto drawType = find_runtime_draw_type(draw.type);
  if (!drawType) {
    // Unregistered between record and replay; the command is a no-op.
    return;
  }

  const auto context = make_draw_context(passInfo);
  drawType->draw(context, pass, draw.payload.data(), draw.payloadSize, drawType->userdata);
}

void execute_encoder_task(wgpu::CommandEncoder& cmd, FramePacket& frame, const EncoderTask& task) {
  const auto taskType = find_runtime_encoder_task_type(task.type);
  if (!taskType) {
    // Unregistered between record and encode; the task is a no-op.
    return;
  }

  auto& res = resources();
  const EncoderTaskContext context{
      .device = g_device,
      .queue = g_queue,
      .vertexBuffer = res.vertexBuffer,
      .indexBuffer = res.indexBuffer,
      .uniformBuffer = res.uniformBuffer,
      .storageBuffer = res.storageBuffer,
  };
  taskType->callback(context, cmd, task.payload.data(), task.payloadSize, taskType->userdata);
  if (taskType->afterSubmit != nullptr) {
    const auto payload = task.payload;
    const auto payloadSize = task.payloadSize;
    const auto callback = taskType->afterSubmit;
    const auto userdata = taskType->userdata;
    frame.afterSubmitCallbacks.emplace_back([payload, payloadSize, callback, userdata] {
      const EncoderTaskCompletionContext completionContext{
          .device = g_device,
          .queue = g_queue,
      };
      callback(completionContext, payload.data(), payloadSize, userdata);
    });
  }
}

#if defined(AURORA_ENABLE_OPENXR) && !defined(__EMSCRIPTEN__)
namespace {
bool env_flag_gfx(const char* name, bool def) {
  const char* v = std::getenv(name);
  if (!v || !*v)
    return def;
  return !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F');
}

// AURORA_XR_FLAT_LOG=1: every 10 s, the flat passes that kept their world
// and hidden draws because something reads them, and what reads them.
void log_kept_flat_pass(const RenderPass& passInfo) {
  static const bool enabled = std::getenv("AURORA_XR_FLAT_LOG") != nullptr;
  if (!enabled)
    return;
  static uint32_t passes, draws, resolve, color, depth, normal;
  static std::array<std::pair<uint64_t, uint32_t>, 4> rects{}; // resolve rect (w << 32 | h) -> passes
  static auto start = std::chrono::steady_clock::now();
  uint32_t kept = 0;
  for (const auto& cmd : passInfo.commands)
    kept += cmd.type == CommandType::Draw &&
            (cmd.xrCategory == XrCategory::World || cmd.xrCategory == XrCategory::Hidden);
  if (kept != 0) {
    ++passes;
    draws += kept;
    resolve += passInfo.resolveTarget ? 1 : 0;
    if (passInfo.resolveTarget) {
      const uint64_t key = static_cast<uint64_t>(passInfo.resolveRect.width) << 32 | passInfo.resolveRect.height;
      for (auto& r : rects) {
        if (r.second == 0 || r.first == key) {
          r.first = key;
          ++r.second;
          break;
        }
      }
    }
    color += passInfo.snapshotColorDst ? 1 : 0;
    depth += passInfo.snapshotDepthDst ? 1 : 0;
    normal += passInfo.snapshotNormalDst ? 1 : 0;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - start >= std::chrono::seconds(10)) {
    std::string sizes;
    for (const auto& r : rects)
      if (r.second != 0)
        sizes += fmt::format(" {}x{}:{}", r.first >> 32, r.first & 0xFFFFFFFF, r.second);
    Log.info("flat passes kept for a reader: {} ({} world/hidden draws); resolve {} (rects{}), color {}, depth {}, normal {}",
             passes, draws, resolve, sizes, color, depth, normal);
    passes = draws = resolve = color = depth = normal = 0;
    rects = {};
    start = now;
  }
}
} // namespace
#endif

void render_pass(const wgpu::RenderPassEncoder& pass, FramePacket& frame, RenderPass& passInfo) {
  ZoneScoped;
  g_currentPipeline = UINTPTR_MAX;
#if defined(AURORA_ENABLE_OPENXR) && !defined(__EMSCRIPTEN__)
  const bool dropWorld = g_xrDropFlatWorld && !passInfo.has_consumer();
  if (g_xrDropFlatWorld && !dropWorld)
    log_kept_flat_pass(passInfo);
  // A flat pass kept only for an EFB copy needs its world and hidden draws
  // only inside the copied rect (Pokemon Stadium's player zoom copies a
  // 113x80 corner): clip them to it. AURORA_XR_FLAT_CLIP=0 draws them whole.
  static const bool clipKept = env_flag_gfx("AURORA_XR_FLAT_CLIP", true);
  const bool clipWorld = clipKept && g_xrDropFlatWorld && passInfo.resolveTarget && !passInfo.snapshotColorDst &&
                         !passInfo.snapshotDepthDst && !passInfo.snapshotNormalDst;
  bool worldClipped = false;
#endif
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> lastDebugGroupStack;
#endif
  Viewport currentViewport{};
  ClipRect currentScissor{};
  bool hasViewport = false;
  bool hasScissor = false;

  // Bind bind group for the whole pass
  pass.SetBindGroup(0, resources().staticBindGroup);
  pass.SetBindGroup(2, gx::g_emptyTextureBindGroup);
#if defined(AURORA_ENABLE_OPENXR) && !defined(__EMSCRIPTEN__)
  const wgpu::BindGroup& xrGroup = passInfo.xrBindGroup ? passInfo.xrBindGroup : gx::g_xrDisabledBindGroup;
  pass.SetBindGroup(3, xrGroup);
#endif

  for (auto& cmd : passInfo.commands) {
#ifdef AURORA_GFX_DEBUG_GROUPS
    {
      size_t firstDiff = lastDebugGroupStack.size();
      for (size_t i = 0; i < lastDebugGroupStack.size(); ++i) {
        if (i >= cmd.debugGroupStack.size() || cmd.debugGroupStack[i] != lastDebugGroupStack[i]) {
          firstDiff = i;
          break;
        }
      }
      for (size_t i = firstDiff; i < lastDebugGroupStack.size(); ++i) {
        pass.PopDebugGroup();
      }
      for (size_t i = firstDiff; i < cmd.debugGroupStack.size(); ++i) {
        pass.PushDebugGroup(cmd.debugGroupStack[i].c_str());
      }
      lastDebugGroupStack = cmd.debugGroupStack;
    }
#endif
    switch (cmd.type) {
    case CommandType::SetViewport: {
      const auto& vp = cmd.data.setViewport;
      apply_viewport(pass, vp);
      currentViewport = vp;
      hasViewport = true;
    } break;
    case CommandType::SetScissor: {
      const auto& sc = cmd.data.setScissor;
      apply_scissor(pass, sc, passInfo.colorAttachments[SceneColorAttachmentIndex].size);
      currentScissor = sc;
      hasScissor = true;
#if defined(AURORA_ENABLE_OPENXR) && !defined(__EMSCRIPTEN__)
      worldClipped = false; // the next world draw clips the new scissor again
#endif
    } break;
    case CommandType::Draw: {
      auto& draw = cmd.data.draw;
#if defined(AURORA_ENABLE_OPENXR) && !defined(__EMSCRIPTEN__)
      const bool world = cmd.xrCategory == XrCategory::World || cmd.xrCategory == XrCategory::Hidden;
      if (dropWorld && world) {
        break;
      }
      if (clipWorld && world != worldClipped) {
        const auto& size = passInfo.colorAttachments[SceneColorAttachmentIndex].size;
        const ClipRect full{0, 0, static_cast<int32_t>(size.width), static_cast<int32_t>(size.height)};
        const ClipRect sc = hasScissor ? currentScissor : full;
        if (world) {
          const ClipRect& r = passInfo.resolveRect;
          const int32_t x0 = std::max(sc.x, r.x), y0 = std::max(sc.y, r.y);
          const int32_t x1 = std::min(sc.x + sc.width, r.x + r.width), y1 = std::min(sc.y + sc.height, r.y + r.height);
          apply_scissor(pass, {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)}, size);
        } else {
          apply_scissor(pass, sc, size);
        }
        worldClipped = world;
      }
#endif
      if (draw.encoder != nullptr) {
        draw.encoder(draw.payload.data(), pass, passInfo);
      }
    } break;
    case CommandType::CustomDraw: {
      render_custom_draw(cmd.data.customDraw, pass, passInfo);
      g_currentPipeline = UINTPTR_MAX;
      pass.SetBindGroup(0, resources().staticBindGroup);
      pass.SetBindGroup(2, gx::g_emptyTextureBindGroup);
#if defined(AURORA_ENABLE_OPENXR) && !defined(__EMSCRIPTEN__)
      pass.SetBindGroup(3, xrGroup);
#endif
      if (hasViewport) {
        apply_viewport(pass, currentViewport);
      }
      if (hasScissor) {
        apply_scissor(pass, currentScissor, passInfo.colorAttachments[SceneColorAttachmentIndex].size);
      }
    } break;
    case CommandType::DebugMarker: {
#if defined(AURORA_GFX_DEBUG_GROUPS)
      pass.InsertDebugMarker(wgpu::StringView(frame.debugMarkers[cmd.data.debugMarkerIndex]));
#endif
    } break;
    case CommandType::XrMarker:
      break;
    }
  }

#ifdef AURORA_GFX_DEBUG_GROUPS
  for (size_t i = 0; i < lastDebugGroupStack.size(); ++i) {
    pass.PopDebugGroup();
  }
#endif
}

void render(wgpu::CommandEncoder& cmd, FramePacket& frame, RenderPass& passInfo, uint32_t passIndex) {
  ZoneScoped;
  if (!passInfo.sealed) {
    return;
  }

  for (const auto& conv : passInfo.paletteConvs) {
    tex_palette_conv::run(cmd, conv);
  }
  if (passInfo.discardable) {
    // This pass has no effect and can be safely discarded (e.g. an empty EFB segment between two back-to-back pass
    // breaks, or an unresolved offscreen pass).
    return;
  }

  std::array<wgpu::RenderPassColorAttachment, MaxColorAttachments> attachments{};
  for (uint32_t i = 0; i < passInfo.colorAttachmentCount; ++i) {
    const auto& source = passInfo.colorAttachments[i];
    attachments[i] = {
        .view = source.view,
        .resolveTarget = source.resolveView,
        .loadOp = source.loadOp != wgpu::LoadOp::Undefined ? source.loadOp
                                                           : (source.clear ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load),
        .storeOp = source.storeOp,
        .clearValue =
            {
                .r = source.clearValue.x(),
                .g = source.clearValue.y(),
                .b = source.clearValue.z(),
                .a = source.clearValue.w(),
            },
    };
  }
  wgpu::RenderPassDepthStencilAttachment depthStencilAttachment{};
  const wgpu::RenderPassDepthStencilAttachment* depthStencilAttachmentPtr = nullptr;
  if (passInfo.depthStencilView) {
    depthStencilAttachment = {
        .view = passInfo.depthStencilView,
        .depthLoadOp = passInfo.hasDepth ? (passInfo.depthLoadOp != wgpu::LoadOp::Undefined
                                                ? passInfo.depthLoadOp
                                                : (passInfo.clearDepth ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load))
                                         : wgpu::LoadOp::Undefined,
        .depthStoreOp = passInfo.hasDepth ? passInfo.depthStoreOp : wgpu::StoreOp::Undefined,
        .depthClearValue = passInfo.clearDepthValue,
        .stencilLoadOp = passInfo.hasStencil ? passInfo.stencilLoadOp : wgpu::LoadOp::Undefined,
        .stencilStoreOp = passInfo.hasStencil ? passInfo.stencilStoreOp : wgpu::StoreOp::Undefined,
        .stencilClearValue = passInfo.stencilClearValue,
    };
    depthStencilAttachmentPtr = &depthStencilAttachment;
  }
  const auto label = passInfo.label.empty() ? fmt::format("Render pass {}", passIndex)
                                            : fmt::format("{} {}", passInfo.label, passIndex);
  const wgpu::RenderPassDescriptor renderPassDescriptor{
      .label = label.c_str(),
      .colorAttachmentCount = passInfo.colorAttachmentCount,
      .colorAttachments = attachments.data(),
      .depthStencilAttachment = depthStencilAttachmentPtr,
      .timestampWrites = webgpu::gpu_prof::pass_writes(label),
  };
#if defined(AURORA_ENABLE_OPENXR) && !defined(__EMSCRIPTEN__)
  wgpu::RenderPassDescriptor timedDescriptor = renderPassDescriptor;
  if (const auto fn = g_xrFirstPassTiming.load(); fn != nullptr && renderPassDescriptor.timestampWrites == nullptr) {
    if (const auto* writes = fn())
      timedDescriptor.timestampWrites = writes;
  }
  auto pass = cmd.BeginRenderPass(&timedDescriptor);
#else
  auto pass = cmd.BeginRenderPass(&renderPassDescriptor);
#endif
  render_pass(pass, frame, passInfo);
  pass.End();

  if (passInfo.captureDepthSnapshot) {
    depth_peek::encode_frame_snapshot(cmd, passInfo.copySourceDepthView,
                                      passInfo.colorAttachments[SceneColorAttachmentIndex].size, passInfo.msaaSamples);
  }

  if (passInfo.resolveTarget) {
    const auto& dstSize = passInfo.resolveTarget->size;
    const bool needsConversion = tex_copy_conv::needs_conversion(passInfo.resolveFormat);
    const bool needsScaling = dstSize.width != static_cast<uint32_t>(passInfo.resolveRect.width) ||
                              dstSize.height != static_cast<uint32_t>(passInfo.resolveRect.height);
    const bool isDepth = gx::is_depth_format(passInfo.resolveFormat);
    const tex_copy_conv::ConvRequest convReq{
        .fmt = passInfo.resolveFormat,
        .srcView = isDepth ? passInfo.copySourceDepthView : passInfo.copySourceView,
        .uniformRange = passInfo.resolveUniformRange,
        .dst = passInfo.resolveTarget,
        .sampleFilter = needsScaling ? tex_copy_conv::SampleFilter::Linear : tex_copy_conv::SampleFilter::Nearest,
        /* Colour reaches us already resolved; depth never is, so the copy
         * shader reads sample 0 of the multisampled texture instead. */
        .msaaSamples = isDepth ? passInfo.msaaSamples : 1,
    };
    if (isDepth || needsConversion) {
      tex_copy_conv::run(cmd, convReq);
    } else if (needsScaling) {
      tex_copy_conv::blit(cmd, convReq);
    } else {
      const webgpu::gpu_prof::Zone zone{cmd, "EFB copy"};
      const wgpu::TexelCopyTextureInfo src{
          .texture = passInfo.copySourceTexture,
          .origin =
              wgpu::Origin3D{
                  .x = static_cast<uint32_t>(passInfo.resolveRect.x),
                  .y = static_cast<uint32_t>(passInfo.resolveRect.y),
              },
      };
      const wgpu::TexelCopyTextureInfo dst{
          .texture = passInfo.resolveTarget->texture,
      };
      const wgpu::Extent3D size{
          .width = static_cast<uint32_t>(passInfo.resolveRect.width),
          .height = static_cast<uint32_t>(passInfo.resolveRect.height),
          .depthOrArrayLayers = 1,
      };
      cmd.CopyTextureToTexture(&src, &dst, &size);
    }
  }

  if (passInfo.snapshotColorDst) {
    const webgpu::gpu_prof::Zone zone{cmd, "Pass snapshot"};
    const wgpu::TexelCopyTextureInfo src{
        .texture = passInfo.copySourceTexture,
    };
    const wgpu::TexelCopyTextureInfo dst{
        .texture = passInfo.snapshotColorDst,
    };
    const wgpu::Extent3D size{
        .width = passInfo.colorAttachments[SceneColorAttachmentIndex].size.width,
        .height = passInfo.colorAttachments[SceneColorAttachmentIndex].size.height,
        .depthOrArrayLayers = 1,
    };
    cmd.CopyTextureToTexture(&src, &dst, &size);
  }
  if (passInfo.snapshotDepthDst) {
    tex_copy_conv::snapshot_depth(cmd, passInfo.copySourceDepthView, passInfo.msaaSamples, passInfo.snapshotDepthDst);
  }
  if (passInfo.snapshotNormalDst) {
    const webgpu::gpu_prof::Zone zone{cmd, "Normal snapshot"};
    const wgpu::TexelCopyTextureInfo src{
        .texture = passInfo.copySourceNormalTexture,
    };
    const wgpu::TexelCopyTextureInfo dst{
        .texture = passInfo.snapshotNormalDst,
    };
    const wgpu::Extent3D size{
        .width = passInfo.colorAttachments[SceneColorAttachmentIndex].size.width,
        .height = passInfo.colorAttachments[SceneColorAttachmentIndex].size.height,
        .depthOrArrayLayers = 1,
    };
    cmd.CopyTextureToTexture(&src, &dst, &size);
  }
}

#ifdef __EMSCRIPTEN__
// end_frame writes vertex, uniform, index and storage data straight into their
// final buffers (browser_upload_pools in frame.cpp); only textures are staged,
// in a buffer of their own.
constexpr uint64_t TextureUploadStagingOffset = 0;
#else
constexpr uint64_t VertexStagingOffset = 0;
constexpr uint64_t UniformStagingOffset = VertexStagingOffset + VertexBufferSize;
constexpr uint64_t IndexStagingOffset = UniformStagingOffset + UniformBufferSize;
constexpr uint64_t StorageStagingOffset = IndexStagingOffset + IndexBufferSize;
constexpr uint64_t TextureUploadStagingOffset = StorageStagingOffset + StorageBufferSize;
#endif

constexpr uint32_t align_down_copy_offset(uint32_t value) noexcept { return value & ~3u; }

#ifndef __EMSCRIPTEN__
void copy_staging_buffer_range(wgpu::CommandEncoder& cmd, const FramePacket& frame, uint32_t& copied,
                               uint32_t highWater, uint64_t stagingOffset, const wgpu::Buffer& dst) {
  if (highWater <= copied) {
    return;
  }
  const uint32_t copyStart = align_down_copy_offset(copied);
  const uint32_t copyEnd = AURORA_ALIGN(highWater, 4);
  cmd.CopyBufferToBuffer(staging_buffer(frame.stagingBuffer), stagingOffset + copyStart, dst, copyStart,
                         copyEnd - copyStart);
  copied = highWater;
}
#endif

bool needs_staging_copy(const FramePacket& frame, const FrameOp& op) {
  const auto& highWater = op.highWater;
  if (highWater.verts > frame.copied.verts || highWater.uniforms > frame.copied.uniforms ||
      highWater.indices > frame.copied.indices || highWater.storage > frame.copied.storage) {
    return true;
  }
  if constexpr (UseTextureBuffer) {
    return op.textureUploads.size() > frame.copied.textureUploadCount;
  }
  return false;
}

void copy_staging_to_high_water(wgpu::CommandEncoder& cmd, FramePacket& frame, const FrameOp& op) {
  if (!needs_staging_copy(frame, op)) {
    return;
  }
  const webgpu::gpu_prof::Zone zone{cmd, "Staging copies"};
  const auto& highWater = op.highWater;
  auto& res = resources();
#ifdef __EMSCRIPTEN__
  frame.copied.verts = highWater.verts;
  frame.copied.uniforms = highWater.uniforms;
  frame.copied.indices = highWater.indices;
  frame.copied.storage = highWater.storage;
#else
  copy_staging_buffer_range(cmd, frame, frame.copied.verts, highWater.verts, VertexStagingOffset, res.vertexBuffer);
  copy_staging_buffer_range(cmd, frame, frame.copied.uniforms, highWater.uniforms, UniformStagingOffset,
                            res.uniformBuffer);
  copy_staging_buffer_range(cmd, frame, frame.copied.indices, highWater.indices, IndexStagingOffset, res.indexBuffer);
  copy_staging_buffer_range(cmd, frame, frame.copied.storage, highWater.storage, StorageStagingOffset,
                            res.storageBuffer);
#endif

  if constexpr (UseTextureBuffer) {
    for (size_t i = frame.copied.textureUploadCount; i < op.textureUploads.size(); ++i) {
      const auto& item = *op.textureUploads[i];
      const wgpu::TexelCopyBufferInfo buf{
          .layout =
              wgpu::TexelCopyBufferLayout{
                  .offset = item.buffer ? item.layout.offset : item.layout.offset + TextureUploadStagingOffset,
                  .bytesPerRow = AURORA_ALIGN(item.layout.bytesPerRow, 256),
                  .rowsPerImage = item.layout.rowsPerImage,
              },
          .buffer = item.buffer ? item.buffer : staging_buffer(frame.stagingBuffer),
      };
      cmd.CopyBufferToTexture(&buf, &item.tex, &item.size);
    }
    frame.copied.textureUpload = highWater.textureUpload;
    frame.copied.textureUploadCount = op.textureUploads.size();
  }
}
} // namespace

namespace detail {
void encode_op(wgpu::CommandEncoder& cmd, FramePacket& frame, const FrameOp& op) {
  copy_staging_to_high_water(cmd, frame, op);
  switch (op.type) {
  case FrameOpType::RenderPass:
    if (op.renderPass != nullptr) {
      render(cmd, frame, *op.renderPass, op.index);
    }
    break;
  case FrameOpType::TextureCopy:
    if (op.textureCopy != nullptr) {
      const webgpu::gpu_prof::Zone zone{cmd, "Texture copy"};
      cmd.CopyTextureToTexture(&op.textureCopy->src, &op.textureCopy->dst, &op.textureCopy->size);
    }
    break;
  case FrameOpType::EncoderTask:
    if (op.encoderTask != nullptr) {
      execute_encoder_task(cmd, frame, *op.encoderTask);
    }
    break;
  }
}
} // namespace detail

bool bind_pipeline(PipelineRef ref, const wgpu::RenderPassEncoder& pass) {
  if (ref == g_currentPipeline) {
    return true;
  }
  wgpu::RenderPipeline pipeline;
  if (!get_pipeline(ref, pipeline)) {
    return false;
  }
  pass.SetPipeline(pipeline);
  g_currentPipeline = ref;
  return true;
}

#if defined(AURORA_ENABLE_OPENXR) && !defined(__EMSCRIPTEN__)
namespace {
XrFrameHook g_xrFrameHook = nullptr;
bool g_xrMultiviewReplay = false; // render worker only

bool same_formats(const RenderTargetLayout& a, const RenderTargetLayout& b) {
  if (a.colorAttachmentCount != b.colorAttachmentCount || a.depthStencilFormat != b.depthStencilFormat ||
      a.sampleCount != b.sampleCount) {
    return false;
  }
  for (uint32_t i = 0; i < a.colorAttachmentCount; ++i) {
    if (a.colorAttachments[i].format != b.colorAttachments[i].format) {
      return false;
    }
  }
  return true;
}
} // namespace

void set_xr_frame_hook(XrFrameHook hook) noexcept { g_xrFrameHook = hook; }
void set_xr_first_pass_timing(XrFirstPassTiming fn) noexcept { g_xrFirstPassTiming = fn; }
static std::atomic<uint64_t> g_xrGameFrameTick{0};
void set_xr_game_frame_tick(uint64_t displayFrame) noexcept { g_xrGameFrameTick = displayFrame; }
uint64_t xr_game_frame_tick() noexcept { return g_xrGameFrameTick; }
void set_xr_drop_flat_world(bool drop) noexcept { g_xrDropFlatWorld = drop; }
XrFrameHook xr_frame_hook() noexcept { return g_xrFrameHook; }

void encode_xr_replay(const wgpu::CommandEncoder& cmd, FramePacket& frame, XrCategory category,
                      const XrReplayTarget& target) {
  ZoneScoped;
  std::array<wgpu::RenderPassColorAttachment, MaxColorAttachments> attachments{};
  for (uint32_t i = 0; i < target.layout.colorAttachmentCount; ++i) {
    attachments[i] = {
        .view = target.colorViews[i],
        .resolveTarget = target.resolveViews[i],
        .loadOp = wgpu::LoadOp::Clear,
        .storeOp = target.colorStore,
        .clearValue = i == SceneColorAttachmentIndex ? target.clearColor : wgpu::Color{0, 0, 0, 0},
    };
  }
  const wgpu::RenderPassDepthStencilAttachment depth{
      .view = target.depthView,
      .depthLoadOp = wgpu::LoadOp::Clear,
      .depthStoreOp = target.depthStore,
      .depthClearValue = target.clearDepth,
  };
  wgpu::RenderPassMultiview multiview{};
  multiview.viewMask = target.viewMask;
  wgpu::RenderPassRenderAreaRect area{};
  area.size = {target.renderAreaWidth, target.renderAreaHeight};
  const bool partial = target.renderAreaWidth != 0 && target.renderAreaHeight != 0;
  const wgpu::ChainedStruct* chain = nullptr;
  if (partial) {
    area.nextInChain = chain;
    chain = &area;
  }
  if (target.viewMask != 0) {
    multiview.nextInChain = chain;
    chain = &multiview;
  }
  const wgpu::RenderPassDescriptor desc{
      .nextInChain = chain,
      .label = category == XrCategory::World ? "XR eye replay" : "XR HUD replay",
      .colorAttachmentCount = target.layout.colorAttachmentCount,
      .colorAttachments = attachments.data(),
      .depthStencilAttachment = &depth,
      .timestampWrites = target.timestampWrites,
  };
  auto pass = cmd.BeginRenderPass(&desc);
  g_currentPipeline = UINTPTR_MAX;
  g_xrMultiviewReplay = target.viewMask != 0;
  pass.SetBindGroup(0, resources().staticBindGroup);
  pass.SetBindGroup(2, gx::g_emptyTextureBindGroup);

  // Draw encoders only read the scene attachment's size from the pass info.
  RenderPass info;
  info.colorAttachmentCount = target.layout.colorAttachmentCount;
  info.colorAttachments[SceneColorAttachmentIndex].size = target.size;
  info.msaaSamples = target.layout.sampleCount;

  const float w = static_cast<float>(target.size.width);
  const float h = static_cast<float>(target.size.height);
  for (uint32_t v = 0; v < std::max(target.viewCount, 1u); ++v) {
    const auto& view = target.views[v];
    const auto group = [&](uint32_t t) -> const wgpu::BindGroup& {
      const auto& g = t < XrMaxTransforms && view.xrBindGroups[t] ? view.xrBindGroups[t] : view.xrBindGroups[0];
      return g ? g : gx::g_xrDisabledBindGroup;
    };
    uint32_t boundTransform = 0;
    pass.SetBindGroup(3, group(0));
    if (target.fullViewport) {
      const bool whole = view.width <= 0.f || view.height <= 0.f;
      const float vx = whole ? 0.f : view.x, vy = whole ? 0.f : view.y;
      const float vw = whole ? w : view.width, vh = whole ? h : view.height;
      pass.SetViewport(vx, vy, vw, vh, 0.f, 1.f);
      pass.SetScissorRect(static_cast<uint32_t>(vx), static_cast<uint32_t>(vy), static_cast<uint32_t>(vw),
                          static_cast<uint32_t>(vh));
    }
    for (auto& src : frame.renderPasses) {
      if (!src.sealed || src.discardable || !same_formats(src.target_layout(), target.layout)) {
        continue;
      }
      const auto& srcSize = src.colorAttachments[SceneColorAttachmentIndex].size;
      const float sx = srcSize.width != 0 ? w / static_cast<float>(srcSize.width) : 1.f;
      const float sy = srcSize.height != 0 ? h / static_cast<float>(srcSize.height) : 1.f;
      for (auto& c : src.commands) {
        switch (c.type) {
        case CommandType::SetViewport:
          if (!target.fullViewport) {
            auto vp = c.data.setViewport;
            vp.left *= sx;
            vp.top *= sy;
            vp.width *= sx;
            vp.height *= sy;
            apply_viewport(pass, vp);
          }
          break;
        case CommandType::SetScissor:
          if (!target.fullViewport) {
            auto sc = c.data.setScissor;
            sc.x = static_cast<int32_t>(static_cast<float>(sc.x) * sx);
            sc.y = static_cast<int32_t>(static_cast<float>(sc.y) * sy);
            sc.width = static_cast<int32_t>(static_cast<float>(sc.width) * sx);
            sc.height = static_cast<int32_t>(static_cast<float>(sc.height) * sy);
            apply_scissor(pass, sc, target.size);
          }
          break;
        case CommandType::Draw:
          if (c.xrCategory == category && c.data.draw.encoder != nullptr) {
            if (c.xrTransform != boundTransform) {
              boundTransform = c.xrTransform;
              pass.SetBindGroup(3, group(boundTransform));
            }
            c.data.draw.encoder(c.data.draw.payload.data(), pass, info);
            if (target.drawCount != nullptr) {
              ++*target.drawCount;
            }
          }
          break;
        default:
          break; // custom draws (clears, copies), markers
        }
      }
    }
  }
  if (target.finish != nullptr) {
    pass.SetViewport(0.f, 0.f, w, h, 0.f, 1.f);
    pass.SetScissorRect(0, 0, target.size.width, target.size.height);
    target.finish(pass, target.finishUser);
  }
  pass.End();
  g_xrMultiviewReplay = false;
  g_currentPipeline = UINTPTR_MAX;
}

bool xr_multiview_replay() noexcept { return g_xrMultiviewReplay; }
#endif
} // namespace aurora::gfx
