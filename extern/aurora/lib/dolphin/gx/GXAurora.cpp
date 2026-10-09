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

void aurora_xr_world_clip(const float plane[4]) { aurora_xr_world_clip_soft(plane, 0.f); }

void aurora_xr_world_clip_soft(const float plane[4], float fade) { aurora_xr_world_clips(plane, fade, nullptr, 0.f); }

void aurora_xr_world_clips(const float plane1[4], float fade1, const float plane2[4], float fade2) {
  if (plane1 == nullptr) {
    aurora_xr_world_clips4(nullptr, nullptr, 0);
    return;
  }
  const float planes[2][4] = {{plane1[0], plane1[1], plane1[2], plane1[3]},
                              {plane2 ? plane2[0] : 0.f, plane2 ? plane2[1] : 0.f, plane2 ? plane2[2] : 0.f,
                               plane2 ? plane2[3] : 1.f}};
  const float fades[2] = {fade1, plane2 ? fade2 : 0.f};
  aurora_xr_world_clips4(planes, fades, 2);
}

// A marker per two planes: planes 1-2 (category 4; also clears the rest),
// then 3-4, 5-6 and 7-8 (categories 5, 7 and 8; 6 is AURORA_XR_HIDDEN), as
// many as `count` needs. Rows: plane, plane, fades. 0,0,0,1 always passes.
void aurora_xr_world_clips4(const float planes[][4], const float fades[], int count) {
  aurora_xr_world_clips4_fade(planes, fades, count, 1.f);
}

// Row 2 of the first marker carries the opacity after the two fades.
void aurora_xr_world_clips4_fade(const float planes[][4], const float fades[], int count, float opacity) {
  if ((planes == nullptr || count <= 0) && opacity >= 1.f) {
    aurora_xr_camera(4, nullptr);
    return;
  }
  if (planes == nullptr)
    count = 0;
  if (count > AURORA_XR_MAX_CLIPS)
    count = AURORA_XR_MAX_CLIPS;
  constexpr int kCategory[] = {4, 5, 7, 8};
  for (int half = 0; half < AURORA_XR_MAX_CLIPS / 2 && (half == 0 || half * 2 < count); ++half) {
    float m[3][4] = {{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f, 1.f}, {}};
    for (int k = 0; k < 2; ++k) {
      const int i = half * 2 + k;
      if (i < count) {
        for (int c = 0; c < 4; ++c)
          m[k][c] = planes[i][c];
        m[2][k] = fades[i];
      }
    }
    if (half == 0)
      m[2][2] = opacity;
    aurora_xr_camera(kCategory[half], m);
  }
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
