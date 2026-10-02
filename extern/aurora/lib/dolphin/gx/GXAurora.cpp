#ifdef AURORA_ENABLE_OPENXR
#include <aurora/xr.h>
#endif
#include <bit>
#include "dolphin/gx/GXAurora.h"

#include <limits>

#include "__gx.h"
#include "gx.hpp"
#include "../../window.hpp"

#include "../../gx/fifo.hpp"

static void GXWriteString(const char* label) {
  auto length = strlen(label);

  if (length > std::numeric_limits<u16>::max()) {
    Log.warn("Debug marker size over u16 max, truncating");
    length = std::numeric_limits<u16>::max();
  }

  GX_WRITE_U16(length);
  GX_WRITE_DATA(label, length);
}

void GXPushDebugGroup(const char* label) {
  GX_WRITE_AURORA(GX_AURORA_DEBUG_GROUP_PUSH);
  GXWriteString(label);
}

void GXPopDebugGroup() { GX_WRITE_AURORA(GX_AURORA_DEBUG_GROUP_POP); }

void GXInsertDebugMarker(const char* label) {
  GX_WRITE_AURORA(GX_AURORA_DEBUG_MARKER_INSERT);
  GXWriteString(label);
}

#ifdef AURORA_ENABLE_OPENXR
// aurora/xr.h (C linkage, declared there). Through the FIFO, so the category lines up exactly with the
// draws around it however the FIFO is processed.
void aurora_xr_camera(int category, const float view[3][4]) {
  GX_WRITE_AURORA(GX_AURORA_XR_CAMERA);
  GX_WRITE_U32(static_cast<u32>(category));
  GX_WRITE_U32(view != nullptr ? 1u : 0u);
  for (int i = 0; i < 12; ++i) {
    GX_WRITE_U32(view != nullptr ? std::bit_cast<u32>(view[i / 4][i % 4]) : 0u);
  }
}

void aurora_xr_world_transform(const float m[3][4]) { aurora_xr_camera(3, m); }

void aurora_xr_world_clip(const float plane[4]) {
  if (plane == nullptr) {
    aurora_xr_camera(4, nullptr);
    return;
  }
  const float m[3][4] = {{plane[0], plane[1], plane[2], plane[3]}, {}, {}};
  aurora_xr_camera(4, m);
}
#endif

void AuroraSetViewportPolicy(AuroraViewportPolicy policy) {
  aurora::gx::set_viewport_policy(policy);
}

void AuroraGetRenderSize(u32* width, u32* height) {
  const auto windowSize = aurora::window::get_window_size();
  if (width != nullptr) {
    *width = windowSize.fb_width;
  }
  if (height != nullptr) {
    *height = windowSize.fb_height;
  }
}

void AuroraSetPresentationAspect(f32 aspect) {
  aurora::gx::set_presentation_aspect(aspect);
}

void AuroraGetWindowSize(u32* width, u32* height) {
  const auto windowSize = aurora::window::get_window_size();
  if (width != nullptr) {
    *width = windowSize.native_fb_width;
  }
  if (height != nullptr) {
    *height = windowSize.native_fb_height;
  }
}

void AuroraGXSync() {
  GXFlush();
  aurora::gx::fifo::drain();
}

void GXSetViewportRender(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
  GX_WRITE_AURORA(GX_AURORA_LOAD_VIEWPORT_RENDER);
  GX_WRITE_F32(left);
  GX_WRITE_F32(top);
  GX_WRITE_F32(wd);
  GX_WRITE_F32(ht);
  GX_WRITE_F32(nearz);
  GX_WRITE_F32(farz);
}

void GXSetScissorRender(u32 left, u32 top, u32 wd, u32 ht) {
  GX_WRITE_AURORA(GX_AURORA_LOAD_SCISSOR_RENDER);
  GX_WRITE_U32(left);
  GX_WRITE_U32(top);
  GX_WRITE_U32(wd);
  GX_WRITE_U32(ht);
}

void GXSetProjectionFull(const void* mtx) {
  const f32* values = reinterpret_cast<const f32*>(mtx);
  GX_WRITE_AURORA(GX_AURORA_LOAD_PROJECTION_FULL);
  for (int i = 0; i < 16; ++i) {
    GX_WRITE_F32(values[i]);
  }
}

void GX2SetPolygonOffset(f32 mFrontOffset, f32 mFrontScale, f32 mBackOffset, f32 mBackScale, f32 mClamp) {
  GX_WRITE_AURORA(GX2_SET_POLYGON_OFFSET);
  GX_WRITE_F32(mFrontOffset);
  GX_WRITE_F32(mFrontScale);
  GX_WRITE_F32(mBackOffset);
  GX_WRITE_F32(mBackScale);
  GX_WRITE_F32(mClamp);
}

void GXCreateFrameBuffer(u32 width, u32 height) {
  GX_WRITE_AURORA(GX_AURORA_BEGIN_OFFSCREEN);
  GX_WRITE_U32(width);
  GX_WRITE_U32(height);
  aurora::gx::fifo::publish();
}

void GXRestoreFrameBuffer() {
  GX_WRITE_AURORA(GX_AURORA_END_OFFSCREEN);
  aurora::gx::fifo::publish();
}
