#pragma once

// Android systrace sections (Perfetto's atrace with this app enabled):
// marks the render worker's encoding and Dawn submits in a scheduler trace.
// Recorded only while a trace is running; nothing elsewhere.
#ifdef __ANDROID__
#include <android/trace.h>
#endif

namespace aurora::gfx {
struct AtraceScope {
  explicit AtraceScope(const char* name) {
#ifdef __ANDROID__
    ATrace_beginSection(name);
#else
    (void)name;
#endif
  }
  ~AtraceScope() {
#ifdef __ANDROID__
    ATrace_endSection();
#endif
  }
  AtraceScope(const AtraceScope&) = delete;
  AtraceScope& operator=(const AtraceScope&) = delete;
};
} // namespace aurora::gfx
