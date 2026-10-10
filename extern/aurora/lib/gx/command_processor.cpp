#include <bit>
#include <cstdlib>
#include <cstring>
#include <string>
#include "command_processor.hpp"

#include "../gfx/depth_peek.hpp"
#include "../gfx/frame_packet.hpp"
#include "../gfx/pipeline_cache.hpp"
#include "../gfx/recording.hpp"
#include "../internal.hpp"
#include "dolphin/gd/GDGeometry.h"
#include "dolphin/gx/GXAurora.h"
#include "gx.hpp"
#include "pipeline.hpp"
#include "regs.hpp"
#include "shader_info.hpp"
#include "texture.hpp"

#include <tracy/Tracy.hpp>
#include <xxhash.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace aurora::gx::fifo {
namespace {
constexpr Module Log{"aurora::gx::fifo"};

u16 prepare_idx_buffer(ByteBuffer& buf, GXPrimitive prim, u16 vtxStart, u16 vtxCount) noexcept {
  u16 numIndices = 0;
  if (prim == GX_QUADS) {
    buf.reserve_extra((vtxCount / 4) * 6 * sizeof(u16));

    for (u16 v = 0; v < vtxCount; v += 4) {
      u16 idx0 = vtxStart + v;
      u16 idx1 = vtxStart + v + 1;
      u16 idx2 = vtxStart + v + 2;
      u16 idx3 = vtxStart + v + 3;

      buf.append(idx0);
      buf.append(idx1);
      buf.append(idx2);
      numIndices += 3;

      buf.append(idx2);
      buf.append(idx3);
      buf.append(idx0);
      numIndices += 3;
    }
  } else if (prim == GX_TRIANGLES) {
    buf.reserve_extra(vtxCount * sizeof(u16));
    for (u16 v = 0; v < vtxCount; ++v) {
      const u16 idx = vtxStart + v;
      buf.append(idx);
      ++numIndices;
    }
  } else if (prim == GX_TRIANGLEFAN) {
    buf.reserve_extra(((u32(vtxCount) - 3) * 3 + 3) * sizeof(u16));
    for (u16 v = 0; v < vtxCount; ++v) {
      const u16 idx = vtxStart + v;
      if (v < 3) {
        buf.append(idx);
        ++numIndices;
        continue;
      }
      buf.append(std::array{vtxStart, static_cast<u16>(idx - 1), idx});
      numIndices += 3;
    }
  } else if (prim == GX_TRIANGLESTRIP) {
    buf.reserve_extra(((static_cast<u32>(vtxCount) - 3) * 3 + 3) * sizeof(u16));
    for (u16 v = 0; v < vtxCount; ++v) {
      const u16 idx = vtxStart + v;
      if (v < 3) {
        buf.append(idx);
        ++numIndices;
        continue;
      }
      if ((v & 1) == 0) {
        buf.append(std::array{static_cast<u16>(idx - 2), static_cast<u16>(idx - 1), idx});
      } else {
        buf.append(std::array{static_cast<u16>(idx - 1), static_cast<u16>(idx - 2), idx});
      }
      numIndices += 3;
    }
  } else if (prim == GX_LINES || prim == GX_LINESTRIP || prim == GX_POINTS) {
    buf.reserve_extra(6 * sizeof(u16));
    buf.append<u16>(0);
    buf.append<u16>(1);
    buf.append<u16>(3);
    buf.append<u16>(3);
    buf.append<u16>(2);
    buf.append<u16>(0);
    numIndices = 6;
  } else
    UNLIKELY FATAL("unsupported primitive type {}", static_cast<u32>(prim));
  return numIndices;
}

// GX FIFO opcodes - use CP_ prefix to avoid clashing with GXCommandList.h macros
constexpr u8 CP_CMD_NOP = GX_NOP;
constexpr u8 CP_CMD_LOAD_CP_REG = GX_LOAD_CP_REG;
constexpr u8 CP_CMD_LOAD_XF_REG = GX_LOAD_XF_REG;
constexpr u8 CP_CMD_LOAD_INDX_A = GX_LOAD_INDX_A;
constexpr u8 CP_CMD_LOAD_INDX_B = GX_LOAD_INDX_B;
constexpr u8 CP_CMD_LOAD_INDX_C = GX_LOAD_INDX_C;
constexpr u8 CP_CMD_LOAD_INDX_D = GX_LOAD_INDX_D;
constexpr u8 CP_CMD_CALL_DL = GX_CMD_CALL_DL;
constexpr u8 CP_CMD_INVAL_VTX = GX_CMD_INVL_VC;
constexpr u8 CP_CMD_LOAD_BP_REG = GX_LOAD_BP_REG & GX_OPCODE_MASK;

// Primitive type mask
constexpr u8 CP_OPCODE_MASK = GX_OPCODE_MASK;
constexpr u8 CP_VAT_MASK = GX_VAT_MASK;

struct FogRangeLutKey {
  std::array<u16, 10> rangeK;
  f32 rangeCenter;
  f32 renderWidth;
  u32 targetWidth;

  bool operator==(const FogRangeLutKey&) const = default;
};

struct FogRangeLutEntry {
  FogRangeLutKey key;
  std::vector<f32> factors;
};

constexpr size_t MaxFogRangeLuts = 32;
std::vector<FogRangeLutEntry> sFogRangeLuts;

struct DrawCache {
  uint64_t targetLayoutKey = 0;
  PipelineConfig config{};
  ShaderInfo shaderInfo{};
  gfx::PipelineRef pipelineRef{};
  // XR multiview twin of pipelineRef, for world draws (and what it was made for).
  gfx::PipelineRef xrPipelineRef{};
  gfx::PipelineRef xrForPipeline{};
  uint64_t xrForLayout = 0;
  // Its dithering twin, for clipped world draws that straddle a clip.
  gfx::PipelineRef xrSoftRef{};
  gfx::PipelineRef xrSoftForPipeline{};
  uint64_t xrSoftForLayout = 0;
  GXBindGroups bindGroups{};
  uint64_t bindGeneration = 0;
  GXVtxFmt fmt = GX_MAX_VTXFMT;
  u8 lineMode = 0;
  bool hasPipeline = false;
  gfx::Range uniformRange{};
  gfx::Range fogRange{};
  FogRangeLutKey fogRangeKey{};
  bool hasFogRange = false;
  GXVtxFmt lastDrawFmt = GX_MAX_VTXFMT;
};
DrawCache sDrawCache;

FogRangeLutKey fog_range_lut_key() noexcept {
  const auto& state = g_gxState.fog;
  const f32 logicalWidth = std::max(g_gxState.logicalViewport.width, 1.f);
  const f32 renderWidth = std::max(g_gxState.renderViewport.width, 1.f);
  return {
      .rangeK = state.rangeK,
      .rangeCenter = ((static_cast<f32>(state.rangeCenter) - g_gxState.logicalViewport.left) / logicalWidth) * 2.f -
                     1.f + (g_gxState.renderViewport.left / renderWidth) * 2.f,
      .renderWidth = renderWidth,
      .targetWidth = gfx::get_render_target_size().x,
  };
}

std::vector<f32> build_fog_range_lut(const FogRangeLutKey& key) {
  std::array<f32, 10> rangeK;
  for (u32 i = 0; i < rangeK.size(); ++i) {
    const u32 source = (i & ~1u) | (1u - (i & 1u));
    rangeK[i] = static_cast<f32>(key.rangeK[source]) / 64.f;
  }

  std::vector<f32> lut(key.targetWidth);
  for (u32 x = 0; x < key.targetWidth; ++x) {
    const f32 screenX = ((static_cast<f32>(x) + 0.5f) / key.renderWidth) * 2.f - 1.f;
    const f32 offset = screenX - key.rangeCenter;
    const f32 rangeIndex = std::clamp(9.f - std::abs(offset) * 9.f, 0.f, 9.f);
    const u32 lower = static_cast<u32>(rangeIndex);
    const u32 upper = std::min(lower + 1, 9u);
    const f32 fraction = rangeIndex - static_cast<f32>(lower);
    const f32 k = std::max(rangeK[lower] * (1.f - fraction) + rangeK[upper] * fraction, 0.000001f);
    lut[x] = std::sqrt(offset * offset + k * k) / k;
  }
  return lut;
}

const std::vector<f32>& resolve_fog_range_lut(const FogRangeLutKey& key) {
  for (const auto& entry : sFogRangeLuts) {
    if (entry.key == key) {
      return entry.factors;
    }
  }
  if (sFogRangeLuts.size() == MaxFogRangeLuts) {
    sFogRangeLuts.erase(sFogRangeLuts.begin());
  }
  sFogRangeLuts.emplace_back(FogRangeLutEntry{key, build_fog_range_lut(key)});
  return sFogRangeLuts.back().factors;
}

gfx::Range push_fog_range_lut(const FogRangeLutKey& key) {
  const auto& lut = resolve_fog_range_lut(key);
  return gfx::push_storage(reinterpret_cast<const u8*>(lut.data()), lut.size() * sizeof(f32));
}

u8 line_mode_for_prim(GXPrimitive prim) noexcept {
  switch (prim) {
  case GX_LINES:
    return 1;
  case GX_LINESTRIP:
    return 2;
  case GX_POINTS:
    return 3;
  default:
    return 0;
  }
}
} // namespace

static uint32_t sMarkerTag = 0;

static void handle_draw(u8 cmd, ByteReader& reader) noexcept;
static void handle_aurora(ByteReader& reader) noexcept;

ProcessResult process(const u8* data, u32 size) noexcept {
  ZoneScoped;
  ByteReader reader{{data, size}};

  while (!reader.empty()) {
    const u8 cmd = reader.read<u8>();
    u8 opcode = cmd & CP_OPCODE_MASK;

    switch (opcode) {
    case CP_CMD_NOP:
      continue;

    case CP_CMD_LOAD_BP_REG: {
      const u32 value = reader.read<u32>();
      handle_bp(value);
      if (reg_get(value, 8, 24) == GX_BP_REG_DRAWDONE) {
        return {static_cast<u32>(reader.offset()), true};
      }
      break;
    }

    case CP_CMD_LOAD_CP_REG: {
      const u8 addr = reader.read<u8>();
      handle_cp(addr, reader.read<u32>());
      break;
    }

    case CP_CMD_LOAD_XF_REG: {
      const u32 header = reader.read<u32>();
      const u32 count = ((header >> 16) & 0xFFFF) + 1;
      const u16 addr = header & 0xFFFF;
      handle_xf(addr, reader.take(count * sizeof(u32)));
      break;
    }

    case CP_CMD_LOAD_INDX_A:
    case CP_CMD_LOAD_INDX_B:
    case CP_CMD_LOAD_INDX_C:
    case CP_CMD_LOAD_INDX_D: {
      ZoneScopedN("LOAD_INDX");
      const u32 arrayType = GX_POS_MTX_ARRAY + (opcode - CP_CMD_LOAD_INDX_A) / 0x08;
      const u16 srcArrayIdx = reader.read<u16>();
      const u16 addrLen = reader.read<u16>();

      const u16 len = (addrLen >> 12) + 1;
      const u16 dstAddr = addrLen & 0x0FFF;
      auto const& array = g_gxState.arrays[arrayType];
      const u32 srcOffset = static_cast<u32>(srcArrayIdx) * array.stride;
      const u32 srcSize = static_cast<u32>(len) * sizeof(u32);
      AURORA_ASSERT(array.data != nullptr, "indexed XF load from unmapped array {}", arrayType);
      AURORA_ASSERT(srcOffset <= array.size && srcSize <= array.size - srcOffset,
                    "indexed XF load outside array {}: offset={}, size={}, array size={}", arrayType, srcOffset,
                    srcSize, array.size);
      auto const* srcData = static_cast<const u8*>(array.data) + srcOffset;
      if (!copy_xf_data(dstAddr, srcData, len, array.le ? std::endian::little : std::endian::big)) {
#ifndef NDEBUG
        Log.debug("Unimplemented indexed XF load (opcode 0x{:02X}, dstAddr=%04x)", opcode, dstAddr);
#endif
      }
      break;
    }

    case CP_CMD_CALL_DL: {
      // Call display list: 8 bytes (address + size)
      Log.warn("Ignoring nested GX_CMD_CALL_DL");
      reader.skip(8);
      break;
    }

    case CP_CMD_INVAL_VTX: {
      for (auto& array : g_gxState.arrays) {
        array.cachedRange = {};
        array.hashed = false;
      }
      g_gxState.dirty |= DirtyImmediates;
      break;
    }

    case GX_AURORA: {
      handle_aurora(reader);
      break;
    }

    // Draw commands: 0x80-0xBF
    case GX_DRAW_QUADS:
    case GX_DRAW_TRIANGLES:
    case GX_DRAW_TRIANGLE_STRIP:
    case GX_DRAW_TRIANGLE_FAN:
    case GX_DRAW_LINES:
    case GX_DRAW_LINE_STRIP:
    case GX_DRAW_POINTS: {
      handle_draw(cmd, reader);
      break;
    }

    default:
      // Check if it's a draw command (0x80-0xBF range)
      if (cmd >= 0x80) {
        handle_draw(cmd, reader);
      } else {
        // Hex dump surrounding bytes for debugging
        {
          const size_t pos = reader.offset();
          size_t dumpStart = (pos > 17) ? pos - 17 : 0;
          size_t dumpEnd = (pos + 16 < size) ? pos + 16 : size;
          std::string hex;
          for (size_t i = dumpStart; i < dumpEnd; i++) {
            if (i == pos - 1)
              hex += fmt::format("[{:02x}]", data[i]);
            else
              hex += fmt::format(" {:02x}", data[i]);
          }
          Log.error("  hex dump (pos {}-{}):{}", dumpStart, dumpEnd - 1, hex);
        }
        FATAL("command_processor: unknown opcode 0x{:02X} at pos {}", cmd, reader.offset() - 1);
      }
      break;
    }
  }
  return {size, false};
}

[[noreturn]] static void handle_draw_overrun(size_t totalVtxBytes, const ByteReader& reader) noexcept {
  // Hex dump around the draw command for debugging
  const size_t pos = reader.offset();
  const size_t size = reader.size();
  const u8* data = reader.data();
  size_t cmdPos = pos - 2 - 1; // opcode byte position (before vtxCount and pos++)
  size_t dumpStart = (cmdPos > 16) ? cmdPos - 16 : 0;
  size_t dumpEnd = (cmdPos + 32 < size) ? cmdPos + 32 : size;
  std::string hex;
  for (size_t i = dumpStart; i < dumpEnd; i++) {
    if (i == cmdPos)
      hex += fmt::format("[{:02x}]", data[i]);
    else
      hex += fmt::format(" {:02x}", data[i]);
  }
  Log.error("  hex dump around draw cmd (pos {}-{}):{}", dumpStart, dumpEnd - 1, hex);
  FATAL("draw vertex data overrun: need {} bytes at pos {}, have {}", totalVtxBytes, pos, reader.remaining());
}

static u32 calc_vtx_size(GXVtxFmt fmt) noexcept {
  u32 vtxSize = 0;
  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto& attrFmt = vtxFmt.attrs[i];
    switch (g_gxState.vtxDesc[i]) {
    case GX_NONE:
      break;
    case GX_DIRECT: {
      const auto attr = static_cast<GXAttr>(i);
      vtxSize += comp_type_size(attr, attrFmt.type) * comp_cnt_count(attr, attrFmt.cnt);
      break;
    }
    case GX_INDEX8:
      vtxSize += i == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3 ? 3 : 1;
      break;
    case GX_INDEX16:
      vtxSize += i == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3 ? 6 : 2;
      break;
    }
  }
  g_gxState.lastVtxFmt = fmt;
  g_gxState.lastVtxSize = vtxSize;
  return vtxSize;
}

// AURORA_POS_DECODE: positions decoded to float3 on the CPU, exactly as the
// shader's fetch_* helpers read them (shader.cpp), for a vertex buffer the
// GPU fetches in hardware. `raw` is the draw's GX vertex stream (big-endian
// indices and direct data); indexed positions come from the bound array.
static float decode_component(const u8* p, u8 compType, float scale, bool le) noexcept {
  const auto u16v = [&] { return le ? u16(p[0] | (p[1] << 8)) : u16((p[0] << 8) | p[1]); };
  switch (compType) {
  case GX_U8:
    return static_cast<float>(p[0]) * scale;
  case GX_S8:
    return static_cast<float>(static_cast<s8>(p[0])) * scale;
  case GX_U16:
    return static_cast<float>(u16v()) * scale;
  case GX_S16:
    return static_cast<float>(static_cast<s16>(u16v())) * scale;
  case GX_F32: {
    u32 v = le ? (u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24))
               : ((u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]));
    return std::bit_cast<float>(v);
  }
  default:
    return 0.f;
  }
}

static float decode_component(const u8* p, u8 compType, u8 frac, bool le) noexcept {
  return decode_component(p, compType, 1.f / static_cast<float>(1u << frac), le);
}

// Where one attribute of one vertex lives (NBT slice `slice`), and its byte
// order: the same address attr_address/attr_load_nbt_slice build in the
// shader. Null when an index runs past its array.
static const u8* decoded_source(const AttrConfig& m, GXAttr attr, u32 slice, const u8* vtx, bool& le,
                                u32 compSize) noexcept {
  const u32 within = attr == GX_VA_NRM ? slice * 3 * compSize : 0;
  if (m.attrType == GX_DIRECT) {
    le = false;
    return vtx + m.offset + within;
  }
  const u32 dlExtra = m.nbt3 ? (m.attrType == GX_INDEX8 ? slice : slice * 2) : 0;
  const u8* ip = vtx + m.offset + dlExtra;
  const u32 idx = m.attrType == GX_INDEX8 ? ip[0] : (u32(ip[0]) << 8) | ip[1];
  const auto& array = g_gxState.arrays[attr];
  const u32 elemSize = attr == GX_VA_CLR0 || attr == GX_VA_CLR1 ? compSize : compSize * std::min<u32>(m.cnt, 3);
  const size_t at = static_cast<size_t>(idx) * m.stride + within;
  if (array.data == nullptr || at + elemSize > array.size) {
    return nullptr;
  }
  le = m.le;
  return static_cast<const u8*>(array.data) + at;
}

static const u8* decoded_source(const AttrConfig& m, GXAttr attr, u32 slice, const u8* vtx, bool& le) noexcept {
  return decoded_source(m, attr, slice, vtx, le, comp_type_size(attr, static_cast<GXCompType>(m.compType)));
}

// fetch_rgb565 .. fetch_rgba8 (shader.cpp).
// Reciprocal multiplies, not divides: per vertex on the FIFO thread.
static void decode_color(const u8* p, u8 compType, bool le, float* out) noexcept {
  const auto u16v = [&] { return le ? u32(p[0] | (p[1] << 8)) : u32((p[0] << 8) | p[1]); };
  switch (compType) {
  case GX_RGB565: {
    const u32 v = u16v();
    out[0] = float((v >> 11) & 0x1F) * (1.f / 31.f), out[1] = float((v >> 5) & 0x3F) * (1.f / 63.f),
    out[2] = float(v & 0x1F) * (1.f / 31.f), out[3] = 1.f;
    break;
  }
  case GX_RGB8:
  case GX_RGBX8:
    out[0] = p[0] * (1.f / 255.f), out[1] = p[1] * (1.f / 255.f), out[2] = p[2] * (1.f / 255.f), out[3] = 1.f;
    break;
  case GX_RGBA4: {
    const u32 v = u16v();
    out[0] = float((v >> 12) & 0xF) * (1.f / 15.f), out[1] = float((v >> 8) & 0xF) * (1.f / 15.f),
    out[2] = float((v >> 4) & 0xF) * (1.f / 15.f), out[3] = float(v & 0xF) * (1.f / 15.f);
    break;
  }
  case GX_RGBA6: {
    const u32 v = le ? u32(p[0] | (p[1] << 8) | (p[2] << 16)) : u32((p[0] << 16) | (p[1] << 8) | p[2]);
    out[0] = float((v >> 18) & 0x3F) * (1.f / 63.f), out[1] = float((v >> 12) & 0x3F) * (1.f / 63.f),
    out[2] = float((v >> 6) & 0x3F) * (1.f / 63.f), out[3] = float(v & 0x3F) * (1.f / 63.f);
    break;
  }
  default: // GX_RGBA8
    out[0] = p[0] * (1.f / 255.f), out[1] = p[1] * (1.f / 255.f), out[2] = p[2] * (1.f / 255.f), out[3] = p[3] * (1.f / 255.f);
    break;
  }
}

// One draw's vertices for the GPU's own vertex fetch: positions only
// (decodedPos, float3), or every attribute interleaved (decodedAll,
// decoded_layout). `raw` is the draw's GX vertex stream. Decoded straight
// into the storage pool, each attribute's formats worked out once per draw:
// this is most of the FIFO thread's time on the Quest (Pokemon Stadium's
// fire form spent 21% of it in memcpy here, and 5% sizing components).
static DecodedLayout decode_layout_for(const ShaderConfig& config) noexcept {
  DecodedLayout layout;
  if (!config.decodedAll || !decoded_layout(config, layout)) {
    layout = {};
    layout.attrs[0] = {GX_VA_POS, 0, DecodedFormat::F32x3, 0};
    layout.count = 1;
    layout.stride = 12;
  }
  return layout;
}

static void decode_vertices(const ShaderConfig& config, const DecodedLayout& layout, const u8* raw, u16 vtxCount,
                            u8* base) noexcept {
  enum : u8 { kU32, kColor, kComps };
  struct Plan {
    const AttrConfig* m;
    GXAttr attr;
    u8 slice, kind, compType;
    float scale;
    u32 compSize, n, words, offset;
  };
  std::array<Plan, std::size(DecodedLayout{}.attrs)> plans;
  for (u8 i = 0; i < layout.count; ++i) {
    const auto& d = layout.attrs[i];
    const auto attr = static_cast<GXAttr>(d.attr);
    const auto& m = config.attrs[attr];
    const u8 kind = d.format == DecodedFormat::U32               ? kU32
                    : attr == GX_VA_CLR0 || attr == GX_VA_CLR1 ? kColor
                                                               : kComps;
    const u32 comps = d.format == DecodedFormat::F32x2 ? 2 : d.format == DecodedFormat::F32x3 ? 3 : 4;
    plans[i] = {&m,
                attr,
                d.slice,
                kind,
                m.compType,
                1.f / static_cast<float>(1u << m.frac),
                comp_type_size(attr, static_cast<GXCompType>(m.compType)),
                std::min<u32>(m.cnt, 3),
                kind == kU32 ? 1u : comps,
                d.offset};
  }
  for (u32 v = 0; v < vtxCount; ++v) {
    const u8* vtx = raw + v * config.vtxStride;
    u8* dst = base + static_cast<size_t>(v) * layout.stride;
    for (u8 i = 0; i < layout.count; ++i) {
      const auto& p = plans[i];
      bool le = false;
      const u8* src = decoded_source(*p.m, p.attr, p.slice, vtx, le, p.compSize);
      // 4-byte words at 4-byte offsets in a 4-aligned range, stored one by
      // one: a memcpy of a size picked at run time is a library call.
      u32* out = reinterpret_cast<u32*>(dst + p.offset);
      if (src == nullptr) {
        // Zeros, as the shader's bounds-checked loads return.
        for (u32 w = 0; w < p.words; ++w)
          out[w] = 0;
        continue;
      }
      if (p.kind == kU32) {
        out[0] = src[0];
        continue;
      }
      float f[4] = {0.f, 0.f, 0.f, 0.f};
      if (p.kind == kColor) {
        decode_color(src, p.compType, le, f);
      } else {
        for (u32 c = 0; c < p.n; ++c) {
          f[c] = decode_component(src + c * p.compSize, p.compType, p.scale, le);
        }
      }
      // Straight-line stores: a loop here is turned back into a memcpy call.
      out[0] = std::bit_cast<u32>(f[0]);
      out[1] = std::bit_cast<u32>(f[1]);
      if (p.words > 2)
        out[2] = std::bit_cast<u32>(f[2]);
      if (p.words > 3)
        out[3] = std::bit_cast<u32>(f[3]);
    }
  }
}

// Decoded vertices kept across frames (AURORA_VTX_CACHE, default on with the
// decode; 0 turns it off). Most of a frame's draws decode the same vertices
// as last frame (the stage, the fighters' meshes: skinning is the GPU's), and
// decoding them was a third of the FIFO thread on the Quest (four-CPU Mute
// City: about 3 of 9.8 ms a frame). The key is the draw's own vertex stream
// (indices, or the data itself), its attribute formats, and the contents of
// every array it indexes, hashed once a frame (AttrArray::contentHash): an
// array the game rewrites gets a new key, so nothing goes stale. A hit is a
// copy into the frame's storage pool. AURORA_VTX_CACHE_CHECK=1 decodes every
// hit anyway and counts mismatches; AURORA_VTX_CACHE_STATS=1 logs hits.
namespace {
struct DecodeKey {
  u64 lo, hi;
  bool operator==(const DecodeKey&) const = default;
};
struct DecodeKeyHash {
  size_t operator()(const DecodeKey& k) const noexcept { return static_cast<size_t>(k.lo); }
};
struct ClipBox {
  u8 mtx; // position matrix (GX_DIRECT matrix indices; else the current one)
  float lo[3], hi[3];
};
struct DecodeEntry {
  DecodeKey key{};
  std::vector<u8> bytes;
  u32 lastFrame = 0;
  std::array<u32, 2> ordinal{}; // its position in DecodeCache::order, by frame parity
  // The draw's model-space bounds per position matrix, for classify_xr_clip.
  std::vector<ClipBox> boxes;
  bool haveBoxes = false;
};
struct DecodeCache {
  std::unordered_map<DecodeKey, DecodeEntry, DecodeKeyHash> entries;
  // Draws come in the same order frame after frame: last frame's entry at a
  // draw's position is tried before the table, whose lookups (a cache miss
  // or two each, thousands a frame) were half the FIFO thread's cost of a
  // hit. Cleared when entries are erased (they point into the table).
  std::vector<DecodeEntry*> order, lastOrder;
  size_t cursor = 0; // where in lastOrder the next draw is expected
  u32 orderFrame = 0;
  u64 predicted = 0, looked = 0; // logged with the hits
  size_t bytes = 0;
  u32 sweptFrame = 0;
  u64 hits = 0, misses = 0, hitBytes = 0, mismatches = 0;
  u32 loggedFrame = 0;
};
DecodeCache sDecodeCache;
// The entry push_decoded_vertices last used, and the draw it was for: the
// clip classification of the same draw follows it (push_gx_draw).
struct LastDecode {
  const u8* raw = nullptr;
  u16 vtxCount = 0;
  u32 frame = 0;
  DecodeEntry* entry = nullptr;
} sLastDecode;
#ifdef __ANDROID__
constexpr size_t kDecodeCacheBytes = 24u << 20;
#else
constexpr size_t kDecodeCacheBytes = 64u << 20;
#endif

bool decode_cache_on() noexcept {
  static const bool on = [] {
    const char* v = std::getenv("AURORA_VTX_CACHE");
    return v == nullptr || *v != '0';
  }();
  return on;
}

u64 array_content_hash(GXAttr attr) noexcept {
  auto& array = g_gxState.arrays[attr];
  if (!array.hashed) {
    array.contentHash = array.data != nullptr ? XXH3_64bits(array.data, array.size) : 0;
    array.hashed = true;
  }
  return array.contentHash;
}

DecodeKey decode_key(const ShaderConfig& config, const u8* raw, u16 vtxCount) noexcept {
  struct Parts {
    u64 raw = 0, attrs = 0;
    std::array<u64, GX_VA_TEX7 - GX_VA_POS + 1> arrays{};
    std::array<u32, GX_VA_TEX7 - GX_VA_POS + 1> sizes{};
    u32 vtxCount = 0;
    u8 vtxStride = 0, decodedAll = 0, decodedPos = 0, pad = 0;
  } parts;
  parts.raw = XXH3_64bits(raw, static_cast<size_t>(vtxCount) * config.vtxStride);
  // The formats change only with the vertex format: hashed when they do.
  static decltype(config.attrs) lastAttrs{};
  static u64 lastAttrsHash = XXH3_64bits(lastAttrs.data(), sizeof(lastAttrs));
  if (std::memcmp(lastAttrs.data(), config.attrs.data(), sizeof(lastAttrs)) != 0) {
    lastAttrs = config.attrs;
    lastAttrsHash = XXH3_64bits(lastAttrs.data(), sizeof(lastAttrs));
  }
  parts.attrs = lastAttrsHash;
  for (int a = GX_VA_POS; a <= GX_VA_TEX7; ++a) {
    const u8 type = config.attrs[a].attrType;
    if (type != GX_INDEX8 && type != GX_INDEX16)
      continue;
    parts.arrays[a - GX_VA_POS] = array_content_hash(static_cast<GXAttr>(a));
    parts.sizes[a - GX_VA_POS] = g_gxState.arrays[a].size;
  }
  parts.vtxCount = vtxCount;
  parts.vtxStride = config.vtxStride;
  parts.decodedAll = config.decodedAll;
  parts.decodedPos = config.decodedPos;
  const XXH128_hash_t h = XXH3_128bits(&parts, sizeof(parts));
  return {h.low64, h.high64};
}

// Every few seconds, and at once past the budget: entries unused for two
// seconds go (a table of stale entries makes every lookup slower), then, if
// still over the budget, everything not used this frame.
void sweep_decode_cache() noexcept {
  auto& c = sDecodeCache;
  const u32 now = g_gxState.frameSerial;
  if (c.sweptFrame == now || (c.bytes <= kDecodeCacheBytes && now - c.sweptFrame < 300))
    return;
  c.sweptFrame = now;
  c.order.clear();
  c.lastOrder.clear();
  sLastDecode = {};
  for (u32 age : {120u, 1u}) {
    for (auto it = c.entries.begin(); it != c.entries.end();) {
      if (now - it->second.lastFrame >= age) {
        c.bytes -= it->second.bytes.size();
        it = c.entries.erase(it);
      } else {
        ++it;
      }
    }
    if (c.bytes <= kDecodeCacheBytes * 3 / 4)
      break;
  }
}
} // namespace

static gfx::Range push_decoded_vertices(const ShaderConfig& config, const u8* raw, u16 vtxCount,
                                        size_t alignment) noexcept {
  const DecodedLayout layout = decode_layout_for(config);
  const size_t size = static_cast<size_t>(vtxCount) * layout.stride;
  u8* base = nullptr;
  if (!decode_cache_on()) {
    const auto range = gfx::map_storage_aligned(size, alignment, base);
    if (base != nullptr)
      decode_vertices(config, layout, raw, vtxCount, base);
    return range;
  }
  auto& c = sDecodeCache;
  const DecodeKey key = decode_key(config, raw, vtxCount);
  const auto range = gfx::map_storage_aligned(size, alignment, base);
  if (base == nullptr)
    return range;
  const u32 now = g_gxState.frameSerial;
  if (c.orderFrame != now) {
    sweep_decode_cache();
    c.orderFrame = now;
    c.lastOrder.swap(c.order);
    c.order.clear();
    c.cursor = 0;
  }
  // Last frame's next draw after the previous match; on a miss the table,
  // and the prediction picks up after where that entry stood last frame, so
  // one draw more or less doesn't throw off every draw after it.
  DecodeEntry* entry = nullptr;
  if (c.cursor < c.lastOrder.size() && c.lastOrder[c.cursor] != nullptr && c.lastOrder[c.cursor]->key == key) {
    entry = c.lastOrder[c.cursor++];
    ++c.predicted;
  } else if (auto it = c.entries.find(key); it != c.entries.end()) {
    entry = &it->second;
    ++c.looked;
    const u32 was = entry->ordinal[(now - 1) & 1];
    if (entry->lastFrame + 1 == now && was < c.lastOrder.size() && c.lastOrder[was] == entry)
      c.cursor = was + 1;
  }
  if (entry != nullptr && entry->bytes.size() == size) {
    sLastDecode = {raw, vtxCount, now, entry};
    entry->ordinal[now & 1] = static_cast<u32>(c.order.size());
    c.order.push_back(entry);
    std::memcpy(base, entry->bytes.data(), size);
    entry->lastFrame = now;
    ++c.hits;
    c.hitBytes += size;
    static const bool check = [] {
      const char* v = std::getenv("AURORA_VTX_CACHE_CHECK");
      return v != nullptr && *v == '1';
    }();
    if (check) {
      static std::vector<u8> fresh;
      fresh.resize(size);
      decode_vertices(config, layout, raw, vtxCount, fresh.data());
      if (std::memcmp(fresh.data(), base, size) != 0 && ++c.mismatches <= 8)
        Log.warn("Decoded-vertex cache: stale entry ({} vertices, stride {})", vtxCount, layout.stride);
    }
  } else {
    decode_vertices(config, layout, raw, vtxCount, base);
    ++c.misses;
    auto& e = c.entries[key];
    c.bytes += size - e.bytes.size();
    e.key = key;
    e.bytes.assign(base, base + size);
    e.lastFrame = now;
    e.ordinal[now & 1] = static_cast<u32>(c.order.size());
    c.order.push_back(&e);
    sLastDecode = {raw, vtxCount, now, &e};
  }
  static const bool stats = [] {
    const char* v = std::getenv("AURORA_VTX_CACHE_STATS");
    return v != nullptr && *v == '1';
  }();
  if (stats && now - c.loggedFrame >= 600) {
    c.loggedFrame = now;
    Log.info("Decoded-vertex cache: {} hits ({:.1f} MiB; {} predicted, {} looked up), {} misses, {} mismatches; "
             "{} entries, {:.1f} MiB",
             c.hits, static_cast<double>(c.hitBytes) / (1 << 20), c.predicted, c.looked, c.misses, c.mismatches,
             c.entries.size(), static_cast<double>(c.bytes) / (1 << 20));
    c.hits = c.misses = c.hitBytes = c.predicted = c.looked = 0;
  }
  return range;
}

#if defined(AURORA_ENABLE_OPENXR) && !defined(__EMSCRIPTEN__)
enum class XrClipClass : u8 { Inside, Straddle, Outside };
// Margins (game units) cover float differences from the GPU's transform.
constexpr float kXrClipEps = 1e-2f;

// Where a clipped world draw lies against the current clip, from its
// vertices' distances to each plane (linear across a triangle, so the
// vertices bound every fragment). Inside: past every fade band, drawn
// solid without discard (keeps early depth). Outside: wholly cut by one
// plane, dropped from the eyes. Straddle: needs the dithering pipeline. A
// draw dissolving as a whole (opacity under 1) is never Inside.
// `outside`, when given (vtxCount entries): each vertex's planes it is past
// (bit per plane), for culling a straddling draw's triangles; set only when
// the vertices were read (`*masked`).
static XrClipClass classify_xr_clip(const ShaderConfig& config, const u8* raw, u16 vtxCount, u8* outside = nullptr,
                                    bool* masked = nullptr) noexcept {
  static const bool enabled = [] {
    const char* v = std::getenv("AURORA_XR_CLIP_CLASSIFY");
    return v == nullptr || *v != '0';
  }();
  if (masked != nullptr)
    *masked = false;
  const auto* clip = gfx::xr_clip_camera();
  if (!enabled || clip == nullptr || config.lineMode != 0) {
    return XrClipClass::Straddle;
  }
  const bool dissolving = clip->opacity < 1.f;
  const auto& pos = config.attrs[GX_VA_POS];
  const auto& idx = config.attrs[GX_VA_PNMTXIDX];
  if (pos.attrType == GX_NONE || (idx.attrType != GX_NONE && idx.attrType != GX_DIRECT)) {
    return XrClipClass::Straddle;
  }
  int planes[gfx::XrMaxClipPlanes];
  int planeCount = 0;
  for (int k = 0; k < gfx::XrMaxClipPlanes; ++k) {
    const auto& pl = clip->planes[k];
    if (pl[0] != 0.f || pl[1] != 0.f || pl[2] != 0.f || pl[3] < 0.f) {
      planes[planeCount++] = k;
    }
  }
  if (planeCount == 0) {
    return dissolving ? XrClipClass::Straddle : XrClipClass::Inside;
  }
  // A plane in matrix `mtx`'s model space.
  const auto to_model = [&](u32 mtx, int n, float* q) {
    const f32* m = reinterpret_cast<const f32*>(&g_gxState.pnMtx[mtx].pos);
    const auto& pl = clip->planes[planes[n]];
    for (int j = 0; j < 4; ++j)
      q[j] = pl[0] * m[j] + pl[1] * m[4 + j] + pl[2] * m[8 + j];
    q[3] += pl[3];
  };
  // The draw's bounds, kept with its decoded vertices: a box wholly inside
  // every plane's fade, or wholly past one plane, settles it without reading
  // a vertex. Reading them all every frame was 15% of the FIFO thread (Mute
  // City, four CPUs).
  DecodeEntry* entry = nullptr;
  if (decode_cache_on() && config.decodedAll) {
    // Used once: a later draw's data could land at the same address.
    if (sLastDecode.raw == raw && sLastDecode.vtxCount == vtxCount && sLastDecode.frame == g_gxState.frameSerial) {
      entry = sLastDecode.entry;
      sLastDecode = {};
    } else if (auto it = sDecodeCache.entries.find(decode_key(config, raw, vtxCount)); it != sDecodeCache.entries.end())
      entry = &it->second;
  }
  static const bool checkBoxes = [] {
    const char* v = std::getenv("AURORA_VTX_CACHE_CHECK");
    return v != nullptr && *v == '1';
  }();
  auto boxResult = XrClipClass::Straddle;
  if (entry != nullptr && entry->haveBoxes) {
    float blo[gfx::XrMaxClipPlanes], bhi[gfx::XrMaxClipPlanes];
    std::fill_n(blo, gfx::XrMaxClipPlanes, INFINITY);
    std::fill_n(bhi, gfx::XrMaxClipPlanes, -INFINITY);
    for (const auto& b : entry->boxes) {
      const u32 mtx = idx.attrType == GX_DIRECT ? b.mtx : std::min<u32>(g_gxState.currentPnMtx, MaxPnMtx - 1);
      for (int n = 0; n < planeCount; ++n) {
        float q[4];
        to_model(mtx, n, q);
        float dlo = q[3], dhi = q[3];
        for (int j = 0; j < 3; ++j) {
          const float a = q[j] * b.lo[j], c = q[j] * b.hi[j];
          dlo += std::min(a, c);
          dhi += std::max(a, c);
        }
        blo[n] = std::min(blo[n], dlo);
        bhi[n] = std::max(bhi[n], dhi);
      }
    }
    bool inside = !dissolving, out = false;
    for (int n = 0; n < planeCount; ++n) {
      out |= bhi[n] < -kXrClipEps;
      inside &= blo[n] > clip->fades[planes[n]] + kXrClipEps;
    }
    boxResult = out ? XrClipClass::Outside : inside ? XrClipClass::Inside : XrClipClass::Straddle;
    if (boxResult != XrClipClass::Straddle && !checkBoxes)
      return boxResult;
    // Straddling, or a loose box: the vertices decide.
  }
  ClipBox boxes[MaxPnMtx];
  u8 boxOf[MaxPnMtx];
  u32 boxCount = 0;
  // Each plane in a position matrix's model space, made when first used.
  float model[MaxPnMtx][gfx::XrMaxClipPlanes][4];
  u16 made = 0;
  float lo[gfx::XrMaxClipPlanes], hi[gfx::XrMaxClipPlanes];
  std::fill_n(lo, gfx::XrMaxClipPlanes, INFINITY);
  std::fill_n(hi, gfx::XrMaxClipPlanes, -INFINITY);
  const u32 compSize = comp_type_size(GX_VA_POS, static_cast<GXCompType>(pos.compType));
  const u32 comps = std::min<u32>(pos.cnt, 3);
  for (u32 v = 0; v < vtxCount; ++v) {
    const u8* vtx = raw + v * config.vtxStride;
    const u32 mtx = idx.attrType == GX_DIRECT ? std::min<u32>(vtx[idx.offset] / 3u, MaxPnMtx - 1)
                                              : std::min<u32>(g_gxState.currentPnMtx, MaxPnMtx - 1);
    if ((made & (1u << mtx)) == 0) {
      made |= 1u << mtx;
      for (int n = 0; n < planeCount; ++n)
        to_model(mtx, n, model[mtx][n]);
      boxOf[mtx] = static_cast<u8>(boxCount);
      boxes[boxCount++] = {static_cast<u8>(mtx), {INFINITY, INFINITY, INFINITY}, {-INFINITY, -INFINITY, -INFINITY}};
    }
    bool le = false;
    const u8* src = decoded_source(pos, GX_VA_POS, 0, vtx, le);
    if (src == nullptr) {
      return XrClipClass::Straddle;
    }
    float p[3] = {0.f, 0.f, 0.f};
    for (u32 c = 0; c < comps; ++c)
      p[c] = decode_component(src + c * compSize, pos.compType, pos.frac, le);
    auto& box = boxes[boxOf[mtx]];
    for (int j = 0; j < 3; ++j) {
      box.lo[j] = std::min(box.lo[j], p[j]);
      box.hi[j] = std::max(box.hi[j], p[j]);
    }
    u8 past = 0;
    for (int n = 0; n < planeCount; ++n) {
      const float* q = model[mtx][n];
      const float d = q[0] * p[0] + q[1] * p[1] + q[2] * p[2] + q[3];
      lo[n] = std::min(lo[n], d);
      hi[n] = std::max(hi[n], d);
      past |= d < -kXrClipEps ? u8(1u << n) : u8(0);
    }
    if (outside != nullptr)
      outside[v] = past;
  }
  if (masked != nullptr)
    *masked = outside != nullptr;
  if (entry != nullptr && !entry->haveBoxes) {
    entry->boxes.assign(boxes, boxes + boxCount);
    entry->haveBoxes = true;
  }
  bool inside = !dissolving;
  auto result = XrClipClass::Straddle;
  for (int n = 0; n < planeCount; ++n) {
    if (hi[n] < -kXrClipEps) {
      result = XrClipClass::Outside;
      break;
    }
    inside &= lo[n] > clip->fades[planes[n]] + kXrClipEps;
  }
  if (result != XrClipClass::Outside && inside)
    result = XrClipClass::Inside;
  if (checkBoxes && boxResult != XrClipClass::Straddle && boxResult != result) {
    static u32 warned = 0;
    if (++warned <= 8)
      Log.warn("Clip bounds classed a draw {} but its vertices say {}", static_cast<int>(boxResult),
               static_cast<int>(result));
  }
  return result;
}

// The multiview twin of the current pipeline for `layout`, cached per
// (pipeline, layout).
static gfx::PipelineRef xr_twin(const gfx::RenderTargetLayout& layout, gfx::PipelineRef& ref,
                                gfx::PipelineRef& forPipeline, uint64_t& forLayout) noexcept {
  auto& cache = sDrawCache;
  if (forPipeline != cache.pipelineRef || forLayout != layout.key) {
    ref = gfx::find_pipeline(cache.config, layout);
    forPipeline = cache.pipelineRef;
    forLayout = layout.key;
  }
  return ref;
}

static gfx::PipelineRef xr_soft_twin(const gfx::RenderTargetLayout& mv) noexcept {
  static gfx::RenderTargetLayout soft;
  static uint64_t softFor = 0;
  if (softFor != mv.key) {
    softFor = mv.key;
    soft = mv;
    soft.xrSoftClip = 1;
    gfx::detail::finalize_render_target_layout(soft);
  }
  auto& cache = sDrawCache;
  return xr_twin(soft, cache.xrSoftRef, cache.xrSoftForPipeline, cache.xrSoftForLayout);
}

// The eye pipeline for a world draw's vertices (0: not drawn in the eyes).
// `cls`, when given: how the draw lies against the clip (Inside without
// one); `outside`/`masked` as for classify_xr_clip.
static gfx::PipelineRef xr_world_pipeline(const u8* raw, u16 vtxCount, XrClipClass* clsOut = nullptr,
                                          u8* outside = nullptr, bool* masked = nullptr) noexcept {
  if (clsOut != nullptr)
    *clsOut = XrClipClass::Inside;
  if (masked != nullptr)
    *masked = false;
  if (!gfx::xr_recording_world()) {
    return 0;
  }
  const auto* mv = gfx::xr_multiview_layout();
  if (mv == nullptr) {
    return 0;
  }
  auto& cache = sDrawCache;
  if (gfx::xr_soft_clip_active()) {
    const auto cls = classify_xr_clip(cache.config.shaderConfig, raw, vtxCount, outside, masked);
    if (clsOut != nullptr)
      *clsOut = cls;
    // AURORA_XR_CLIP_STATS=1: how clipped draws were classed, every 20000.
    static const bool stats = std::getenv("AURORA_XR_CLIP_STATS") != nullptr;
    if (stats) {
      static u32 draws[3], verts[3], n;
      ++draws[u8(cls)];
      verts[u8(cls)] += vtxCount;
      if (++n % 20000 == 0) {
        Log.info("XR clip draws (verts): inside {} ({}), straddle {} ({}), outside {} ({})", draws[0], verts[0],
                 draws[1], verts[1], draws[2], verts[2]);
        std::fill_n(draws, 3, 0u), std::fill_n(verts, 3, 0u);
      }
    }
    switch (cls) {
    case XrClipClass::Outside:
      return 0;
    case XrClipClass::Straddle:
      return xr_soft_twin(*mv);
    case XrClipClass::Inside:
      break;
    }
  }
  return xr_twin(*mv, cache.xrPipelineRef, cache.xrForPipeline, cache.xrForLayout);
}
#endif

static void push_gx_draw(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, gfx::Range vertRange, gfx::Range idxRange,
                         u32 numIndices, const u8* raw) noexcept {
  auto& state = g_gxState;
  auto& cache = sDrawCache;

  DrawImmediateData immediates{.vtxStart = vertRange.offset, .currentPnMtx = state.currentPnMtx};
  for (int i = GX_VA_POS; i <= GX_VA_TEX7; ++i) {
    if (state.vtxDesc[i] != GX_INDEX8 && state.vtxDesc[i] != GX_INDEX16) {
      continue;
    }
    auto& array = state.arrays[i];
    if (array.cachedRange.size == 0) {
      array.cachedRange = gfx::push_storage(static_cast<const uint8_t*>(array.data), array.size);
    }
    immediates.arrayStart[i - GX_VA_POS] = array.cachedRange.offset;
  }

  const u8 lineMode = line_mode_for_prim(prim);
  const auto targetLayoutKey = gfx::get_render_target_layout().key;
  const bool pipelineValid = cache.hasPipeline && (state.dirty & DirtyPipeline) == 0 && cache.fmt == fmt &&
                             cache.lineMode == lineMode && cache.targetLayoutKey == targetLayoutKey;
  if (!pipelineValid) {
    const bool hadPipeline = cache.hasPipeline;
    const auto prevSampledTextures = cache.shaderInfo.sampledTextures;
    const auto prevSampledIndTextures = cache.shaderInfo.sampledIndTextures;
    populate_pipeline_config(cache.config, prim, fmt);
    cache.shaderInfo = build_shader_info(cache.config.shaderConfig);
    cache.pipelineRef = gfx::pipeline_ref(cache.config);
    cache.targetLayoutKey = targetLayoutKey;
    cache.fmt = fmt;
    cache.lineMode = lineMode;
    cache.hasPipeline = true;
    state.dirty = (state.dirty & ~DirtyPipeline) | DirtyUniform;
    if (!hadPipeline || prevSampledTextures != cache.shaderInfo.sampledTextures ||
        prevSampledIndTextures != cache.shaderInfo.sampledIndTextures) {
      cache.bindGeneration = 0;
    }
  }

  const bool bindGroupsValid =
      (state.dirty & DirtyTextures) == 0 && cache.bindGeneration == texture::current_bind_generation();
  if (!bindGroupsValid) {
    const auto prevBindGroup = cache.bindGroups.textureBindGroup;
    resolve_sampled_textures(cache.shaderInfo);
    cache.bindGroups = build_bind_groups(cache.shaderInfo);
    // AURORA_LOG_TEV=1: when a draw ends up sampling no texture, report what
    // the TEV stages actually asked for. A stage whose texMapId is
    // GX_TEXMAP_NULL means the game never pointed the stage at a texture.
    if (!cache.bindGroups.textureBindGroup && std::getenv("AURORA_LOG_TEV") != nullptr) {
      static uint64_t n = 0;
      if ((n++ % 4000) == 0) {
        const auto& sc = cache.config.shaderConfig;
        fmt::print(stderr, "no-tex draw #{}: tevStages={}", n, sc.tevStageCount);
        for (uint32_t i = 0; i < sc.tevStageCount && i < 4; ++i) {
          fmt::print(stderr, " [s{} texMap={} texCoord={} coloridx={}]", i, static_cast<int>(sc.tevStages[i].texMapId),
                     static_cast<int>(sc.tevStages[i].texCoordId), static_cast<int>(sc.tevStages[i].channelId));
        }
        fmt::print(stderr, "\n");
      }
    }
    cache.bindGeneration = texture::current_bind_generation();
    state.dirty &= ~DirtyTextures;
    // For texture_size_bias uniform
    if (cache.bindGroups.textureBindGroup != prevBindGroup) {
      state.dirty |= DirtyUniform;
    }
  }

  const bool uniformValid = (state.dirty & DirtyUniform) == 0 && cache.uniformRange.size != 0;
  if (!uniformValid) {
    cache.uniformRange = build_uniform(cache.shaderInfo);
    state.dirty &= ~DirtyUniform;
  }
  if (cache.config.shaderConfig.fogRangeEnabled) {
    const auto key = fog_range_lut_key();
    if (!cache.hasFogRange || cache.fogRangeKey != key) {
      cache.fogRange = push_fog_range_lut(key);
      cache.fogRangeKey = key;
      cache.hasFogRange = true;
    }
  }
  immediates.fogRangeBase = cache.fogRange.offset / sizeof(u32);

  state.dirty &= ~DirtyImmediates;

  uint32_t instanceCount = 1;
  if (prim == GX_LINES) {
    instanceCount = vtxCount / 2;
  } else if (prim == GX_LINESTRIP) {
    instanceCount = vtxCount - 1;
  } else if (prim == GX_POINTS) {
    instanceCount = vtxCount;
  }
  // AURORA_LOG_UNTEX_REGS=1: for an untextured 4-vertex quad (the shape of
  // the white-HUD artifact), report the TEV colour registers as they stand
  // AT RECORD TIME. The FIFO is drained on a worker thread, so this state
  // must be read here and not at render time.
  {
    static const bool logRegs = [] {
      const char* v = std::getenv("AURORA_LOG_UNTEX_REGS");
      return v != nullptr && *v != '\0' && *v != '0';
    }();
    if (logRegs && vtxCount == 4 && !cache.shaderInfo.sampledTextures.any()) {
      static uint64_t n = 0;
      // Sample across the WHOLE run, not just the first draws: the opening
      // movie and title issue thousands of these before gameplay starts, so
      // a first-N sample describes the wrong scene entirely.
      ++n;
      static uint64_t n_add = 0;
      // Filter to the ARTIFACT CLASS (additive: dst=GX_BL_ONE) rather than
      // sampling by count. There are ~57000 untextured quads per run and only
      // the additive ones are the white blobs, so a periodic sample almost
      // never lands on one -- which is why every field previously looked
      // faithful: it was describing the wrong draws.
      if (state.blendFacDst == GX_BL_ONE && n_add++ < 12) {
        const auto& c0 = state.colorRegs[0];
        const auto& c1 = state.colorRegs[1];
        const auto& c2 = state.colorRegs[2];
        fmt::print(stderr,
                   "untex quad #{}: stages={} c0=({:.3f},{:.3f},{:.3f},{:.3f}) "
                   "c1=({:.3f},{:.3f},{:.3f},{:.3f}) c2=({:.3f},{:.3f},{:.3f},{:.3f}) "
                   "s0.color a={} b={} c={} d={} s0.alpha a={} b={} c={} d={} "
                   "s0.texMap={} s0.chan={} | chan0 lit={} matSrc={} ambSrc={} "
                   "mat=({:.3f},{:.3f},{:.3f},{:.3f}) amb=({:.3f},{:.3f},{:.3f},{:.3f}) "
                   "vtxClr0={} clr0fmt cnt={} type={} desc={} | blend={} src={} dst={} op={} alphaUpd={} numChans={} | posDesc={} posCnt={} posType={} arrStride={}\n",
                   n, cache.config.shaderConfig.tevStageCount,
                   c0[0], c0[1], c0[2], c0[3], c1[0], c1[1], c1[2], c1[3],
                   c2[0], c2[1], c2[2], c2[3],
                   underlying(cache.config.shaderConfig.tevStages[0].colorPass.a),
                   underlying(cache.config.shaderConfig.tevStages[0].colorPass.b),
                   underlying(cache.config.shaderConfig.tevStages[0].colorPass.c),
                   underlying(cache.config.shaderConfig.tevStages[0].colorPass.d),
                   underlying(cache.config.shaderConfig.tevStages[0].alphaPass.a),
                   underlying(cache.config.shaderConfig.tevStages[0].alphaPass.b),
                   underlying(cache.config.shaderConfig.tevStages[0].alphaPass.c),
                   underlying(cache.config.shaderConfig.tevStages[0].alphaPass.d),
                   underlying(cache.config.shaderConfig.tevStages[0].texMapId),
                   underlying(cache.config.shaderConfig.tevStages[0].channelId),
                   state.colorChannelConfig[0].lightingEnabled,
                   underlying(state.colorChannelConfig[0].matSrc),
                   underlying(state.colorChannelConfig[0].ambSrc),
                   state.colorChannelState[0].matColor[0], state.colorChannelState[0].matColor[1],
                   state.colorChannelState[0].matColor[2], state.colorChannelState[0].matColor[3],
                   state.colorChannelState[0].ambColor[0], state.colorChannelState[0].ambColor[1],
                   state.colorChannelState[0].ambColor[2], state.colorChannelState[0].ambColor[3],
                   state.vtxDesc[GX_VA_CLR0] != GX_NONE,
                   underlying(state.vtxFmts[fmt].attrs[GX_VA_CLR0].cnt),
                   underlying(state.vtxFmts[fmt].attrs[GX_VA_CLR0].type),
                   underlying(state.vtxDesc[GX_VA_CLR0]),
                   underlying(state.blendMode), underlying(state.blendFacSrc),
                   underlying(state.blendFacDst), underlying(state.blendOp),
                   state.alphaUpdate, state.numChans,
                   underlying(state.vtxDesc[GX_VA_POS]),
                   underlying(state.vtxFmts[fmt].attrs[GX_VA_POS].cnt),
                   underlying(state.vtxFmts[fmt].attrs[GX_VA_POS].type),
                   state.arrays[GX_VA_POS].stride);
      }
    }
  }

  cache.lastDrawFmt = fmt;
  const auto& sc = cache.config.shaderConfig;
  const gfx::Range posRange =
      sc.decodedPos || sc.decodedAll ? push_decoded_vertices(sc, raw, vtxCount, 4) : gfx::Range{};
  gfx::PipelineRef xrPipeline = 0;
  gfx::Range xrIdxRange{};
  u32 xrIndexCount = 0;
#if defined(AURORA_ENABLE_OPENXR) && !defined(__EMSCRIPTEN__)
  // Clipped stage parts: only draws that straddle a clip use the dithering
  // variant; the rest keep early depth (no discard) or are dropped.
  {
    static std::vector<u8> outside;
    outside.resize(vtxCount);
    XrClipClass cls;
    bool masked;
    xrPipeline = xr_world_pipeline(raw, vtxCount, &cls, outside.data(), &masked);
    // A straddling draw's triangles wholly past one plane are left out of
    // the eyes (their own index list; the flat frame keeps every one). On
    // the Quest the dithering pipeline's discard costs early depth for the
    // whole draw, so a long strip mostly outside the clip (Brinstar's acid
    // runs the length of the cave) cost ~5 ms of eye pass.
    if (xrPipeline != 0 && cls == XrClipClass::Straddle && masked &&
        (prim == GX_TRIANGLES || prim == GX_TRIANGLESTRIP || prim == GX_TRIANGLEFAN || prim == GX_QUADS)) {
      static ByteBuffer all;
      static std::vector<u16> kept;
      const u32 n = prepare_idx_buffer(all, prim, 0, vtxCount);
      const u16* tri = reinterpret_cast<const u16*>(all.data());
      kept.clear();
      for (u32 i = 0; i + 2 < n; i += 3) {
        if ((outside[tri[i]] & outside[tri[i + 1]] & outside[tri[i + 2]]) == 0) {
          kept.insert(kept.end(), {tri[i], tri[i + 1], tri[i + 2]});
        }
      }
      all.clear();
      if (kept.empty()) {
        xrPipeline = 0;
      } else if (kept.size() < n) {
        xrIdxRange = gfx::push_indices(reinterpret_cast<const u8*>(kept.data()), kept.size() * sizeof(u16), 4);
        xrIndexCount = static_cast<u32>(kept.size());
      }
    }
  }
#endif
  gfx::push_draw_command(DrawData{
      .pipeline = cache.pipelineRef,
      .xrPipeline = xrPipeline,
      .vertRange = vertRange,
      .idxRange = idxRange,
      .posRange = posRange,
      .uniformRange = cache.uniformRange,
      .immediateData = immediates,
#ifdef __EMSCRIPTEN__
      .immediateRange = gfx::push_uniform(immediates),
#endif
      .vtxCount = vtxCount,
      .indexCount = numIndices,
      .instanceCount = instanceCount,
      .bindGroups = cache.bindGroups,
      .dstAlpha = state.dstAlpha,
      .tag = sMarkerTag,
      .xrIdxRange = xrIdxRange,
      .xrIndexCount = xrIndexCount,
  });
}

static void handle_draw_unmerged(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, gfx::Range vertRange,
                                 const u8* raw) noexcept {
  ZoneScoped;
  u32 numIndices = 0;
  gfx::Range idxRange;

  if (prim != GX_TRIANGLES) {
    ZoneScopedN("build idx buffer");
    static ByteBuffer idxBuf;
    numIndices = prepare_idx_buffer(idxBuf, prim, 0, vtxCount);
    idxRange = gfx::push_indices(idxBuf.data(), idxBuf.size(), 4);
    idxBuf.clear();
  }

  push_gx_draw(prim, fmt, vtxCount, vertRange, idxRange, numIndices, raw);
}

static void draw_prim(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, ByteReader& reader) noexcept {
  ZoneScoped;
  u32 vtxSize;
  if (g_gxState.lastVtxFmt == fmt)
    LIKELY { vtxSize = g_gxState.lastVtxSize; }
  else
    UNLIKELY { vtxSize = calc_vtx_size(fmt); }

  u32 totalVtxBytes = vtxCount * vtxSize;
  if (totalVtxBytes > reader.remaining())
    UNLIKELY { handle_draw_overrun(totalVtxBytes, reader); }

  const bool cleanState = g_gxState.dirty == 0 && fmt == sDrawCache.lastDrawFmt && sDrawCache.lineMode == 0 &&
                          prim != GX_LINES && prim != GX_LINESTRIP && prim != GX_POINTS;
  auto* lastDraw = cleanState ? gfx::get_last_draw_command<DrawData>() : nullptr;
  // A draw with decoded positions merges only while its range can grow in
  // place: nothing else may have been pushed to the storage pool since.
  bool canMerge = lastDraw != nullptr && lastDraw->instanceCount == 1 &&
                  (lastDraw->posRange.size == 0 ||
                   gfx::storage_tail() == size_t{lastDraw->posRange.offset} + lastDraw->posRange.size);

  // Push raw vertex data to buffer. Merged draws must remain contiguous with the previous range.
  const auto vertexData = reader.take(totalVtxBytes);
#if defined(AURORA_ENABLE_OPENXR) && !defined(__EMSCRIPTEN__)
  // Clipped world draws merge only with draws of their own class (inside,
  // straddling, outside), so a merged draw never needs the dithering
  // pipeline just because some of it straddles.
  // Straddling draws don't merge: each keeps its own eye index list.
  if (canMerge && gfx::xr_soft_clip_active()) {
    XrClipClass cls;
    canMerge = xr_world_pipeline(vertexData.data(), vtxCount, &cls) == lastDraw->xrPipeline &&
               cls != XrClipClass::Straddle && lastDraw->xrIndexCount == 0;
  }
#endif
  // AURORA_LOG_QUADPOS=1: log the vertex POSITIONS of untextured 4-vertex
  // quads. Sampling these draws by count is useless -- there are tens of
  // thousands per run and only a handful are the white artifact -- so the
  // artifact has to be picked out by WHERE it is on screen.
  {
    static const bool logPos = [] {
      const char* v = std::getenv("AURORA_LOG_QUADPOS");
      return v != nullptr && *v != '\0' && *v != '0';
    }();
    if (logPos && vtxCount == 4 && g_gxState.vtxDesc[GX_VA_POS] == GX_DIRECT &&
        g_gxState.vtxFmts[fmt].attrs[GX_VA_POS].type == GX_F32) {
      const auto cnt = g_gxState.vtxFmts[fmt].attrs[GX_VA_POS].cnt;
      const u32 nComp = (cnt == GX_POS_XYZ) ? 3u : 2u;
      if (vtxSize >= nComp * 4) {
        float px[4], py[4];
        for (u32 v = 0; v < 4; ++v) {
          u32 off = v * vtxSize;
          // Vertex data is big-endian regardless of host.
          auto be32 = [&](u32 o) {
            return (u32(vertexData[o]) << 24) | (u32(vertexData[o + 1]) << 16) |
                   (u32(vertexData[o + 2]) << 8) | u32(vertexData[o + 3]);
          };
          u32 bx = be32(off), by = be32(off + 4);
          std::memcpy(&px[v], &bx, 4);
          std::memcpy(&py[v], &by, 4);
        }
        fmt::print(stderr, "quad blend={} src={} dst={} v=({:.1f},{:.1f}) "
                           "({:.1f},{:.1f}) ({:.1f},{:.1f}) ({:.1f},{:.1f})\n",
                   underlying(g_gxState.blendMode), underlying(g_gxState.blendFacSrc),
                   underlying(g_gxState.blendFacDst),
                   px[0], py[0], px[1], py[1], px[2], py[2], px[3], py[3]);
      }
    }
  }
  gfx::Range vertRange = gfx::push_verts(vertexData.data(), vertexData.size(), canMerge ? 0 : 4);

  // Try to merge with previous draw call
  if (canMerge) {
    u32 numIndices = 0;
    gfx::Range idxRange;
    static ByteBuffer idxBuf;
    const bool hadIndexRange = lastDraw->idxRange.size != 0;
    if (lastDraw->indexCount == 0 && prim != GX_TRIANGLES) {
      // Generate triangle index buffer for previous draw
      lastDraw->indexCount = prepare_idx_buffer(idxBuf, GX_TRIANGLES, 0, lastDraw->vtxCount);
    }
    if (lastDraw->indexCount != 0) {
      numIndices += prepare_idx_buffer(idxBuf, prim, lastDraw->vtxCount, vtxCount);
      idxRange = gfx::push_indices(idxBuf.data(), idxBuf.size(), hadIndexRange ? 0 : 4);
      idxBuf.clear();
    }
    CHECK(lastDraw->vertRange.offset + lastDraw->vertRange.size == vertRange.offset,
          "Non-consecutive vertex ranges ({} < {})", lastDraw->vertRange.offset + lastDraw->vertRange.size,
          vertRange.offset);
    if (hadIndexRange) {
      CHECK(lastDraw->idxRange.offset + lastDraw->idxRange.size == idxRange.offset,
            "Non-consecutive index ranges ({} < {})", lastDraw->idxRange.offset + lastDraw->idxRange.size,
            idxRange.offset);
    }
    lastDraw->vertRange.size += vertRange.size;
    if (lastDraw->posRange.size != 0) {
      lastDraw->posRange.size +=
          push_decoded_vertices(sDrawCache.config.shaderConfig, vertexData.data(), vtxCount, 0).size;
    }
    if (lastDraw->idxRange.size == 0) {
      lastDraw->idxRange = idxRange;
    } else {
      lastDraw->idxRange.size += idxRange.size;
    }
    lastDraw->vtxCount += vtxCount;
    lastDraw->indexCount += numIndices;
    gfx::detail::increment_merged_draw_count();
    return;
  }

  handle_draw_unmerged(prim, fmt, vtxCount, vertRange, vertexData.data());
}

static void handle_draw(u8 cmd, ByteReader& reader) noexcept {
  const auto fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
  const auto prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
  draw_prim(prim, fmt, reader.read<u16>(), reader);
}

void handle_aurora(ByteReader& reader) noexcept {
  ZoneScoped;
  const u16 subCmd = reader.read<u16>();

  if (subCmd == GX_AURORA_LOAD_VIEWPORT_RENDER) {
    const f32 left = reader.read<f32>();
    const f32 top = reader.read<f32>();
    const f32 width = reader.read<f32>();
    const f32 height = reader.read<f32>();
    const f32 nearZ = reader.read<f32>();
    const f32 farZ = reader.read<f32>();
    set_render_viewport({
        .left = left,
        .top = top,
        .width = width,
        .height = height,
        .znear = nearZ,
        .zfar = farZ,
    });
  } else if (subCmd == GX_AURORA_LOAD_SCISSOR_RENDER) {
    const s32 left = reader.read<s32>();
    const s32 top = reader.read<s32>();
    const s32 width = reader.read<s32>();
    const s32 height = reader.read<s32>();
    set_render_scissor({left, top, width, height});
  } else if (subCmd == GX_AURORA_LOAD_PROJECTION_FULL) {
    auto& proj = g_gxState.proj;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        proj[r][c] = reader.read<f32>();
      }
    }
    // Invalidate projection XF regs
    for (u32 reg = 0x20; reg <= 0x26; ++reg) {
      g_gxState.xfRegValid.reset(reg);
    }
    g_gxState.dirty |= DirtyUniform;
  } else if (subCmd >= GX_AURORA_LOAD_ARRAYBASE && subCmd <= (GX_AURORA_LOAD_ARRAYBASE | 0x0f)) {
    const u32 attrIdx = subCmd - GX_AURORA_LOAD_ARRAYBASE + GX_VA_POS;
    const u64 arrayAddr = reader.read<u64>();
    const u32 arraySize = reader.read<u32>();
    const bool le = reader.read<u8>() == 1;

    auto& array = g_gxState.arrays[attrIdx];
    const auto newData = reinterpret_cast<void*>(arrayAddr);
    if (array.data != newData || array.size != arraySize || array.le != le) {
      if (array.le != le) {
        // Endianness is baked into the shader
        g_gxState.dirty |= DirtyPipeline;
      }
      array.data = newData;
      array.size = arraySize;
      array.le = le;
      array.cachedRange = {};
      array.hashed = false;
      g_gxState.dirty |= DirtyImmediates;
    }
  } else if (subCmd == GX_AURORA_LOAD_TEXOBJ) {
    const auto texMapId = reader.read<u8>();
    CHECK(texMapId < MaxTextures, "invalid texture map id {}", texMapId);
    auto& slot = g_gxState.loadedTextures[texMapId];
    const auto newData = reinterpret_cast<const void*>(reader.read<u64>());
    const u32 newWidth = reader.read<u32>();
    const u32 newHeight = reader.read<u32>();
    const auto newFormat = static_cast<GXTexFmt>(reader.read<u32>());
    const auto newTlut = static_cast<GXTlut>(reader.read<u32>());
    u8 newFlags = slot.flags & ~0x80u; // Reset no-cache flag
    if (reader.read<u8>() != 0) {
      newFlags |= 1u;
    } else {
      newFlags &= ~1u;
    }
    const u32 newTexObjId = reader.read<u32>();
    const u32 newTexDataVersion = reader.read<u32>();
    if (slot.data != newData || slot.mWidth != newWidth || slot.mHeight != newHeight ||
        slot.mFormat != static_cast<u32>(newFormat) || slot.tlut != newTlut || slot.flags != newFlags ||
        slot.texObjId != newTexObjId || slot.texDataVersion != newTexDataVersion) {
      slot.data = newData;
      slot.mWidth = newWidth;
      slot.mHeight = newHeight;
      slot.mFormat = newFormat;
      slot.tlut = newTlut;
      slot.flags = newFlags;
      slot.texObjId = newTexObjId;
      slot.texDataVersion = newTexDataVersion;
      g_gxState.dirty |= DirtyTextures;
    }
  } else if (subCmd == GX_AURORA_LOAD_TLUT) {
    const auto idx = reader.read<u8>();
    CHECK(idx < MaxTluts, "invalid tlut slot {}", idx);
    auto& slot = g_gxState.loadedTluts[idx];
    const auto newData = reinterpret_cast<const void*>(reader.read<u64>());
    const auto newFormat = static_cast<GXTlutFmt>(reader.read<u32>());
    const u16 newNumEntries = reader.read<u16>();
    const u32 newTlutObjId = reader.read<u32>();
    const u32 newTlutDataVersion = reader.read<u32>();
    const u8 newFlags = slot.flags & ~0x80u; // Reset no-cache flag
    if (slot.data != newData || slot.format != newFormat || slot.numEntries != newNumEntries ||
        slot.tlutObjId != newTlutObjId || slot.tlutDataVersion != newTlutDataVersion || slot.flags != newFlags) {
      if (slot.tlutObjId != newTlutObjId || slot.tlutDataVersion != newTlutDataVersion) {
        texture::invalidate_bindings();
      }
      slot.data = newData;
      slot.format = newFormat;
      slot.numEntries = newNumEntries;
      slot.tlutObjId = newTlutObjId;
      slot.tlutDataVersion = newTlutDataVersion;
      slot.flags = newFlags;
      g_gxState.dirty |= DirtyTextures;
    }
  } else if (subCmd == GX2_SET_POLYGON_OFFSET) {
    const f32 frontOffset = reader.read<f32>();
    const f32 frontScale = reader.read<f32>();
    const f32 backOffset = reader.read<f32>();
    const f32 backScale = reader.read<f32>();
    const f32 clamp = reader.read<f32>();
    if (g_gxState.frontOffset != frontOffset || g_gxState.frontScale != frontScale ||
        g_gxState.backOffset != backOffset || g_gxState.backScale != backScale || g_gxState.clamp != clamp) {
      g_gxState.frontOffset = frontOffset;
      g_gxState.frontScale = frontScale;
      g_gxState.backOffset = backOffset;
      g_gxState.backScale = backScale;
      g_gxState.clamp = clamp;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_LOAD_COPY_SRC) {
    const s32 left = reader.read<s32>();
    const s32 top = reader.read<s32>();
    const s32 width = reader.read<s32>();
    const s32 height = reader.read<s32>();
    g_gxState.texCopySrc = {left, top, width, height};
  } else if (subCmd == GX_AURORA_LOAD_COPY_DST) {
    g_gxState.texCopyDstWidth = reader.read<u32>();
    g_gxState.texCopyDstHeight = reader.read<u32>();
    g_gxState.texCopyFmt = static_cast<GXTexFmt>(reader.read<u32>());
    reader.skip(1); // mipmap is not implemented, but remains part of the command payload
    g_gxState.texCopyDstWide = true;
  } else if (subCmd == GX_AURORA_LOAD_COPY_DEST) {
    g_gxState.texCopyDest = reinterpret_cast<const void*>(reader.read<u64>());
  } else if (subCmd == GX_AURORA_REQUEST_DEPTH_SNAPSHOT) {
    gfx::depth_peek::request_snapshot();
  } else if (subCmd == GX_AURORA_XR_CAMERA) {
    const u32 category = reader.read<u32>();
    const bool hasView = reader.read<u32>() != 0;
    std::array<float, 12> view{};
    for (auto& v : view) {
      v = std::bit_cast<float>(reader.read<u32>());
    }
    if (category == 3) { // aurora_xr_world_transform
      gfx::xr_set_world_transform(hasView ? view.data() : nullptr);
    } else if (category == 4) { // aurora_xr_world_clips4: planes 1-2 (clears the rest)
      gfx::xr_set_world_clip(hasView ? view.data() : nullptr);
    } else if (category == 5 || category == 7 || category == 8) { // planes 3-4, 5-6, 7-8
      gfx::xr_set_world_clip_more(category == 5 ? 1 : category - 5, view.data());
    } else {
      const auto cat = category <= 2 ? static_cast<gfx::XrCategory>(category)
                       : category == 6 ? gfx::XrCategory::Hidden // AURORA_XR_HIDDEN
                                       : gfx::XrCategory::Mono;
      gfx::xr_set_category(cat, hasView ? view.data() : nullptr);
    }
  } else if (subCmd == GX_AURORA_BEGIN_OFFSCREEN) {
    const u32 width = reader.read<u32>();
    const u32 height = reader.read<u32>();
    gfx::begin_offscreen(width, height);
  } else if (subCmd == GX_AURORA_END_OFFSCREEN) {
    gfx::end_offscreen();
  } else if (subCmd == GX_AURORA_DESTROY_TEXOBJ) {
    evict_texture_object(reader.read<u32>());
  } else if (subCmd == GX_AURORA_DESTROY_TLUT) {
    evict_tlut_object(reader.read<u32>());
  } else if (subCmd == GX_AURORA_INVALIDATE_TEX) {
    invalidate_texture_hashes();
  } else if (subCmd == GX_AURORA_DESTROY_COPY_TEX) {
    evict_copy_texture(reinterpret_cast<const void*>(reader.read<u64>()));
  } else if (subCmd == GX_AURORA_DRAW_SIZED) {
    const u8 cmd = reader.read<u8>();
    const u32 byteLen = reader.read<u32>();
    const GXVtxFmt fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
    const GXPrimitive prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
    if (byteLen != 0) {
      u32 vtxSize;
      if (g_gxState.lastVtxFmt == fmt) {
        vtxSize = g_gxState.lastVtxSize;
      } else {
        vtxSize = calc_vtx_size(fmt);
      }
      AURORA_ASSERT(vtxSize != 0 && byteLen % vtxSize == 0,
                    "GX_AURORA_DRAW_SIZED: {} bytes is not a whole number of size-{} vertices", byteLen, vtxSize);
      u32 vtxCount = byteLen / vtxSize;
      AURORA_ASSERT(vtxCount <= 0xFFFF, "GX_AURORA_DRAW_SIZED: too many vertices ({})", vtxCount);
      draw_prim(prim, fmt, static_cast<u16>(vtxCount), reader);
    }
  } else if (subCmd == GX_AURORA_DRAW_INDEXED) {
    ZoneScopedN("DRAW_INDEXED");
    const u8 cmd = reader.read<u8>();
    const u16 vtxCount = reader.read<u16>();
    const u32 indexCount = reader.read<u32>();
    const GXVtxFmt fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
    const GXPrimitive prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
    AURORA_ASSERT(prim == GX_TRIANGLES, "GX_AURORA_DRAW_INDEXED: primitive must be GX_TRIANGLES, got {}",
                  static_cast<u32>(prim));
    const size_t idxBytes = static_cast<size_t>(indexCount) * sizeof(u16);
    // Index data is always host-endian; push it to the GPU buffer as-is
    const auto indexData = reader.take(idxBytes);
    const gfx::Range idxRange = gfx::push_indices(indexData.data(), indexData.size(), 4);
    u32 vtxSize;
    if (g_gxState.lastVtxFmt == fmt) {
      vtxSize = g_gxState.lastVtxSize;
    } else {
      vtxSize = calc_vtx_size(fmt);
    }
    const u32 totalVtxBytes = vtxCount * vtxSize;
    const auto vertexData = reader.take(totalVtxBytes);
    const gfx::Range vertRange = gfx::push_verts(vertexData.data(), vertexData.size(), 4);
    if (indexCount != 0) {
      push_gx_draw(prim, fmt, vtxCount, vertRange, idxRange, indexCount, vertexData.data());
    }
  } else if (subCmd == GX_AURORA_DEBUG_GROUP_PUSH) {
    auto label = reader.read_string();
    gfx::push_debug_group(std::move(label));
  } else if (subCmd == GX_AURORA_DEBUG_GROUP_POP) {
    pop_debug_group();
  } else if (subCmd == GX_AURORA_DEBUG_MARKER_INSERT) {
    auto label = reader.read_string();
    // A numeric marker is kept as a breadcrumb for the following draws. This
    // runs on the same worker that records draws, so ordering is exact.
    sMarkerTag = static_cast<uint32_t>(std::strtoul(label.c_str(), nullptr, 0));
    gfx::insert_debug_marker(std::move(label));
  }

  else {
    Log.error("Unknown Aurora subcommand: {:04X}", subCmd);
  }
}

void clear_draw_cache() noexcept {
  sDrawCache.bindGeneration = 0;
  sDrawCache.uniformRange = {};
  sDrawCache.fogRange = {};
  sDrawCache.hasFogRange = false;
}

} // namespace aurora::gx::fifo
