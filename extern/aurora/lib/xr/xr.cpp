#include "xr.hpp"

#include <aurora/xr.h>

#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
#include "../gfx/recording.hpp"
#include "../gfx/xr_replay.hpp"
#include "../gx/gx.hpp"

#include <aurora/gfx.hpp>

#include <SDL3/SDL.h>

#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR
#define XR_USE_PLATFORM_ANDROID
#include <android/hardware_buffer.h>
#include <jni.h>
#endif
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
// After vulkan.h: the fork's single-device hooks (github.com/encounter/dawn
// plus the melee-xr patches; see docs/quest-xr.md).
#include <dawn/native/VulkanBackend.h>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <bit>
#include <cmath>
#include <limits>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace aurora::xr {
namespace {
Module Log("aurora::xr");

// ---------------------------------------------------------------- platform
//
// One Vulkan device: the OpenXR runtime creates Dawn's VkInstance and
// VkDevice (XR_KHR_vulkan_enable2, through the Dawn fork's
// SetExternalVulkanHooks), and Dawn renders straight into the runtime's
// swapchain images. Dawn submits on queue 0 of its family; the XR thread and
// the runtime use queue 1. A finished image comes back from Dawn with a
// semaphore (SYNC_FD on Android, OPAQUE_FD on Linux) that the XR thread waits
// on, on its queue, before releasing the image.
#ifdef __ANDROID__
constexpr VkExternalSemaphoreHandleTypeFlagBits kSemHandle = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
constexpr VkSemaphoreImportFlags kSemImportFlags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT; // required for SYNC_FD
constexpr wgpu::SharedFenceType kDawnFenceType = wgpu::SharedFenceType::SyncFD;
using DawnFenceExportInfo = wgpu::SharedFenceSyncFDExportInfo;
#else
constexpr VkExternalSemaphoreHandleTypeFlagBits kSemHandle = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
constexpr VkSemaphoreImportFlags kSemImportFlags = 0;
constexpr wgpu::SharedFenceType kDawnFenceType = wgpu::SharedFenceType::VkSemaphoreOpaqueFD;
using DawnFenceExportInfo = wgpu::SharedFenceVkSemaphoreOpaqueFDExportInfo;
#endif

// ---------------------------------------------------------------- state
//
// Three image streams go from the render worker to the XR thread, each with
// its own swapchain:
//   Screen  the presented frame, on the virtual screen (menus, or any time
//           the game draws no fight)
//   Stereo  3D fights: both eyes side by side, submitted as a projection
//           layer with the head poses they were rendered for
//   Hud     the HUD of a 3D fight, on a quad above the arena
//
// A slot is one swapchain image. The XR thread acquires images ahead of the
// render worker, which draws into the oldest one through Dawn and hands it
// back; the XR thread then releases them in acquisition order, as OpenXR
// requires.

enum class SlotState {
  Released,  // the runtime's
  Acquired,  // acquired by the XR thread; xrWaitSwapchainImage still pending
  Free,      // acquired and waited: the render worker may take it
  Rendering, // the render worker is drawing into it
  Ready,     // drawn; the XR thread releases it next
};

struct PendingAccess {
  // Dawn's release of the image: semaphores to wait on and the layout it left.
  std::vector<int> fds;
  VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct Slot {
  VkImage image = VK_NULL_HANDLE; // the runtime's swapchain image

  // Render worker only.
  wgpu::SharedTextureMemory stm;
  wgpu::Texture texture;

  // Guarded by g_mutex.
  SlotState state = SlotState::Released;
  uint64_t acquireSeq = 0;       // acquisition order
  PendingAccess fromDawn;        // set when Ready
  std::array<XrView, 2> views{}; // Stereo: the eye poses this image was rendered for
  std::array<int32_t, 2> rect{};  // Stereo, dynamic resolution: the rendered size (0: all)
  uint64_t tickFrame = 0;         // lock-step: the display frame of the tick that started its game frame
  uint64_t readySeen = 0;         // XR thread: the display frame it was first seen Ready (0: not yet)

  // XR thread only.
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool inFlight = false;
  std::vector<VkSemaphore> waitSems;
};

enum StreamId : int { kScreen = 0, kStereo = 1, kHud = 2, kStreamCount = 3 };

struct Stream {
  const char* name = "";
  std::vector<Slot> slots;        // one per swapchain image; sized before Phase::SlotsReady
  uint32_t width = 0, height = 0; // set before the XR thread starts
  uint32_t layers = 1;            // Stereo with multiview: one per eye
  VkFormat format = VK_FORMAT_R8G8B8A8_UNORM; // what Dawn draws as (bytes already sRGB-encoded)
  int64_t swapFormat = 0;                     // swapchain: the sRGB twin, created mutable-format
  uint64_t acquireSeq = 0;        // g_mutex
  int renderingSlot = -1;         // render worker

  // XR thread only.
  XrSwapchain swapchain = XR_NULL_HANDLE;
  bool haveImage = false;
  std::chrono::steady_clock::time_point lastRelease{};
  uint64_t releases = 0;
  std::array<XrView, 2> shownViews{}; // Stereo: poses of the last released image
  std::array<int32_t, 2> shownRect{};  // Stereo: its rendered size (0: all)
  // Debug dump (AURORA_XR_DUMP)
  VkBuffer dumpBuf = VK_NULL_HANDLE;
  VkDeviceMemory dumpMem = VK_NULL_HANDLE;
  void* dumpPtr = nullptr;
  int dumpSlot = -1;
  bool dumped = false;
};

enum class Phase { Idle, Starting, SlotsReady, Imported, Failed };

std::mutex g_mutex;
std::condition_variable g_slotFreeCv; // a slot became Free (XR thread -> render worker)
std::atomic<Phase> g_phase{Phase::Idle};
std::atomic<bool> g_stop{false};
std::atomic<bool> g_sessionRunning{false};
std::thread g_thread;
std::array<Stream, kStreamCount> g_streams;

// Dynamic resolution (AURORA_XR_DYNRES=1, multiview only): the stereo
// swapchain is made at the largest scale, and each frame renders a corner of
// it at `scale` times the runtime's recommended eye size, reported to the
// compositor through imageRect. Render worker only, after setup.
struct DynamicResolution {
  bool on = false;
  bool random = false; // AURORA_XR_DYNRES_RANDOM: a random scale every frame (testing)
  float minScale = 0.8f, maxScale = 1.3f;
  float scale = 1.f;
  uint32_t recWidth = 0, recHeight = 0; // recommended eye size (times AURORA_XR_EYE_SCALE)
  // Controller (update_dynamic_resolution): a live cost table. The scale
  // moves in steps of kDynresStep; each step keeps the frame GPU time
  // measured there, trusted less the longer ago it was measured, and steps
  // not measured lately are predicted from the ones that were.
  double targetNs = 13.0e6; // per game frame, compositor preemptions included
  double bandNs = 0.5e6;    // hysteresis: down only past target + band, up to the target
  double emaNs = 0;         // frame GPU time at the current step, this visit
  int level = 0;            // the current step: scale = minScale + level * kDynresStep
  uint64_t seenSeq = 0;     // R.timing.frameSeq last read
  int settle = 0;           // frames measured since the last scale change
  int sinceStep = 0;        // frames since the last step up
  uint32_t pipelinesSeen = 0; // gfx::pipelines_created() last frame
  int compileQuiet = 0;       // frames since a pipeline was last created
  std::array<uint64_t, 4> recentMisses{};
  uint32_t recentIndex = 0;
  uint64_t frameCount = 0;
  std::chrono::steady_clock::time_point lastFrame{};
  // Logged every 10 s.
  double scaleSum = 0, nsSum = 0;
  uint32_t frames = 0, nsFrames = 0, panics = 0, loneMisses = 0, lateImages = 0;
  uint32_t lateSeen = 0;
  float lowest = 99.f, highest = 0.f;
  std::chrono::steady_clock::time_point lastLog{};
} g_dynres;

// One step of the cost table: the frame GPU time measured at that scale and
// how much of it there is, which fades with time since the last visit.
constexpr float kDynresStep = 0.025f;
constexpr int kDynresLevels = 64;
struct DynresCost {
  double ns = 0;          // running average (0: never measured)
  double samples = 0;     // weight behind it, decaying
  uint64_t lastFrame = 0; // frameCount of the last sample
};
// Per stage and mode (mixed reality or VR): costs differ a lot between them.
// Kept for the session. Render worker only.
std::unordered_map<int, std::array<DynresCost, kDynresLevels>> g_dynresCosts;
// The game framebuffer's format and sample count, read by begin_frame before
// the XR thread starts: when they allow it, both eyes render straight into
// the 3D swapchain image, which then has to be in the framebuffer's format.
wgpu::TextureFormat g_sceneFormat = wgpu::TextureFormat::RGBA8Unorm;
uint32_t g_sceneSamples = 1;
// Both eyes in one pass with a view mask over a 2-layer 3D image (Dawn fork
// multiview), decided by begin_frame before the XR thread starts. Otherwise
// the eyes are replayed side by side, one after the other.
bool g_multiview = false;

// Latest predicted eye poses, from the XR thread for the render worker.
std::mutex g_viewMutex;
std::array<XrView, 2> g_latestViews{};
bool g_viewsValid = false;

// Lock-step pacing (aurora_xr_pace): the XR thread ticks every
// g_displayPerGameFrame display frames, the game waits for the tick instead
// of its own 60 Hz timer, so every game frame is shown for the same number
// of display frames. 0 = the display rate is not a multiple of 60; no pacing.
std::mutex g_paceMutex;
std::condition_variable g_paceCv;
uint64_t g_paceTick = 0;
std::atomic<int> g_displayPerGameFrame{0};
std::atomic<uint64_t> g_lastTickFrame{0}; // the display frame of the newest tick
// 3D images the headset held past their display frames during a fight (XR
// thread): a missed frame as the user sees it. Dynamic resolution reads it.
std::atomic<uint32_t> g_stereoLate{0};

// Where the arena sits in the room (meters, starting head space): A =
// T(pos) · R_y(yaw) · S(scale) takes game units to the room. The XR thread
// moves it while the player drags the arena during a pause; the render
// worker reads it for every 3D frame.
struct ArenaPose {
  XrVector3f pos{0.f, -0.25f, -0.7f};
  float yaw = 0.f;
  float scale = 0.0035f;
};
std::mutex g_arenaMutex;
// The current stage's arena (aurora_xr_set_stage).
ArenaPose g_arena;
// Where every stage starts (AURORA_XR_ARENA_POS, _YAW, _SCALE), before its
// own scale.
ArenaPose g_defaultArena;
bool g_arenaInit = false;
// The current stage (aurora_xr_set_stage; -1: none yet), and where the
// player has put each stage this session. Placing one stage leaves the
// others where they were.
int g_stage = -1;
std::unordered_map<int, ArenaPose> g_stageArenas;
// The game point (game units) placed at the arena position: a stage whose
// geometry sits far from the origin is centered on its action.
std::array<float, 3> g_arenaCenter{};
// The current stage's size against the default scale.
float g_stageScale = 1.f;
// The stage's highest floor (game units; NaN: unknown), for the HUD.
float g_stageTop = std::numeric_limits<float>::quiet_NaN();

// Set by the game every fight frame (aurora_xr_set_paused): the fight is
// paused, so the controllers point and grab instead of playing.
std::atomic<bool> g_fightPaused{false};
// Set by the game (aurora_xr_set_placing): the fight is held before it
// starts for placing the stage. 0 no; 1 with the legend; 2 with the cards.
std::atomic<int> g_placing{0};

// The pictures shown while placing (aurora_xr_set_placing_image): given on
// the game thread, made into swapchains on the XR thread when first shown.
struct PlacingImage {
  std::vector<uint8_t> rgba; // straight alpha; kept for a new session
  uint32_t width = 0, height = 0;
  XrSwapchain swapchain = XR_NULL_HANDLE;
};
std::mutex g_placingMutex;
// The cards and the legend for Touch controllers, then for tracked hands.
std::array<PlacingImage, 4> g_placingImages;
// Looping clips shown over the cards' pictures (aurora_xr_set_placing_clip):
// a grid of frames, played forward then back. 0-2 on the controllers'
// cards, 3-5 on the hands'.
struct PlacingClip {
  PlacingImage atlas;
  int frames = 0, cols = 0, frameW = 0, frameH = 0;
  float fps = 12.f;
  int x = 0, y = 0, w = 0, h = 0; // on its cards, in their pixels
};
std::array<PlacingClip, 6> g_placingClips;

// Set by the game (aurora_xr_set_passthrough): mixed reality shows the room,
// full VR doesn't. The XR thread pauses or starts passthrough to match.
std::atomic<bool> g_passthroughWanted{true};

// Controller input, written by the XR thread, read by the game thread.
std::mutex g_padMutex;
PADStatus g_pad{};
bool g_padValid = false;
// Where a laser points on the virtual screen's picture, for the game
// (aurora_xr_screen_pointer). Under g_padMutex.
struct ScreenPointer {
  bool valid = false, pressed = false;
  float x = 0.f, y = 0.f;
};
ScreenPointer g_screenPointer;
// The frame's size as the game draws it: the picture is letterboxed on the
// screen when its aspect differs from the screen's.
std::atomic<uint32_t> g_contentW{0}, g_contentH{0};

// XR thread only, except where noted.
struct Bridge {
  XrInstance instance = XR_NULL_HANDLE;
  XrSystemId systemId = XR_NULL_SYSTEM_ID;
  XrSession session = XR_NULL_HANDLE;
  XrSpace space = XR_NULL_HANDLE;
  int64_t swapFormat = 0;
  bool hasPassthroughExt = false;
  XrPassthroughFB passthrough = XR_NULL_HANDLE;
  XrPassthroughLayerFB passthroughLayer = XR_NULL_HANDLE;
  bool running = false;

  // Dawn's device (borrowed: Dawn owns it), set by begin_frame before the XR
  // thread starts. `queue` is the spare queue Dawn created for us (index
  // queueIndex of Dawn's family); the runtime gets it in the graphics binding.
  VkInstance vkInstance = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice dev = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t queueFamily = 0;
  uint32_t queueIndex = 1;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memProps{};
  PFN_vkImportSemaphoreFdKHR importSemaphoreFd = nullptr;
  // XR_KHR_vulkan_enable2, for Dawn's creation hooks (main thread).
  PFN_xrCreateVulkanInstanceKHR createVulkanInstance = nullptr;
  PFN_xrCreateVulkanDeviceKHR createVulkanDevice = nullptr;
  PFN_xrGetVulkanGraphicsDevice2KHR getVulkanGraphicsDevice = nullptr;
  bool prepared = false; // prepare_device ran and Dawn's hooks are set

  bool hasRefreshRateExt = false;
  bool hasPerfSettingsExt = false;
  PFN_xrPerfSettingsSetPerformanceLevelEXT setPerformanceLevel = nullptr;
  PFN_xrEnumerateDisplayRefreshRatesFB enumerateRefreshRates = nullptr;
  PFN_xrRequestDisplayRefreshRateFB requestRefreshRate = nullptr;
  PFN_xrGetDisplayRefreshRateFB getRefreshRate = nullptr;
  uint64_t displayFrame = 0;

  PFN_xrCreatePassthroughFB createPassthrough = nullptr;
  PFN_xrDestroyPassthroughFB destroyPassthrough = nullptr;
  PFN_xrCreatePassthroughLayerFB createPassthroughLayer = nullptr;
  PFN_xrDestroyPassthroughLayerFB destroyPassthroughLayer = nullptr;
  PFN_xrPassthroughStartFB startPassthrough = nullptr;
  PFN_xrPassthroughPauseFB pausePassthrough = nullptr;
  bool passthroughRunning = false;
#ifdef XR_META_boundary_visibility
  // The Guardian boundary, hidden while passthrough shows the room.
  bool hasBoundaryVisibilityExt = false;
  PFN_xrRequestBoundaryVisibilityMETA requestBoundaryVisibility = nullptr;
  bool boundarySuppressed = false; // last request
#endif

  std::string dumpDir; // AURORA_XR_DUMP

  // Controller input
  XrActionSet actionSet = XR_NULL_HANDLE;
  XrAction stickMain = XR_NULL_HANDLE, stickC = XR_NULL_HANDLE;
  XrAction btnA = XR_NULL_HANDLE, btnB = XR_NULL_HANDLE, btnX = XR_NULL_HANDLE, btnY = XR_NULL_HANDLE;
  XrAction btnZ = XR_NULL_HANDLE, btnStart = XR_NULL_HANDLE;
  XrAction trigL = XR_NULL_HANDLE, trigR = XR_NULL_HANDLE;
  // Pointing and grabbing the arena while a fight is paused.
  std::array<XrAction, 2> aim{}, grab{};
  std::array<XrSpace, 2> aimSpace{};
  // Uploads to static swapchains still on the GPU: freed once their fence
  // signals (free_finished_uploads), never waited on mid-session.
  struct Upload {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
  };
  std::vector<Upload> uploads;
  bool pointerMode = false; // last frame: the grips grab (paused fight, or a laser on the screen) instead of pressing Z
  bool triggersClick = false; // last frame: a laser on the screen, so the triggers click instead of pressing L and R
  int reelHand = -1;        // last frame: the controller dragging the screen; its stick pushes and pulls it
  std::array<float, 2> stickY{}; // each controller's raw thumbstick Y (left: control stick, right: C-stick)
  // Hand tracking (XR_EXT_hand_tracking): pinches grab the arena too.
  bool hasHandTrackingExt = false;
  PFN_xrCreateHandTrackerEXT createHandTracker = nullptr;
  PFN_xrDestroyHandTrackerEXT destroyHandTracker = nullptr;
  PFN_xrLocateHandJointsEXT locateHandJoints = nullptr;
  std::array<XrHandTrackerEXT, 2> handTrackers{};
  XrVector3f head{};        // between the eyes, from the latest views
  XrQuaternionf headOrientation{0.f, 0.f, 0.f, 1.f};
  bool focused = false;

  // Laser layers: static textures, drawn as quads at display rate.
  XrSwapchain beamSwapchain = XR_NULL_HANDLE, dotSwapchain = XR_NULL_HANDLE;
  XrSwapchain barSwapchain = XR_NULL_HANDLE; // the screen's grab bar
  bool hasColorScaleBias = false;

  uint64_t framesShown = 0, fightFrames = 0;
  std::array<uint64_t, 4> shownHistogram{}; // 3D images shown for 1, 2, 3, 4+ display frames
  std::array<uint64_t, kStreamCount> releasedSinceStats{};
  std::chrono::steady_clock::time_point statsStart;
};
Bridge B;

#define XR_TRY(x)                                                                                                      \
  do {                                                                                                                 \
    XrResult r_ = (x);                                                                                                 \
    if (XR_FAILED(r_)) {                                                                                               \
      Log.error("{} failed: XrResult {}", #x, static_cast<int>(r_));                                                   \
      return false;                                                                                                    \
    }                                                                                                                  \
  } while (0)
#define VK_TRY(x)                                                                                                      \
  do {                                                                                                                 \
    VkResult r_ = (x);                                                                                                 \
    if (r_ != VK_SUCCESS) {                                                                                            \
      Log.error("{} failed: VkResult {}", #x, static_cast<int>(r_));                                                   \
      return false;                                                                                                    \
    }                                                                                                                  \
  } while (0)

bool env_flag(const char* name, bool def) {
  const char* v = std::getenv(name);
  if (!v || !*v)
    return def;
  return !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F');
}

float env_float(const char* name, float def) {
  const char* v = std::getenv(name);
  if (!v || !*v)
    return def;
  char* end = nullptr;
  const float f = std::strtof(v, &end);
  return end != v ? f : def;
}

// How often the GPU timing and dynamic resolution lines are logged:
// AURORA_XR_LOG_PERIOD seconds, 10 by default (1 lines them up with what
// is on screen when profiling a stage).
std::chrono::steady_clock::duration log_period() {
  static const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<float>(std::clamp(env_float("AURORA_XR_LOG_PERIOD", 10.f), 0.25f, 600.f)));
  return period;
}

template <typename F>
F xr_proc(const char* name) {
  PFN_xrVoidFunction fn = nullptr;
  if (XR_FAILED(xrGetInstanceProcAddr(B.instance, name, &fn)))
    return nullptr;
  return reinterpret_cast<F>(fn);
}

uint32_t find_memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
  for (uint32_t i = 0; i < B.memProps.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (B.memProps.memoryTypes[i].propertyFlags & want) == want)
      return i;
  return UINT32_MAX;
}

VkImageMemoryBarrier barrier(VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags srcA,
                             VkAccessFlags dstA, uint32_t srcQ = VK_QUEUE_FAMILY_IGNORED,
                             uint32_t dstQ = VK_QUEUE_FAMILY_IGNORED) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = srcA;
  b.dstAccessMask = dstA;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = srcQ;
  b.dstQueueFamilyIndex = dstQ;
  b.image = img;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
  return b;
}

// ---------------------------------------------------------------- XR thread: setup

bool create_instance() {
#ifdef __ANDROID__
  auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  JavaVM* vm = nullptr;
  env->GetJavaVM(&vm);
  auto localActivity = static_cast<jobject>(SDL_GetAndroidActivity());
  jobject activity = env->NewGlobalRef(localActivity); // kept for the instance's lifetime
  env->DeleteLocalRef(localActivity);
  PFN_xrInitializeLoaderKHR initLoader = nullptr;
  XR_TRY(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                               reinterpret_cast<PFN_xrVoidFunction*>(&initLoader)));
  XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
  li.applicationVM = vm;
  li.applicationContext = activity;
  XR_TRY(initLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&li)));
#endif
  uint32_t n = 0;
  XR_TRY(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr));
  std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
  XR_TRY(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data()));
  bool hasVk2 = false;
  for (const auto& p : props) {
    hasVk2 |= !std::strcmp(p.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
    B.hasPassthroughExt |= !std::strcmp(p.extensionName, XR_FB_PASSTHROUGH_EXTENSION_NAME);
    B.hasRefreshRateExt |= !std::strcmp(p.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    B.hasColorScaleBias |= !std::strcmp(p.extensionName, XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME);
    B.hasPerfSettingsExt |= !std::strcmp(p.extensionName, XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    B.hasHandTrackingExt |= !std::strcmp(p.extensionName, XR_EXT_HAND_TRACKING_EXTENSION_NAME);
#ifdef XR_META_boundary_visibility
    B.hasBoundaryVisibilityExt |= !std::strcmp(p.extensionName, XR_META_BOUNDARY_VISIBILITY_EXTENSION_NAME);
#endif
  }
  if (!hasVk2) {
    Log.error("OpenXR runtime lacks XR_KHR_vulkan_enable2");
    return false;
  }
  std::vector<const char*> exts{XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
  if (B.hasPassthroughExt)
    exts.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
  if (B.hasRefreshRateExt)
    exts.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
  if (B.hasColorScaleBias)
    exts.push_back(XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME);
  if (B.hasPerfSettingsExt)
    exts.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
  // AURORA_XR_HANDS=0 leaves hand tracking off, for measuring what it costs.
  B.hasHandTrackingExt &= env_flag("AURORA_XR_HANDS", true);
  if (B.hasHandTrackingExt)
    exts.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
#ifdef XR_META_boundary_visibility
  // AURORA_XR_BOUNDARY=1 keeps the Guardian boundary in mixed reality.
  B.hasBoundaryVisibilityExt &= !env_flag("AURORA_XR_BOUNDARY", false);
  if (B.hasBoundaryVisibilityExt)
    exts.push_back(XR_META_BOUNDARY_VISIBILITY_EXTENSION_NAME);
#endif
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
#ifdef __ANDROID__
  exts.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
  XrInstanceCreateInfoAndroidKHR aci{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
  aci.applicationVM = vm;
  aci.applicationActivity = activity;
  ci.next = &aci;
#endif
  std::strncpy(ci.applicationInfo.applicationName, "melee-pc", XR_MAX_APPLICATION_NAME_SIZE - 1);
  std::strncpy(ci.applicationInfo.engineName, "aurora", XR_MAX_ENGINE_NAME_SIZE - 1);
  ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
  ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
  ci.enabledExtensionNames = exts.data();
  XR_TRY(xrCreateInstance(&ci, &B.instance));

  XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
  XR_TRY(xrGetInstanceProperties(B.instance, &ip));
  Log.info("OpenXR runtime: {} {}.{}.{} (XR_FB_passthrough {})", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
           XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion),
           B.hasPassthroughExt ? "available" : "absent");

  XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
  si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  XR_TRY(xrGetSystem(B.instance, &si, &B.systemId));
  if (B.hasPassthroughExt) {
    B.createPassthrough = xr_proc<PFN_xrCreatePassthroughFB>("xrCreatePassthroughFB");
    B.destroyPassthrough = xr_proc<PFN_xrDestroyPassthroughFB>("xrDestroyPassthroughFB");
    B.createPassthroughLayer = xr_proc<PFN_xrCreatePassthroughLayerFB>("xrCreatePassthroughLayerFB");
    B.destroyPassthroughLayer = xr_proc<PFN_xrDestroyPassthroughLayerFB>("xrDestroyPassthroughLayerFB");
    B.startPassthrough = xr_proc<PFN_xrPassthroughStartFB>("xrPassthroughStartFB");
    B.pausePassthrough = xr_proc<PFN_xrPassthroughPauseFB>("xrPassthroughPauseFB");
  }
  if (B.hasRefreshRateExt) {
    B.enumerateRefreshRates = xr_proc<PFN_xrEnumerateDisplayRefreshRatesFB>("xrEnumerateDisplayRefreshRatesFB");
    B.requestRefreshRate = xr_proc<PFN_xrRequestDisplayRefreshRateFB>("xrRequestDisplayRefreshRateFB");
    B.getRefreshRate = xr_proc<PFN_xrGetDisplayRefreshRateFB>("xrGetDisplayRefreshRateFB");
  }
  if (B.hasPerfSettingsExt)
    B.setPerformanceLevel = xr_proc<PFN_xrPerfSettingsSetPerformanceLevelEXT>("xrPerfSettingsSetPerformanceLevelEXT");
  if (B.hasHandTrackingExt) {
    B.createHandTracker = xr_proc<PFN_xrCreateHandTrackerEXT>("xrCreateHandTrackerEXT");
    B.destroyHandTracker = xr_proc<PFN_xrDestroyHandTrackerEXT>("xrDestroyHandTrackerEXT");
    B.locateHandJoints = xr_proc<PFN_xrLocateHandJointsEXT>("xrLocateHandJointsEXT");
  }
#ifdef XR_META_boundary_visibility
  if (B.hasBoundaryVisibilityExt)
    B.requestBoundaryVisibility = xr_proc<PFN_xrRequestBoundaryVisibilityMETA>("xrRequestBoundaryVisibilityMETA");
  Log.info("XR_META_boundary_visibility {}", B.requestBoundaryVisibility ? "available" : "unavailable");
#endif
  return true;
}

// Dawn's Vulkan instance and device, created through the runtime
// (XR_KHR_vulkan_enable2) so it can add what it needs and pick the GPU.
// Called by Dawn on the main thread during adapter discovery and device
// creation (prepare_device sets these up).
VkResult hook_create_instance(void*, PFN_vkGetInstanceProcAddr getProcAddr, const VkInstanceCreateInfo* info,
                              VkInstance* out) {
  XrVulkanInstanceCreateInfoKHR xici{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
  xici.systemId = B.systemId;
  xici.pfnGetInstanceProcAddr = getProcAddr;
  xici.vulkanCreateInfo = info;
  VkResult vr = VK_SUCCESS;
  const XrResult r = B.createVulkanInstance(B.instance, &xici, out, &vr);
  if (XR_FAILED(r)) {
    Log.error("xrCreateVulkanInstanceKHR failed: XrResult {}", static_cast<int>(r));
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  return vr;
}

VkPhysicalDevice hook_physical_device(void*, VkInstance instance) {
  XrVulkanGraphicsDeviceGetInfoKHR gi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
  gi.systemId = B.systemId;
  gi.vulkanInstance = instance;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  if (XR_FAILED(B.getVulkanGraphicsDevice(B.instance, &gi, &phys)))
    Log.error("xrGetVulkanGraphicsDevice2KHR failed");
  return phys;
}

VkResult hook_create_device(void*, PFN_vkGetInstanceProcAddr getProcAddr, VkPhysicalDevice phys,
                            const VkDeviceCreateInfo* info, VkDevice* out) {
  XrVulkanDeviceCreateInfoKHR xdci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
  xdci.systemId = B.systemId;
  xdci.pfnGetInstanceProcAddr = getProcAddr;
  xdci.vulkanPhysicalDevice = phys;
  xdci.vulkanCreateInfo = info;
  VkResult vr = VK_SUCCESS;
  const XrResult r = B.createVulkanDevice(B.instance, &xdci, out, &vr);
  if (XR_FAILED(r)) {
    Log.error("xrCreateVulkanDeviceKHR failed: XrResult {}", static_cast<int>(r));
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  return vr;
}

// XR thread: everything it needs on Dawn's device (handles set by
// begin_frame from dawn::native::vulkan::GetDeviceVkHandles).
bool adopt_dawn_device() {
  if (!B.dev || !B.queue) {
    Log.error("Dawn's device has no spare queue for the OpenXR runtime");
    return false;
  }
  vkGetPhysicalDeviceMemoryProperties(B.phys, &B.memProps);
  VkPhysicalDeviceProperties pp;
  vkGetPhysicalDeviceProperties(B.phys, &pp);
  B.importSemaphoreFd =
      reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(vkGetDeviceProcAddr(B.dev, "vkImportSemaphoreFdKHR"));
  if (!B.importSemaphoreFd) {
    Log.error("vkImportSemaphoreFdKHR missing on Dawn's device");
    return false;
  }
  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = B.queueFamily;
  VK_TRY(vkCreateCommandPool(B.dev, &pci, nullptr, &B.pool));
  Log.info("Rendering on Dawn's VkDevice: {}, queue family {} (OpenXR queue {})", pp.deviceName, B.queueFamily,
           B.queueIndex);
  return true;
}

// The image's create info as Dawn should see it: the stream's UNORM format
// over the runtime's sRGB, mutable-format swapchain image, so Dawn stores the
// already sRGB-encoded bytes unchanged and the compositor decodes them.
VkImageCreateInfo swapchain_image_info(const Stream& st) {
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = st.format;
  ici.extent = {st.width, st.height, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = st.layers;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  return ici;
}

bool create_swapchain(Stream& st) {
  XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT |
                  XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
  ci.format = st.swapFormat;
  ci.sampleCount = 1;
  ci.width = st.width;
  ci.height = st.height;
  ci.faceCount = 1;
  ci.arraySize = st.layers;
  ci.mipCount = 1;
  XR_TRY(xrCreateSwapchain(B.session, &ci, &st.swapchain));
  uint32_t n = 0;
  XR_TRY(xrEnumerateSwapchainImages(st.swapchain, 0, &n, nullptr));
  std::vector<XrSwapchainImageVulkan2KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
  XR_TRY(xrEnumerateSwapchainImages(st.swapchain, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data())));
  st.slots = std::vector<Slot>(n);
  for (uint32_t i = 0; i < n; ++i) {
    Slot& s = st.slots[i];
    s.image = imgs[i].image;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = B.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VK_TRY(vkAllocateCommandBuffers(B.dev, &cai, &s.cmd));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_TRY(vkCreateFence(B.dev, &fci, nullptr, &s.fence));
  }
  return true;
}

// Eye resolution for 3D fights: the runtime's recommendation scaled by
// AURORA_XR_EYE_SCALE (default 1.0). On a Quest 3 the full 1680x1760 per eye
// first cost 21-27 ms of GPU per frame; with the flat present skipped during
// fights, both eyes drawn straight into the shared image and the shader copy,
// it costs about 9-10 ms per game frame at 120 Hz.
bool size_stereo_stream() {
  uint32_t n = 0;
  XR_TRY(xrEnumerateViewConfigurationViews(B.instance, B.systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n,
                                           nullptr));
  std::vector<XrViewConfigurationView> views(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
  XR_TRY(xrEnumerateViewConfigurationViews(B.instance, B.systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n,
                                           views.data()));
  if (n != 2) {
    Log.error("Expected 2 stereo views, got {}", n);
    return false;
  }
  const float scale = std::clamp(env_float("AURORA_XR_EYE_SCALE", 1.f), 0.25f, 2.f);
  const auto even = [](float v) { return (static_cast<uint32_t>(v + 0.5f) + 1u) & ~1u; };
  auto& dr = g_dynres;
  dr.recWidth = even(static_cast<float>(views[0].recommendedImageRectWidth) * scale);
  dr.recHeight = even(static_cast<float>(views[0].recommendedImageRectHeight) * scale);
  // Multiview only: the side-by-side path packs both eyes in one image.
#ifdef __ANDROID__
  dr.on = g_multiview && env_flag("AURORA_XR_DYNRES", true); // measured there
#else
  dr.on = g_multiview && env_flag("AURORA_XR_DYNRES", false);
#endif
  if (dr.on) {
    dr.minScale = std::clamp(env_float("AURORA_XR_DYNRES_MIN", 0.8f), 0.5f, 1.f);
    dr.maxScale = std::clamp(env_float("AURORA_XR_DYNRES_MAX", 1.3f), 1.f, 1.6f);
    // A frame averaging 13.5 ms filled the GPU enough for the XR thread to
    // miss submits; Brinstar at a fixed 1.0 averaged 12.5 (13.3 at worst)
    // with no more misses than at 0.8. So aim at 13 and only cut past 13.5:
    // a single 12 ms target answered Brinstar's acid (0.5 ms) by sliding to
    // the lowest scale, which saves almost nothing there.
    dr.targetNs = std::clamp(env_float("AURORA_XR_DYNRES_TARGET_MS", 13.f), 3.f, 16.f) * 1.0e6;
    dr.bandNs = std::clamp(env_float("AURORA_XR_DYNRES_BAND_MS", 0.5f), 0.f, 4.f) * 1.0e6;
    dr.random = env_flag("AURORA_XR_DYNRES_RANDOM", false);
    dr.level = std::max(static_cast<int>((1.f - dr.minScale) / kDynresStep + 0.5f), 0); // start at 1.0
    dr.scale = std::min(dr.minScale + kDynresStep * static_cast<float>(dr.level), dr.maxScale);
  }
  const float alloc = dr.on ? dr.maxScale : 1.f;
  const uint32_t eyeW = even(static_cast<float>(dr.recWidth) * alloc);
  const uint32_t eyeH = even(static_cast<float>(dr.recHeight) * alloc);
  if (dr.on)
    Log.info("Dynamic resolution: {:.2f}-{:.2f} of {}x{} per eye, frame GPU target {:.1f} +- {:.1f} ms",
             dr.minScale, dr.maxScale, dr.recWidth, dr.recHeight, dr.targetNs / 1.0e6, dr.bandNs / 1.0e6);
  g_streams[kStereo].width = g_multiview ? eyeW : eyeW * 2;
  g_streams[kStereo].height = eyeH;
  g_streams[kStereo].layers = g_multiview ? 2 : 1;
  return true;
}

bool create_session() {
  XrGraphicsBindingVulkan2KHR gb{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
  gb.instance = B.vkInstance;
  gb.physicalDevice = B.phys;
  gb.device = B.dev;
  gb.queueFamilyIndex = B.queueFamily;
  gb.queueIndex = B.queueIndex; // Dawn's spare queue; Dawn submits on 0
  XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
  sci.next = &gb;
  sci.systemId = B.systemId;
  XR_TRY(xrCreateSession(B.instance, &sci, &B.session));

  XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  rsci.poseInReferenceSpace.orientation.w = 1.f;
  XR_TRY(xrCreateReferenceSpace(B.session, &rsci, &B.space));

  // The game's frames are already sRGB-encoded bytes. Dawn draws them through
  // a UNORM view of a mutable-format *_SRGB swapchain image, which stores the
  // bytes unchanged, and the runtime decodes them correctly; an sRGB view
  // would re-encode them and wash them out.
  uint32_t n = 0;
  XR_TRY(xrEnumerateSwapchainFormats(B.session, 0, &n, nullptr));
  std::vector<int64_t> fmts(n);
  XR_TRY(xrEnumerateSwapchainFormats(B.session, n, &n, fmts.data()));
  for (int64_t want : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM}) {
    if (std::find(fmts.begin(), fmts.end(), want) != fmts.end()) {
      B.swapFormat = want;
      break;
    }
  }
  if (!B.swapFormat) {
    Log.error("Runtime offers no R8G8B8A8 swapchain format");
    return false;
  }
  for (auto& st : g_streams)
    st.swapFormat = B.swapFormat;
  // The 3D image takes the framebuffer's channel order when the eyes render
  // straight into it (single-sample BGRA8 framebuffers, e.g. desktop NVIDIA).
  auto& stereo = g_streams[kStereo];
  const auto offered = [&](int64_t f) { return std::find(fmts.begin(), fmts.end(), f) != fmts.end(); };
#ifndef __ANDROID__
  if (g_sceneSamples == 1 && g_sceneFormat == wgpu::TextureFormat::BGRA8Unorm && offered(VK_FORMAT_B8G8R8A8_SRGB)) {
    stereo.format = VK_FORMAT_B8G8R8A8_UNORM;
    stereo.swapFormat = VK_FORMAT_B8G8R8A8_SRGB;
  }
#endif
  (void)offered;

  if (B.createPassthrough && B.createPassthroughLayer && env_flag("AURORA_XR_PASSTHROUGH", true)) {
    XrPassthroughCreateInfoFB pci{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
    pci.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
    if (XR_SUCCEEDED(B.createPassthrough(B.session, &pci, &B.passthrough))) {
      XrPassthroughLayerCreateInfoFB lci{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
      lci.passthrough = B.passthrough;
      lci.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
      lci.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
      if (XR_FAILED(B.createPassthroughLayer(B.session, &lci, &B.passthroughLayer)))
        B.passthroughLayer = XR_NULL_HANDLE;
      B.passthroughRunning = true;
    } else {
      B.passthrough = XR_NULL_HANDLE;
    }
  }
  Log.info("Swapchains: screen {}x{}, 3D {}x{} (two {}x{} eyes, format {}), HUD {}x{} (format {}), passthrough {}",
           g_streams[kScreen].width, g_streams[kScreen].height, g_streams[kStereo].width, g_streams[kStereo].height,
           g_multiview ? g_streams[kStereo].width : g_streams[kStereo].width / 2, g_streams[kStereo].height,
           g_streams[kStereo].swapFormat,
           g_streams[kHud].width, g_streams[kHud].height, B.swapFormat, B.passthroughLayer ? "on" : "off");
  return true;
}

bool create_dump_buffer(Stream& st) {
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = static_cast<VkDeviceSize>(st.width) * st.height * st.layers * 4;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VK_TRY(vkCreateBuffer(B.dev, &bci, nullptr, &st.dumpBuf));
  VkMemoryRequirements mr;
  vkGetBufferMemoryRequirements(B.dev, st.dumpBuf, &mr);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex =
      find_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VK_TRY(vkAllocateMemory(B.dev, &mai, nullptr, &st.dumpMem));
  VK_TRY(vkBindBufferMemory(B.dev, st.dumpBuf, st.dumpMem, 0));
  VK_TRY(vkMapMemory(B.dev, st.dumpMem, 0, bci.size, 0, &st.dumpPtr));
  return true;
}

void write_dump(const Stream& st) {
  const auto* px = static_cast<const uint8_t*>(st.dumpPtr);
  const std::string base = B.dumpDir + "/xr_" + st.name;
  if (FILE* f = std::fopen((base + ".ppm").c_str(), "wb")) {
    std::fprintf(f, "P6\n%u %u\n255\n", st.width, st.height * st.layers);
    for (size_t i = 0; i < static_cast<size_t>(st.width) * st.height * st.layers; ++i)
      std::fwrite(px + i * 4, 1, 3, f);
    std::fclose(f);
  }
  if (FILE* f = std::fopen((base + "_alpha.pgm").c_str(), "wb")) {
    std::fprintf(f, "P5\n%u %u\n255\n", st.width, st.height * st.layers);
    for (size_t i = 0; i < static_cast<size_t>(st.width) * st.height * st.layers; ++i)
      std::fputc(px[i * 4 + 3], f);
    std::fclose(f);
  }
  Log.info("Dumped {} to {}.ppm", st.name, base);
}

// ---------------------------------------------------------------- XR thread: controller input
//
// Touch controllers as a GameCube pad (port 1):
//   left stick -> control stick     right stick -> C-stick
//   A / B (right) -> A / B          X / Y (left) -> X / Y
//   left / right trigger -> analog L / R (digital past 90%)
//   either grip -> Z                left menu button -> Start
// While a fight is paused the grips grab the arena instead (Z would retry the
// match in some modes), and each controller shows a laser.

bool create_action(XrAction& action, const char* name, const char* localized, XrActionType type) {
  XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
  std::strncpy(ai.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
  std::strncpy(ai.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
  ai.actionType = type;
  XR_TRY(xrCreateAction(B.actionSet, &ai, &action));
  return true;
}

XrPath path(const char* str) {
  XrPath p = XR_NULL_PATH;
  xrStringToPath(B.instance, str, &p);
  return p;
}

bool suggest(const char* profile, std::initializer_list<std::pair<XrAction, const char*>> bindings) {
  std::vector<XrActionSuggestedBinding> sb;
  for (const auto& [action, binding] : bindings)
    sb.push_back({action, path(binding)});
  XrInteractionProfileSuggestedBinding isb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
  isb.interactionProfile = path(profile);
  isb.suggestedBindings = sb.data();
  isb.countSuggestedBindings = static_cast<uint32_t>(sb.size());
  const XrResult r = xrSuggestInteractionProfileBindings(B.instance, &isb);
  if (XR_FAILED(r))
    Log.warn("Bindings for {} rejected: XrResult {}", profile, static_cast<int>(r));
  return XR_SUCCEEDED(r);
}

bool create_input() {
  XrActionSetCreateInfo asi{XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strncpy(asi.actionSetName, "gamecube", XR_MAX_ACTION_SET_NAME_SIZE - 1);
  std::strncpy(asi.localizedActionSetName, "GameCube controller", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
  XR_TRY(xrCreateActionSet(B.instance, &asi, &B.actionSet));
  if (!create_action(B.stickMain, "control_stick", "Control stick", XR_ACTION_TYPE_VECTOR2F_INPUT) ||
      !create_action(B.stickC, "c_stick", "C-stick", XR_ACTION_TYPE_VECTOR2F_INPUT) ||
      !create_action(B.btnA, "a", "A", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(B.btnB, "b", "B", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(B.btnX, "x", "X", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(B.btnY, "y", "Y", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(B.btnZ, "z", "Z", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(B.btnStart, "start", "Start", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(B.trigL, "l", "L", XR_ACTION_TYPE_FLOAT_INPUT) ||
      !create_action(B.trigR, "r", "R", XR_ACTION_TYPE_FLOAT_INPUT) ||
      !create_action(B.aim[0], "aim_left", "Left pointer", XR_ACTION_TYPE_POSE_INPUT) ||
      !create_action(B.aim[1], "aim_right", "Right pointer", XR_ACTION_TYPE_POSE_INPUT) ||
      !create_action(B.grab[0], "grab_left", "Left grab", XR_ACTION_TYPE_FLOAT_INPUT) ||
      !create_action(B.grab[1], "grab_right", "Right grab", XR_ACTION_TYPE_FLOAT_INPUT))
    return false;

  const bool touch = suggest("/interaction_profiles/oculus/touch_controller",
                             {
                                 {B.stickMain, "/user/hand/left/input/thumbstick"},
                                 {B.stickC, "/user/hand/right/input/thumbstick"},
                                 {B.btnA, "/user/hand/right/input/a/click"},
                                 {B.btnB, "/user/hand/right/input/b/click"},
                                 {B.btnX, "/user/hand/left/input/x/click"},
                                 {B.btnY, "/user/hand/left/input/y/click"},
                                 {B.btnZ, "/user/hand/left/input/squeeze/value"},
                                 {B.btnZ, "/user/hand/right/input/squeeze/value"},
                                 {B.btnStart, "/user/hand/left/input/menu/click"},
                                 {B.trigL, "/user/hand/left/input/trigger/value"},
                                 {B.trigR, "/user/hand/right/input/trigger/value"},
                                 {B.aim[0], "/user/hand/left/input/aim/pose"},
                                 {B.aim[1], "/user/hand/right/input/aim/pose"},
                                 {B.grab[0], "/user/hand/left/input/squeeze/value"},
                                 {B.grab[1], "/user/hand/right/input/squeeze/value"},
                             });
  // Pointing and grabbing only, no pad buttons: the Quest reports bare
  // hands as this profile, select being a pinch, and a pinch must grab,
  // not press A or B. (Monado's keyboard/mouse controllers come through it
  // too; the desktop keyboard plays the pad there.)
  suggest("/interaction_profiles/khr/simple_controller", {
                                                             {B.aim[0], "/user/hand/left/input/aim/pose"},
                                                             {B.aim[1], "/user/hand/right/input/aim/pose"},
                                                             {B.grab[0], "/user/hand/left/input/select/click"},
                                                             {B.grab[1], "/user/hand/right/input/select/click"},
                                                         });
  XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  attach.countActionSets = 1;
  attach.actionSets = &B.actionSet;
  XR_TRY(xrAttachSessionActionSets(B.session, &attach));
  for (int h = 0; h < 2; ++h) {
    XrActionSpaceCreateInfo asci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    asci.action = B.aim[h];
    asci.poseInActionSpace.orientation.w = 1.f;
    XR_TRY(xrCreateActionSpace(B.session, &asci, &B.aimSpace[h]));
  }
  Log.info("Controller input ready ({})", touch ? "Touch controller bindings" : "simple controller only");
  return true;
}

// One tracker per hand, when the runtime and headset support it.
bool create_hand_trackers() {
  if (!B.hasHandTrackingExt || !B.createHandTracker || !B.locateHandJoints)
    return false;
  XrSystemHandTrackingPropertiesEXT ht{XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
  XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
  sp.next = &ht;
  XR_TRY(xrGetSystemProperties(B.instance, B.systemId, &sp));
  if (!ht.supportsHandTracking)
    return false;
  for (int h = 0; h < 2; ++h) {
    XrHandTrackerCreateInfoEXT ci{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
    ci.hand = h == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
    ci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
    XR_TRY(B.createHandTracker(B.session, &ci, &B.handTrackers[h]));
  }
  Log.info("Hand tracking ready");
  return true;
}

bool action_bool(XrAction a) {
  XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
  gi.action = a;
  XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
  return XR_SUCCEEDED(xrGetActionStateBoolean(B.session, &gi, &st)) && st.isActive && st.currentState;
}

float action_float(XrAction a) {
  XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
  gi.action = a;
  XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
  return XR_SUCCEEDED(xrGetActionStateFloat(B.session, &gi, &st)) && st.isActive ? st.currentState : 0.f;
}

XrVector2f action_vec2(XrAction a) {
  XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
  gi.action = a;
  XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
  if (XR_SUCCEEDED(xrGetActionStateVector2f(B.session, &gi, &st)) && st.isActive)
    return st.currentState;
  return {0.f, 0.f};
}

s8 to_axis(float v) { return static_cast<s8>(std::lround(std::clamp(v, -1.f, 1.f) * 127.f)); }

void update_input() {
  if (!B.actionSet || !B.focused) {
    std::lock_guard lock{g_padMutex};
    g_padValid = false;
    return;
  }
  XrActiveActionSet active{B.actionSet, XR_NULL_PATH};
  XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
  sync.countActiveActionSets = 1;
  sync.activeActionSets = &active;
  if (XR_FAILED(xrSyncActions(B.session, &sync)))
    return;
  PADStatus pad{};
  const XrVector2f main = action_vec2(B.stickMain);
  const XrVector2f c = action_vec2(B.stickC);
  B.stickY = {main.y, c.y};
  // A controller dragging the screen reels it in and out with its stick
  // instead of steering the menu.
  if (B.reelHand != 0) {
    pad.stickX = to_axis(main.x);
    pad.stickY = to_axis(main.y);
  }
  if (B.reelHand != 1) {
    pad.substickX = to_axis(c.x);
    pad.substickY = to_axis(c.y);
  }
  const float l = B.triggersClick ? 0.f : action_float(B.trigL);
  const float r = B.triggersClick ? 0.f : action_float(B.trigR);
  pad.triggerLeft = static_cast<u8>(std::lround(std::clamp(l, 0.f, 1.f) * 255.f));
  pad.triggerRight = static_cast<u8>(std::lround(std::clamp(r, 0.f, 1.f) * 255.f));
  u16 buttons = 0;
  buttons |= action_bool(B.btnA) ? PAD_BUTTON_A : 0;
  buttons |= action_bool(B.btnB) ? PAD_BUTTON_B : 0;
  buttons |= action_bool(B.btnX) ? PAD_BUTTON_X : 0;
  buttons |= action_bool(B.btnY) ? PAD_BUTTON_Y : 0;
  buttons |= !B.pointerMode && action_bool(B.btnZ) ? PAD_TRIGGER_Z : 0;
  buttons |= action_bool(B.btnStart) ? PAD_BUTTON_START : 0;
  buttons |= l > 0.9f ? PAD_TRIGGER_L : 0;
  buttons |= r > 0.9f ? PAD_TRIGGER_R : 0;
  pad.button = buttons;
  std::lock_guard lock{g_padMutex};
  g_pad = pad;
  g_padValid = true;
}

// ---------------------------------------------------------------- XR thread: per frame

// The slot's last release has finished on the GPU: its command buffer and
// semaphores can be reused, and a dump it recorded can be written.
void retire_slot(Stream& st, Slot& s, int index) {
  if (!s.inFlight)
    return;
  vkWaitForFences(B.dev, 1, &s.fence, VK_TRUE, UINT64_MAX);
  vkResetFences(B.dev, 1, &s.fence);
  s.inFlight = false;
  for (VkSemaphore sem : s.waitSems)
    vkDestroySemaphore(B.dev, sem, nullptr);
  s.waitSems.clear();
  if (st.dumpSlot == index) {
    st.dumpSlot = -1;
    st.dumped = true;
    write_dump(st);
  }
}

// Keeps one waited-on image ready for the render worker. OpenXR allows one
// waited image per swapchain until it is released, so the next is waited on
// only once the last went back. The wait is short: an image the compositor
// still holds is waited on again next display frame.
bool acquire_ahead(Stream& st) {
  int pending = -1;
  bool haveWaited = false;
  size_t held = 0;
  {
    std::lock_guard lock{g_mutex};
    for (size_t i = 0; i < st.slots.size(); ++i) {
      const SlotState state = st.slots[i].state;
      haveWaited |= state == SlotState::Free || state == SlotState::Rendering || state == SlotState::Ready;
      if (state == SlotState::Acquired)
        pending = static_cast<int>(i);
      if (state != SlotState::Released)
        ++held;
    }
  }
  if (haveWaited)
    return true;
  if (pending < 0) {
    if (held >= st.slots.size())
      return true;
    uint32_t index = 0;
    XR_TRY(xrAcquireSwapchainImage(st.swapchain, nullptr, &index));
    std::lock_guard lock{g_mutex};
    st.slots[index].state = SlotState::Acquired;
    st.slots[index].acquireSeq = ++st.acquireSeq;
    pending = static_cast<int>(index);
  }
  XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wi.timeout = 2'000'000; // 2 ms
  const XrResult r = xrWaitSwapchainImage(st.swapchain, &wi);
  if (r == XR_TIMEOUT_EXPIRED)
    return true;
  XR_TRY(r);
  retire_slot(st, st.slots[pending], pending);
  {
    std::lock_guard lock{g_mutex};
    st.slots[pending].state = SlotState::Free;
  }
  g_slotFreeCv.notify_all();
  return true;
}

// Fixed-latency presentation (XR thread). Under lock-step pacing a game
// frame starts on a tick and its 3D and HUD images are released `latency`
// display frames after that tick, not as soon as they're ready: released as
// soon as ready, images finishing right around a frame boundary alternated
// between being shown 3 display frames and 1 instead of 2 and 2 (about 20
// such pairs a second on Battlefield). `latency` is the smallest number of
// display frames by which 97% of recent 3D images were ready, at most one
// past a game frame's display frames; it rises at once and falls after four
// steady windows. On with dynamic resolution (see release_ready);
// AURORA_XR_FIXED_LATENCY=0 turns it off, =<n> forces it on at n.
struct Presentation {
  int forced = -1; // AURORA_XR_FIXED_LATENCY
  int latency = 0;
  std::array<uint8_t, 120> samples{};
  size_t count = 0, next = 0;
  int sinceUpdate = 0, steadyLower = 0;
  uint64_t held = 0; // images held for their slot (logged)
} g_present;

void note_ready_latency(uint64_t lat) {
  auto& p = g_present;
  p.samples[p.next++ % p.samples.size()] = static_cast<uint8_t>(std::min<uint64_t>(lat, 15));
  p.count = std::min(p.count + 1, p.samples.size());
  if (p.forced >= 0) {
    p.latency = p.forced;
    return;
  }
  if (++p.sinceUpdate < 30)
    return;
  p.sinceUpdate = 0;
  std::array<int, 16> hist{};
  for (size_t i = 0; i < p.count; ++i)
    ++hist[p.samples[i]];
  int k = 0;
  for (int acc = 0; k < 16; ++k) {
    acc += hist[k];
    if (acc * 100 >= static_cast<int>(p.count) * 97)
      break;
  }
  // At most one display frame past the ideal: holding images longer ties up
  // the swapchain's few images, and the render worker then finds none free
  // (Pokémon Stadium rock with four CPUs climbed to 4 and missed frames).
  k = std::min(k, std::max(g_displayPerGameFrame.load(), 1) + 1);
  if (k > p.latency) {
    p.latency = k;
    p.steadyLower = 0;
  } else if (k < p.latency) {
    if (++p.steadyLower >= 4) {
      p.latency = k;
      p.steadyLower = 0;
    }
  } else {
    p.steadyLower = 0;
  }
}

// Releases drawn images in acquisition order: waits (on the GPU, on our
// queue) for Dawn's semaphores, puts the image back in the layout the
// runtime expects, then hands it to the runtime.
void write_dump_layout(const std::array<XrView, 2>& views);

bool release_ready(Stream& st, bool& released) {
  released = false;
  for (;;) {
    int index = -1;
    PendingAccess fromDawn;
    std::array<XrView, 2> views{};
    std::array<int32_t, 2> rect{};
    {
      std::lock_guard lock{g_mutex};
      uint64_t oldest = UINT64_MAX;
      for (size_t i = 0; i < st.slots.size(); ++i) {
        const auto& sl = st.slots[i];
        if (sl.state != SlotState::Released && sl.acquireSeq < oldest) {
          oldest = sl.acquireSeq;
          index = static_cast<int>(i);
        }
      }
      if (index < 0 || st.slots[index].state != SlotState::Ready)
        return true;
      // Fixed-latency presentation: hold a 3D or HUD image for its slot.
      auto& sl = st.slots[index];
      // On with dynamic resolution, which keeps frames on time: without it,
      // frames running late on an overloaded stage were held into slots the
      // render worker then lacked (Pokémon Stadium rock, four CPUs: 58.2 ->
      // 55.5 game fps). AURORA_XR_FIXED_LATENCY=<n> forces it on.
      const bool fixedLatency = g_present.forced > 0 || (g_present.forced < 0 && g_dynres.on);
      if (fixedLatency && g_displayPerGameFrame > 0 && sl.tickFrame != 0 &&
          (&st == &g_streams[kStereo] || &st == &g_streams[kHud])) {
        if (sl.readySeen == 0) {
          sl.readySeen = B.displayFrame;
          if (&st == &g_streams[kStereo] && B.displayFrame >= sl.tickFrame)
            note_ready_latency(B.displayFrame - sl.tickFrame);
        }
        if (B.displayFrame < sl.tickFrame + static_cast<uint64_t>(g_present.latency)) {
          if (&st == &g_streams[kStereo] && sl.readySeen == B.displayFrame)
            ++g_present.held;
          return true;
        }
      }
      fromDawn = std::move(st.slots[index].fromDawn);
      st.slots[index].fromDawn = {};
      views = st.slots[index].views;
      rect = st.slots[index].rect;
    }
    Slot& s = st.slots[index];
    retire_slot(st, s, index);

    for (int fd : fromDawn.fds) {
      VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      VkSemaphore sem = VK_NULL_HANDLE;
      VK_TRY(vkCreateSemaphore(B.dev, &sci, nullptr, &sem));
      VkImportSemaphoreFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
      imp.semaphore = sem;
      imp.flags = kSemImportFlags;
      imp.handleType = kSemHandle;
      imp.fd = fd; // Vulkan owns it on success
      if (B.importSemaphoreFd(B.dev, &imp) != VK_SUCCESS) {
        close(fd);
        vkDestroySemaphore(B.dev, sem, nullptr);
        Log.error("Importing Dawn's semaphore failed");
        return false;
      }
      s.waitSems.push_back(sem);
    }

    // OpenXR takes Vulkan color swapchain images back in
    // COLOR_ATTACHMENT_OPTIMAL; Dawn may have left another layout.
    const VkImageLayout from = fromDawn.newLayout != VK_IMAGE_LAYOUT_UNDEFINED
                                   ? fromDawn.newLayout
                                   : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const bool dumpNow = !B.dumpDir.empty() && !st.dumped && st.dumpSlot < 0 &&
                         st.releases >= static_cast<uint64_t>(env_float("AURORA_XR_DUMP_AFTER", 300.f)) &&
                         (st.dumpBuf || create_dump_buffer(st));
    const bool record = dumpNow || from != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (record) {
      VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      VK_TRY(vkResetCommandBuffer(s.cmd, 0));
      VK_TRY(vkBeginCommandBuffer(s.cmd, &cbi));
      VkImageLayout layout = from;
      if (dumpNow) {
        const auto toSrc = barrier(s.image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &toSrc);
        VkBufferImageCopy rb{};
        rb.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, st.layers}; // layers stack in the dump
        rb.imageExtent = {st.width, st.height, 1};
        vkCmdCopyImageToBuffer(s.cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, st.dumpBuf, 1, &rb);
        st.dumpSlot = index;
        layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        if (&st == &g_streams[kStereo])
          write_dump_layout(s.views);
      }
      if (layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        const auto back = barrier(s.image, layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, 0);
        vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &back);
      }
      VK_TRY(vkEndCommandBuffer(s.cmd));
    }
    std::vector<VkPipelineStageFlags> stages(s.waitSems.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = static_cast<uint32_t>(s.waitSems.size());
    si.pWaitSemaphores = s.waitSems.data();
    si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = record ? 1 : 0;
    si.pCommandBuffers = &s.cmd;
    VK_TRY(vkQueueSubmit(B.queue, 1, &si, s.fence));
    s.inFlight = true;
    XR_TRY(xrReleaseSwapchainImage(st.swapchain, nullptr));
    {
      std::lock_guard lock{g_mutex};
      s.state = SlotState::Released;
    }
    st.haveImage = true;
    st.lastRelease = std::chrono::steady_clock::now();
    st.shownViews = views;
    st.shownRect = rect;
    ++st.releases;
    released = true;
  }
}

// CPU and GPU clock levels (XR_EXT_performance_settings), per domain:
// AURORA_XR_PERF_CPU / AURORA_XR_PERF_GPU = off, low, high, boost.
// - CPU: high by default on Android. Light scenes let the CPU drop to its
//   lowest clock, and the XR thread then misses its 120 Hz submits: 2-player
//   Battlefield showed 6-7 stale frames a second at CPU level 2 and 0.2 at
//   level 3 with the request (2026-10-03).
// - GPU: the runtime's own by default. A sustained-high request changed
//   nothing: passthrough caps a Quest 3's GPU at level 2 (docs/quest-xr.md).
void request_performance_levels() {
  if (!B.setPerformanceLevel) {
    Log.info("XR_EXT_performance_settings unavailable; the runtime picks clock levels");
    return;
  }
  const auto request = [](const char* name, const char* fallback, XrPerfSettingsDomainEXT domain,
                          const char* label) {
    const char* v = std::getenv(name);
    const std::string_view s = v != nullptr && *v != '\0' ? v : fallback;
    if (s == "off")
      return;
    const auto lvl = s == "low"     ? XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT
                     : s == "boost" ? XR_PERF_SETTINGS_LEVEL_BOOST_EXT
                                    : XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
    const XrResult r = B.setPerformanceLevel(B.session, domain, lvl);
    Log.info("Performance level requested: {} {} ({})", label, static_cast<int>(lvl), static_cast<int>(r));
  };
#ifdef __ANDROID__
  request("AURORA_XR_PERF_CPU", "high", XR_PERF_SETTINGS_DOMAIN_CPU_EXT, "CPU");
#else
  request("AURORA_XR_PERF_CPU", "off", XR_PERF_SETTINGS_DOMAIN_CPU_EXT, "CPU");
#endif
  request("AURORA_XR_PERF_GPU", "off", XR_PERF_SETTINGS_DOMAIN_GPU_EXT, "GPU");
}

void request_refresh_rate() {
  if (!B.enumerateRefreshRates || !B.requestRefreshRate)
    return;
  uint32_t n = 0;
  if (XR_FAILED(B.enumerateRefreshRates(B.session, 0, &n, nullptr)) || n == 0)
    return;
  std::vector<float> rates(n);
  if (XR_FAILED(B.enumerateRefreshRates(B.session, n, &n, rates.data())))
    return;
  std::string list;
  for (float r : rates)
    list += fmt::format("{}{:.0f}", list.empty() ? "" : ", ", r);
  const auto offered = [&](float want) {
    return std::any_of(rates.begin(), rates.end(), [&](float r) { return std::abs(r - want) < 0.5f; });
  };
  float pick = 0.f;
  if (const float wanted = env_float("AURORA_XR_REFRESH", 0.f); wanted > 0.f && offered(wanted))
    pick = wanted;
  else if (offered(60.f))
    pick = 60.f;
  else if (offered(120.f))
    pick = 120.f;
  if (pick > 0.f && XR_SUCCEEDED(B.requestRefreshRate(B.session, pick)))
    Log.info("Display refresh rates offered: {}; requested {:.0f} Hz", list, pick);
  else
    Log.info("Display refresh rates offered: {}; keeping the default", list);
}

// Recompute how many display frames each game frame gets from the current
// display rate (on session start and on XR_FB_display_refresh_rate changes).
void update_pacing() {
  float rate = 0.f;
  if (B.getRefreshRate)
    B.getRefreshRate(B.session, &rate);
  if (rate <= 0.f && !B.getRefreshRate)
    rate = 60.f; // no extension (e.g. some runtimes): assume a 60 Hz display
  const float ratio = rate / 60.f;
  const int per = std::abs(ratio - std::round(ratio)) < 0.02f ? static_cast<int>(std::round(ratio)) : 0;
  const int old = g_displayPerGameFrame.exchange(env_flag("AURORA_XR_LOCKSTEP", true) ? per : 0);
  if (old != g_displayPerGameFrame)
    Log.info("Display at {:.1f} Hz: {}", rate,
             g_displayPerGameFrame ? fmt::format("game paced to every {} display frame(s)", g_displayPerGameFrame.load())
                                   : std::string{"not a multiple of 60 Hz, game keeps its own timer"});
}

// Publish where the eyes will be at this frame's display time, for the
// render worker's next 3D frame.
void publish_views(XrTime displayTime) {
  XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
  vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  vli.displayTime = displayTime;
  vli.space = B.space;
  XrViewState vs{XR_TYPE_VIEW_STATE};
  std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
  uint32_t n = 0;
  if (XR_FAILED(xrLocateViews(B.session, &vli, &vs, 2, &n, views.data())) || n != 2)
    return;
  const bool valid = (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0 &&
                     (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
  std::lock_guard lock{g_viewMutex};
  if (valid) {
    g_latestViews = views;
    g_viewsValid = true;
    const XrVector3f &l = views[0].pose.position, &r = views[1].pose.position;
    B.head = {(l.x + r.x) * 0.5f, (l.y + r.y) * 0.5f, (l.z + r.z) * 0.5f};
    B.headOrientation = views[0].pose.orientation;
  }
}

// ---------------------------------------------------------------- XR thread: arena and screen placement
//
// While a fight is paused each hand, holding a controller or tracked (the
// controller put down), points a laser at the arena, or shows a dot once its
// touch point (the controller's tip, or the pinch) is inside the arena's grab
// box. Squeezing the grip or pinching with the laser on the arena, or inside
// the box, grabs it:
//   one hand   the arena hangs off the laser at the grabbed point, or off
//              the touch point, keeping its heading; letting go leaves it
//   two hands  squeezing or pinching with the other hand too (anywhere)
//              scales the arena and turns it about the vertical axis around
//              the point between the touch points, and the arena follows
//              that point as it moves; letting go of one hand carries on
//              with the other alone
// Controllers and hands work the same way and mix freely.
// Starting position and scale: AURORA_XR_ARENA_POS, AURORA_XR_ARENA_SCALE.
//
// The virtual screen (menus) works like a Quest window, any time it shows.
// Its lasers appear only while they point at it. On the picture a laser
// points for the game (aurora_xr_screen_pointer) and the trigger or a pinch
// clicks; the grips press Z and the triggers L and R unless a laser is on
// the screen. The bar under it is the handle: a grip, trigger or pinch with
// the laser on it, or a hand touching it, takes hold of the screen, and
// while one hand holds it a press of the other with its laser anywhere on
// the screen joins in. It always turns to face the head; one controller
// dragging it also pushes and pulls it with its stick, and two hands resize
// it about its center. The arena and the screen keep separate poses: moving
// one never moves the other.

XrVector3f operator+(XrVector3f a, XrVector3f b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
XrVector3f operator-(XrVector3f a, XrVector3f b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
XrVector3f operator*(XrVector3f a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float vdot(XrVector3f a, XrVector3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
XrVector3f vcross(XrVector3f a, XrVector3f b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float vlen(XrVector3f a) { return std::sqrt(vdot(a, a)); }
XrVector3f vnorm(XrVector3f a) {
  const float l = vlen(a);
  return l > 1e-6f ? a * (1.f / l) : a;
}
// v rotated by the unit quaternion q.
XrVector3f qrot(const XrQuaternionf& q, XrVector3f v) {
  const XrVector3f u{q.x, q.y, q.z};
  const XrVector3f t = vcross(u, v) * 2.f;
  return v + t * q.w + vcross(u, t);
}
// Rotation by `a` radians about +Y (counter-clockwise seen from above).
XrVector3f rot_y(XrVector3f v, float a) {
  const float c = std::cos(a), s = std::sin(a);
  return {c * v.x + s * v.z, v.y, -s * v.x + c * v.z};
}
XrQuaternionf yaw_quat(float a) { return {0.f, std::sin(a / 2.f), 0.f, std::cos(a / 2.f)}; }
// The rotation whose local axes are x, y, z (orthonormal).
XrQuaternionf quat_from_axes(XrVector3f x, XrVector3f y, XrVector3f z) {
  const float tr = x.x + y.y + z.z;
  XrQuaternionf q;
  if (tr > 0.f) {
    const float s = std::sqrt(tr + 1.f) * 2.f;
    q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, s / 4.f};
  } else if (x.x > y.y && x.x > z.z) {
    const float s = std::sqrt(1.f + x.x - y.y - z.z) * 2.f;
    q = {s / 4.f, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
  } else if (y.y > z.z) {
    const float s = std::sqrt(1.f + y.y - x.x - z.z) * 2.f;
    q = {(y.x + x.y) / s, s / 4.f, (z.y + y.z) / s, (z.x - x.z) / s};
  } else {
    const float s = std::sqrt(1.f + z.z - x.x - y.y) * 2.f;
    q = {(z.x + x.z) / s, (z.y + y.z) / s, s / 4.f, (x.y - y.x) / s};
  }
  return q;
}

// Under g_arenaMutex.
void init_arena() {
  if (g_arenaInit)
    return;
  g_arenaInit = true;
  ArenaPose& d = g_defaultArena;
  if (const char* v = std::getenv("AURORA_XR_ARENA_POS"))
    std::sscanf(v, "%f,%f,%f", &d.pos.x, &d.pos.y, &d.pos.z);
  d.scale = env_float("AURORA_XR_ARENA_SCALE", 0.0035f);
  d.yaw = env_float("AURORA_XR_ARENA_YAW", 0.f) * 3.14159265f / 180.f; // degrees
  g_arena = d;
  g_arena.scale *= g_stageScale;
}

ArenaPose arena_pose() {
  std::lock_guard lock{g_arenaMutex};
  init_arena();
  return g_arena;
}

// Places the current stage only; it stays there for the rest of the session.
void set_arena_pose(const ArenaPose& a) {
  std::lock_guard lock{g_arenaMutex};
  g_arena = a;
  g_stageArenas[g_stage] = a;
}

// What the lasers can grab, in game units around the arena center (the game
// point placed at the arena position, aurora_xr_set_stage): wider and
// taller than any stage's main platform, so pointing near it is enough.
constexpr XrVector3f kGrabBoxMin{-120.f, -80.f, -60.f};
constexpr XrVector3f kGrabBoxMax{120.f, 100.f, 60.f};
// Meters per game unit: Final Destination from about 25 cm to 8.5 m wide.
constexpr float kMinArenaScale = 0.0015f, kMaxArenaScale = 0.05f;

// A room point (meters) in game units from the arena center. Not from the
// world origin: on Corneria, centered on the Great Fox far above it, the box
// sat well below the ship.
XrVector3f to_game(const ArenaPose& a, XrVector3f p) { return rot_y(p - a.pos, -a.yaw) * (1.f / a.scale); }

bool in_grab_box(const ArenaPose& a, XrVector3f p) {
  const XrVector3f g = to_game(a, p);
  return g.x >= kGrabBoxMin.x && g.x <= kGrabBoxMax.x && g.y >= kGrabBoxMin.y && g.y <= kGrabBoxMax.y &&
         g.z >= kGrabBoxMin.z && g.z <= kGrabBoxMax.z;
}

// Distance along the ray (meters) to the arena's grab box, or -1 for a miss.
float hit_arena(const ArenaPose& a, XrVector3f origin, XrVector3f dir) {
  const XrVector3f o = to_game(a, origin);
  const XrVector3f d = rot_y(dir, -a.yaw) * (1.f / a.scale);
  const float os[3] = {o.x, o.y, o.z}, ds[3] = {d.x, d.y, d.z};
  const float lo[3] = {kGrabBoxMin.x, kGrabBoxMin.y, kGrabBoxMin.z};
  const float hi[3] = {kGrabBoxMax.x, kGrabBoxMax.y, kGrabBoxMax.z};
  float t0 = -1e30f, t1 = 1e30f;
  for (int i = 0; i < 3; ++i) {
    if (std::abs(ds[i]) < 1e-9f) {
      if (os[i] < lo[i] || os[i] > hi[i])
        return -1.f;
      continue;
    }
    float ta = (lo[i] - os[i]) / ds[i], tb = (hi[i] - os[i]) / ds[i];
    if (ta > tb)
      std::swap(ta, tb);
    t0 = std::max(t0, ta);
    t1 = std::min(t1, tb);
  }
  if (t0 > t1 || t1 < 0.f)
    return -1.f;
  return std::max(t0, 0.f);
}

// The virtual screen, in the starting head space: a quad facing +Z in its
// own frame. XR thread only. Kept for the whole session.
struct ScreenPose {
  XrVector3f pos{0.f, 0.f, -0.85f};
  XrQuaternionf orientation{0.f, 0.f, 0.f, 1.f};
  float width = 0.7f; // meters: about 45 degrees across at 0.85 m
};
ScreenPose g_screen;
bool g_screenInit = false;
constexpr float kMinScreenWidth = 0.4f, kMaxScreenWidth = 8.f;
// One controller's reach along its laser while dragging the screen.
constexpr float kMinScreenDist = 0.3f, kMaxScreenDist = 10.f;

ScreenPose& screen_pose() {
  if (!g_screenInit) {
    g_screenInit = true;
    g_screen.pos = {0.f, env_float("AURORA_XR_SCREEN_Y", 0.f), -env_float("AURORA_XR_SCREEN_DISTANCE", 0.85f)};
    g_screen.width = env_float("AURORA_XR_SCREEN_WIDTH", 0.7f);
  }
  return g_screen;
}

// Screen height over width, from its swapchain.
float screen_aspect() {
  const auto& st = g_streams[kScreen];
  return st.width > 0 ? static_cast<float>(st.height) / static_cast<float>(st.width) : 9.f / 16.f;
}

XrQuaternionf qconj(const XrQuaternionf& q) { return {-q.x, -q.y, -q.z, q.w}; }

// The screen turned to face `eye`, upright.
XrQuaternionf facing(XrVector3f pos, XrVector3f eye) {
  const XrVector3f z = vnorm(eye - pos);
  XrVector3f x = vcross({0.f, 1.f, 0.f}, z);
  if (vlen(x) < 1e-3f)
    return {0.f, 0.f, 0.f, 1.f}; // straight above or below: keep it level
  x = vnorm(x);
  return quat_from_axes(x, vcross(z, x), z);
}

// The grab bar under the screen, in the screen's frame (meters): its
// center's height and half extents. It grows with the screen, within a
// hand's reach of sizes.
struct BarRect {
  float y, hw, hh;
};
BarRect bar_rect(const ScreenPose& s) {
  const float hw = std::clamp(s.width * 0.1f, 0.05f, 0.2f);
  const float hh = std::clamp(s.width * 0.008f, 0.005f, 0.012f);
  return {-s.width * screen_aspect() * 0.5f - hh * 3.f, hw, hh};
}

// A point in the screen's frame on (or near) the bar: 2 cm of slack around
// it, since it is thin.
bool on_bar(const ScreenPose& s, XrVector3f local) {
  const BarRect b = bar_rect(s);
  return std::abs(local.x) <= b.hw + 0.02f && std::abs(local.y - b.y) <= b.hh + 0.02f;
}

// Touching the bar: on it and within a few centimeters of its surface.
bool in_bar_box(const ScreenPose& s, XrVector3f p) {
  const XrVector3f l = qrot(qconj(s.orientation), p - s.pos);
  return on_bar(s, l) && std::abs(l.z) <= 0.05f;
}

// Where the ray meets the screen's plane, from either side: meters along
// it and the point in the screen's frame. False: parallel, or behind.
bool hit_screen_plane(const ScreenPose& s, XrVector3f origin, XrVector3f dir, float& t, XrVector3f& local) {
  const XrQuaternionf inv = qconj(s.orientation);
  const XrVector3f o = qrot(inv, origin - s.pos), d = qrot(inv, dir);
  if (std::abs(d.z) < 1e-6f)
    return false;
  t = -o.z / d.z;
  if (t < 0.f)
    return false;
  local = o + d * t;
  return true;
}

// What the hands place: the arena while a fight is paused, the screen
// otherwise.
enum class Target { Arena, Screen };

struct Hand {
  bool valid = false;
  bool tracked = false;               // an articulated hand (pinches) rather than a controller (grip)
  XrVector3f origin{};                // the laser: from the controller, or from the hand's index knuckle
  XrVector3f dir{0.f, 0.f, -1.f};
  XrVector3f touch{};                 // what reaches into the arena: the pinch, or the controller's tip
  bool held = false, wasHeld = false; // grip or pinch closed, with hysteresis
  bool sel = false, wasSel = false;   // a controller's trigger pulled (a pinch is `held`), with hysteresis
  bool palmIn = false;                // a tracked hand's palm faces the head: not pointing
  bool inside = false;                // `touch` is in the arena's grab box (or on the screen's bar): a dot instead of a laser
  bool engaged = false;               // holding the arena
  bool direct = false;                // engaged from inside the box: drags by `touch`, not along the laser
  float hit = -1.f;                   // outside the box: meters along the laser to the arena, -1 = none
  // The screen only: what the laser is on, where on the picture (0..1,
  // x right, y down), and whether this hand is clicking the picture.
  enum class On { None, Picture, Bar } on = On::None;
  float u = 0.f, v = 0.f;
  bool clicking = false;
};

struct Grab {
  std::array<Hand, 2> hands;
  Target target = Target::Arena;
  int oneHand = -1; // the hand dragging the target alone
  bool twoHands = false;
  ArenaPose start;
  ScreenPose screenStart;
  float dist = 0.f;  // one controller: the grabbed point's distance along its laser
  XrVector3f offset; // one hand: target position minus the grabbed point
  XrVector3f mid0;   // two hands: midpoint, span and heading at the start
  float span0 = 1.f, heading0 = 0.f;
};
Grab G;

// Thumb and index tips closer than kPinchClose (meters) close a pinch;
// farther than kPinchOpen open it.
constexpr float kPinchClose = 0.02f, kPinchOpen = 0.035f;

// An articulated hand's pinch and laser, while hand tracking sees the hand
// (on Quest, whenever that controller is put down). False: use the
// controller. The laser runs from an estimated shoulder through the index
// knuckle, which stays put while the fingers pinch.
bool locate_pinch(int h, XrTime time, Hand& hand) {
  if (!B.handTrackers[h])
    return false;
  std::array<XrHandJointLocationEXT, XR_HAND_JOINT_COUNT_EXT> joints{};
  XrHandJointLocationsEXT locs{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
  locs.jointCount = static_cast<uint32_t>(joints.size());
  locs.jointLocations = joints.data();
  XrHandJointsLocateInfoEXT li{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
  li.baseSpace = B.space;
  li.time = time;
  if (XR_FAILED(B.locateHandJoints(B.handTrackers[h], &li, &locs)) || !locs.isActive)
    return false;
  hand.tracked = true;
  const auto& thumb = joints[XR_HAND_JOINT_THUMB_TIP_EXT];
  const auto& index = joints[XR_HAND_JOINT_INDEX_TIP_EXT];
  const auto& knuckle = joints[XR_HAND_JOINT_INDEX_PROXIMAL_EXT];
  if (!(thumb.locationFlags & index.locationFlags & knuckle.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
    return true; // seen but lost this frame: not valid, so any grab lets go
  hand.valid = true;
  hand.touch = (thumb.pose.position + index.pose.position) * 0.5f;
  const XrVector3f fwd = qrot(B.headOrientation, {0.f, 0.f, -1.f});
  const XrVector3f right = vnorm({-fwd.z, 0.f, fwd.x});
  const XrVector3f shoulder = B.head + XrVector3f{0.f, -0.15f, 0.f} + right * (h == 0 ? -0.18f : 0.18f);
  hand.origin = knuckle.pose.position;
  hand.dir = vnorm(hand.origin - shoulder);
  const float d = vlen(index.pose.position - thumb.pose.position);
  hand.held = hand.held ? d < kPinchOpen : d < kPinchClose;
  // A palm turned toward the face isn't pointing (as on the Quest's own
  // menus): no laser, and its pinches do nothing, until it turns away. The
  // palm faces along its joint's -Y. A hand already holding or clicking
  // keeps going.
  const auto& palm = joints[XR_HAND_JOINT_PALM_EXT];
  if (palm.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) {
    const XrVector3f normal = qrot(palm.pose.orientation, {0.f, -1.f, 0.f});
    const float facing = vdot(normal, vnorm(B.head - palm.pose.position));
    hand.palmIn = hand.palmIn ? facing > 0.2f : facing > 0.45f;
  }
  if (hand.palmIn && !hand.engaged && !hand.clicking)
    hand.valid = false;
  return true;
}

void locate_hands(XrTime time) {
  for (int h = 0; h < 2; ++h) {
    Hand& hand = G.hands[h];
    const bool wasTracked = hand.tracked;
    hand.wasHeld = hand.held;
    hand.wasSel = hand.sel;
    hand.valid = false;
    if (!B.focused) {
      hand.held = false;
      hand.sel = false;
      continue;
    }
    if (!locate_pinch(h, time, hand)) {
      hand.tracked = false;
      if (wasTracked)
        hand.held = false; // picked up the controller: its grip starts open
      if (!B.aimSpace[h]) {
        hand.held = false;
        continue;
      }
      XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
      constexpr XrSpaceLocationFlags kValid =
          XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
      if (XR_SUCCEEDED(xrLocateSpace(B.aimSpace[h], B.space, time, &loc)) && (loc.locationFlags & kValid) == kValid) {
        hand.valid = true;
        hand.origin = loc.pose.position;
        hand.dir = vnorm(qrot(loc.pose.orientation, {0.f, 0.f, -1.f}));
        hand.touch = hand.origin;
      }
      const float grip = action_float(B.grab[h]);
      hand.held = hand.held ? grip > 0.35f : grip > 0.65f;
      const float trigger = action_float(h == 0 ? B.trigL : B.trigR);
      hand.sel = hand.sel ? trigger > 0.35f : trigger > 0.65f;
    } else if (!wasTracked) {
      hand.held = false; // put the controller down: the pinch starts open
    }
    if (hand.tracked || !hand.valid)
      hand.sel = false;
  }
}

void end_grab() {
  if (G.oneHand < 0 && !G.twoHands)
    return;
  G.oneHand = -1;
  G.twoHands = false;
  if (G.target == Target::Screen) {
    const ScreenPose& s = screen_pose();
    Log.info("Screen placed at {:.2f},{:.2f},{:.2f}, width {:.2f} m", s.pos.x, s.pos.y, s.pos.z, s.width);
    return;
  }
  const ArenaPose a = arena_pose();
  int stage;
  float stageScale;
  {
    std::lock_guard lock{g_arenaMutex};
    stage = g_stage;
    stageScale = g_stageScale;
  }
  // The scale without the stage's own, as AURORA_XR_ARENA_SCALE takes it.
  Log.info("Arena for stage {} placed at {:.2f},{:.2f},{:.2f}, yaw {:.0f} deg, scale {:.4f} (stage's own x{:.2f})",
           stage, a.pos.x, a.pos.y, a.pos.z, a.yaw * 57.2958f, a.scale / stageScale, stageScale);
}

// What a hand drags alone: its pinch or controller tip, or the grabbed
// point on its laser.
XrVector3f drag_point(const Hand& hand) { return hand.direct ? hand.touch : hand.origin + hand.dir * G.dist; }

void begin_one_hand(int h, XrVector3f pos) {
  const Hand& hand = G.hands[h];
  G.oneHand = h;
  G.twoHands = false;
  G.start = arena_pose();
  G.screenStart = screen_pose();
  // The laser's hit, or (back from two hands with the laser off the target)
  // the point on the laser nearest the target.
  if (!hand.direct)
    G.dist = hand.hit >= 0.f ? hand.hit : std::max(vdot(pos - hand.origin, hand.dir), 0.1f);
  G.offset = pos - drag_point(hand);
}

void begin_two_hands() {
  const XrVector3f l = G.hands[0].touch, r = G.hands[1].touch;
  const XrVector3f span = r - l;
  G.oneHand = -1;
  G.twoHands = true;
  G.start = arena_pose();
  G.screenStart = screen_pose();
  G.mid0 = (l + r) * 0.5f;
  G.span0 = std::max(vlen(span), 0.05f);
  G.heading0 = std::atan2(-span.z, span.x);
}

// The screen while one hand drags it, or two resize it. It keeps facing
// the head.
void place_screen() {
  ScreenPose& s = screen_pose();
  if (G.oneHand >= 0) {
    const Hand& hand = G.hands[G.oneHand];
    if (!hand.direct && !hand.tracked) {
      // The stick reels it along the laser, about doubling the reach per
      // second at full tilt.
      static auto last = std::chrono::steady_clock::now();
      const auto now = std::chrono::steady_clock::now();
      const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.05f);
      last = now;
      const float y = B.stickY[G.oneHand];
      if (std::abs(y) > 0.2f)
        G.dist = std::clamp(G.dist * std::exp(y * dt * 0.7f), kMinScreenDist, kMaxScreenDist);
    }
    s.pos = drag_point(hand) + G.offset;
  } else {
    const XrVector3f lp = G.hands[0].touch, rp = G.hands[1].touch;
    const float ratio = vlen(rp - lp) / G.span0;
    s.width = std::clamp(G.screenStart.width * ratio, kMinScreenWidth, kMaxScreenWidth);
    s.pos = G.screenStart.pos + ((lp + rp) * 0.5f - G.mid0);
  }
  s.orientation = facing(s.pos, B.head);
}

// Engaging and letting go, the same for both targets: one engaged hand
// drags alone, two scale (and turn the arena). False: nothing held.
bool update_holds(XrVector3f targetPos) {
  const bool l = G.hands[0].engaged, r = G.hands[1].engaged;
  if (l && r) {
    if (!G.twoHands)
      begin_two_hands();
  } else if (l || r) {
    // Letting go of one of two hands carries on with the other alone.
    if (G.oneHand != (l ? 0 : 1))
      begin_one_hand(l ? 0 : 1, targetPos);
  } else {
    end_grab();
    return false;
  }
  return true;
}

void engage(Hand& hand) {
  hand.engaged = true;
  hand.direct = hand.inside;
}

// The picture's point in the game's frame (0..1), off its letterbox; false
// when it is in the bars.
bool picture_to_frame(float u, float v, float& x, float& y) {
  const auto& st = g_streams[kScreen];
  const uint32_t cw = g_contentW, ch = g_contentH;
  if (st.width == 0 || st.height == 0 || cw == 0 || ch == 0) {
    x = u;
    y = v;
    return true;
  }
  const auto vp = webgpu::calculate_present_viewport(st.width, st.height, cw, ch);
  x = (u * static_cast<float>(st.width) - vp.left) / vp.width;
  y = (v * static_cast<float>(st.height) - vp.top) / vp.height;
  return x >= 0.f && x <= 1.f && y >= 0.f && y <= 1.f;
}

// The virtual screen, per display frame. A laser on the picture points at
// it for the game and clicks with the trigger or a pinch; the bar under it
// (pointed at or touched) takes hold of it with a grip, trigger or pinch,
// and while one hand holds it the other joins by pressing with its laser
// anywhere on the screen, to resize it.
void update_screen(bool active) {
  const ScreenPose& screen = screen_pose();
  const float hw = screen.width * 0.5f, hh = screen.width * screen_aspect() * 0.5f;
  for (Hand& hand : G.hands) {
    hand.inside = hand.valid && in_bar_box(screen, hand.touch);
    hand.hit = -1.f;
    hand.on = Hand::On::None;
    float t;
    XrVector3f l;
    if (hand.valid && !hand.inside && hit_screen_plane(screen, hand.origin, hand.dir, t, l)) {
      if (std::abs(l.x) <= hw && std::abs(l.y) <= hh) {
        hand.on = Hand::On::Picture;
        hand.u = l.x / (2.f * hw) + 0.5f;
        hand.v = 0.5f - l.y / (2.f * hh);
      } else if (on_bar(screen, l)) {
        hand.on = Hand::On::Bar;
      }
      if (hand.on != Hand::On::None)
        hand.hit = t;
    }
    if (hand.inside)
      hand.on = Hand::On::Bar;
  }
  B.reelHand = -1;
  const auto down = [](const Hand& h) { return h.valid && (h.held || h.sel); };
  const auto pressed = [](const Hand& h) { return h.valid && ((h.held && !h.wasHeld) || (h.sel && !h.wasSel)); };
  // Clicks are the trigger's or a pinch's; a controller's grip only takes
  // hold of the screen.
  const auto clickDown = [](const Hand& h) { return h.valid && (h.tracked ? h.held : h.sel); };
  const auto clickPressed = [](const Hand& h) {
    return h.valid && (h.tracked ? h.held && !h.wasHeld : h.sel && !h.wasSel);
  };
  for (Hand& hand : G.hands) {
    hand.engaged &= active && down(hand);
    hand.clicking &= active && clickDown(hand);
  }
  if (active) {
    for (Hand& hand : G.hands)
      if (pressed(hand) && hand.on == Hand::On::Bar)
        engage(hand);
    for (int h = 0; h < 2; ++h) {
      Hand& hand = G.hands[h];
      if (!pressed(hand) || hand.engaged)
        continue;
      if (G.hands[1 - h].engaged && hand.on != Hand::On::None) {
        hand.clicking = false;
        engage(hand);
      } else if (hand.on == Hand::On::Picture && clickPressed(hand)) {
        hand.clicking = true;
      }
    }
  }
  // The hand pointing for the game: a clicking one, or else the one that
  // already was while it stays on the picture, or else either on it.
  static int pointer = -1;
  {
    const auto pointing = [](const Hand& h) { return h.on == Hand::On::Picture && !h.engaged; };
    int next = -1;
    for (int h = 0; h < 2; ++h)
      if (G.hands[h].clicking && (next < 0 || h == pointer))
        next = h;
    if (next < 0 && pointer >= 0 && pointing(G.hands[pointer]))
      next = pointer;
    for (int h = 0; h < 2 && next < 0; ++h)
      if (pointing(G.hands[h]))
        next = h;
    pointer = next;
  }
  {
    ScreenPointer p;
    if (active && pointer >= 0 && G.oneHand < 0 && !G.twoHands) {
      const Hand& hand = G.hands[pointer];
      float x, y;
      // A click keeps the pointer even past the picture's edge, held there.
      p.valid = picture_to_frame(hand.u, hand.v, x, y) || hand.clicking;
      p.x = std::clamp(x, 0.f, 1.f);
      p.y = std::clamp(y, 0.f, 1.f);
      p.pressed = hand.clicking;
    }
    std::lock_guard lock{g_padMutex};
    g_screenPointer = p;
  }
  if (!active) {
    end_grab();
    return;
  }
  if (!update_holds(screen.pos))
    return;
  place_screen();
  if (G.oneHand >= 0 && !G.hands[G.oneHand].direct && !G.hands[G.oneHand].tracked)
    B.reelHand = G.oneHand;
}

// Per display frame. `active`: the target shows on the headset and can be
// placed (a paused fight's arena, or the screen).
void update_grab(Target target, bool active) {
  if (target != G.target) {
    for (Hand& hand : G.hands) {
      hand.engaged = false;
      hand.clicking = false;
      hand.on = Hand::On::None;
    }
    end_grab();
    G.target = target;
  }
  if (target == Target::Screen) {
    update_screen(active);
    return;
  }
  {
    std::lock_guard lock{g_padMutex};
    g_screenPointer = {};
  }
  ArenaPose a = arena_pose();
  for (Hand& hand : G.hands) {
    hand.inside = hand.valid && in_grab_box(a, hand.touch);
    hand.hit = hand.valid && !hand.inside ? hit_arena(a, hand.origin, hand.dir) : -1.f;
  }
  B.reelHand = -1;
  if (!active) {
    for (Hand& hand : G.hands)
      hand.engaged = false;
    end_grab();
    return;
  }
  const auto pressed = [](const Hand& h) { return h.valid && h.held && !h.wasHeld; };
  for (Hand& hand : G.hands)
    hand.engaged &= hand.valid && hand.held;
  // A press inside the box or with the laser on the arena takes hold of it;
  // with one hand already holding, a press of the other joins it anywhere
  // (also when both press together). Inside the box the hand drags by its
  // touch point, outside along its laser, until it lets go.
  for (Hand& hand : G.hands)
    if (pressed(hand) && (hand.inside || hand.hit >= 0.f))
      engage(hand);
  for (int h = 0; h < 2; ++h)
    if (pressed(G.hands[h]) && !G.hands[h].engaged && G.hands[1 - h].engaged)
      engage(G.hands[h]);
  if (!update_holds(a.pos))
    return;
  if (G.oneHand >= 0) {
    a.pos = drag_point(G.hands[G.oneHand]) + G.offset;
  } else {
    const XrVector3f lp = G.hands[0].touch, rp = G.hands[1].touch;
    const XrVector3f span = rp - lp;
    a.scale = std::clamp(G.start.scale * vlen(span) / G.span0, kMinArenaScale, kMaxArenaScale);
    const float ratio = a.scale / G.start.scale;
    const float turn = std::atan2(-span.z, span.x) - G.heading0;
    a.yaw = G.start.yaw + turn;
    a.pos = (lp + rp) * 0.5f + rot_y((G.start.pos - G.mid0) * ratio, turn);
  }
  set_arena_pose(a);
}

// ---------------------------------------------------------------- XR thread: lasers

// Where the HUD quad goes, in the room. It moves, turns and scales with the
// arena.
void hud_placement(XrPosef& pose, XrExtent2Df& size) {
  const auto& hud = g_streams[kHud];
  const ArenaPose arena = arena_pose();
  // The HUD follows the player's resizes, not the stage's own scale.
  float k;
  {
    std::lock_guard lock{g_arenaMutex};
    k = arena.scale / (g_defaultArena.scale * g_stageScale);
  }
  const float width = env_float("AURORA_XR_HUD_WIDTH", 0.5f) * k;
  size = {width, hud.width > 0 ? width * static_cast<float>(hud.height) / static_cast<float>(hud.width) : width};
  // Above the arena's back edge, like a scoreboard. Quad layers draw over
  // the 3D view, so the HUD's bottom edge (where the damage meters are)
  // clears the stage's highest floor by about a fighter's height; on low
  // stages it keeps AURORA_XR_HUD_HEIGHT (its center above the arena).
  float height = env_float("AURORA_XR_HUD_HEIGHT", 0.3f) * k;
  float top, centerY;
  {
    std::lock_guard lock{g_arenaMutex};
    top = g_stageTop;
    centerY = g_arenaCenter[1];
  }
  if (!std::isnan(top) && !std::getenv("AURORA_XR_HUD_HEIGHT")) {
    const float clear = env_float("AURORA_XR_HUD_CLEARANCE", 40.f); // game units
    height = std::max(height, (top + clear - centerY) * arena.scale + size.height * 0.5f);
  }
  pose.orientation = yaw_quat(arena.yaw);
  pose.position = arena.pos + rot_y({0.f, height, -0.15f * k}, arena.yaw);
}

// AURORA_XR_DUMP: the left eye's view and the HUD quad, so a dump of the 3D
// image can be composited with the HUD offline (one line each:
// position xyz, orientation xyzw, then fov angles or quad size).
void write_dump_layout(const std::array<XrView, 2>& views) {
  XrPosef hud{};
  XrExtent2Df size{};
  hud_placement(hud, size);
  if (FILE* f = std::fopen((B.dumpDir + "/xr_layout.txt").c_str(), "w")) {
    const XrPosef& e = views[0].pose;
    const XrFovf& fov = views[0].fov;
    std::fprintf(f, "eye %f %f %f %f %f %f %f %f %f %f %f\n", e.position.x, e.position.y, e.position.z,
                 e.orientation.x, e.orientation.y, e.orientation.z, e.orientation.w, fov.angleLeft, fov.angleRight,
                 fov.angleUp, fov.angleDown);
    std::fprintf(f, "hud %f %f %f %f %f %f %f %f %f\n", hud.position.x, hud.position.y, hud.position.z,
                 hud.orientation.x, hud.orientation.y, hud.orientation.z, hud.orientation.w, size.width, size.height);
    std::fclose(f);
  }
}

// Static swapchain uploads whose copies have finished: free what they used.
// `all`: wait for the rest too (teardown, after the queue is idle).
void free_finished_uploads(bool all = false) {
  std::erase_if(B.uploads, [all](const auto& u) {
    if (!all && vkGetFenceStatus(B.dev, u.fence) != VK_SUCCESS)
      return false;
    vkDestroyFence(B.dev, u.fence, nullptr);
    vkFreeCommandBuffers(B.dev, B.pool, 1, &u.cmd);
    vkDestroyBuffer(B.dev, u.buf, nullptr);
    vkFreeMemory(B.dev, u.mem, nullptr);
    return true;
  });
}

// Fill a one-image swapchain once; the runtime keeps showing it.
bool create_static_swapchain(XrSwapchain& out, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba) {
  XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  ci.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
  ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
  ci.format = B.swapFormat;
  ci.sampleCount = 1;
  ci.width = w;
  ci.height = h;
  ci.faceCount = 1;
  ci.arraySize = 1;
  ci.mipCount = 1;
  if (XR_FAILED(xrCreateSwapchain(B.session, &ci, &out))) {
    ci.createFlags = 0;
    XR_TRY(xrCreateSwapchain(B.session, &ci, &out));
  }
  uint32_t n = 0;
  XR_TRY(xrEnumerateSwapchainImages(out, 0, &n, nullptr));
  std::vector<XrSwapchainImageVulkan2KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
  XR_TRY(xrEnumerateSwapchainImages(out, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data())));

  VkBuffer buf = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  const auto upload = [&]() -> bool {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = rgba.size();
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VK_TRY(vkCreateBuffer(B.dev, &bci, nullptr, &buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(B.dev, buf, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex =
        find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX)
      return false;
    VK_TRY(vkAllocateMemory(B.dev, &mai, nullptr, &mem));
    VK_TRY(vkBindBufferMemory(B.dev, buf, mem, 0));
    void* p = nullptr;
    VK_TRY(vkMapMemory(B.dev, mem, 0, rgba.size(), 0, &p));
    std::memcpy(p, rgba.data(), rgba.size());
    vkUnmapMemory(B.dev, mem);

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XR_TRY(xrAcquireSwapchainImage(out, &ai, &index));
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    XR_TRY(xrWaitSwapchainImage(out, &wi));

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = B.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VK_TRY(vkAllocateCommandBuffers(B.dev, &cai, &cmd));
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_TRY(vkBeginCommandBuffer(cmd, &cbi));
    VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = imgs[index].image;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &bar);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, buf, imgs[index].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    // The layout OpenXR expects a released color swapchain image in.
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &bar);
    VK_TRY(vkEndCommandBuffer(cmd));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_TRY(vkCreateFence(B.dev, &fci, nullptr, &fence));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    // Not waited on: this queue also carries the stream releases, which
    // wait on the render worker, which can wait on this thread. The staging
    // buffer goes once the fence signals.
    VK_TRY(vkQueueSubmit(B.queue, 1, &si, fence));
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XR_TRY(xrReleaseSwapchainImage(out, &ri));
    B.uploads.push_back({buf, mem, cmd, fence});
    buf = VK_NULL_HANDLE;
    mem = VK_NULL_HANDLE;
    cmd = VK_NULL_HANDLE;
    fence = VK_NULL_HANDLE;
    return true;
  };
  const bool ok = upload();
  if (fence)
    vkDestroyFence(B.dev, fence, nullptr);
  if (cmd)
    vkFreeCommandBuffers(B.dev, B.pool, 1, &cmd);
  if (buf)
    vkDestroyBuffer(B.dev, buf, nullptr);
  if (mem)
    vkFreeMemory(B.dev, mem, nullptr);
  return ok;
}

constexpr uint32_t kBeamW = 16, kBeamH = 4, kDotSize = 64, kBarW = 256, kBarH = 32;

// White, premultiplied, soft-edged: a beam (alpha across its width) and a
// round dot. The lasers tint them per frame (color scale/bias layers).
bool create_pointer_textures() {
  const bool srgb = B.swapFormat == VK_FORMAT_R8G8B8A8_SRGB;
  const auto texel = [srgb](std::vector<uint8_t>& px, size_t i, float alpha) {
    alpha = std::clamp(alpha, 0.f, 1.f);
    const float c = srgb ? (alpha <= 0.0031308f ? alpha * 12.92f : 1.055f * std::pow(alpha, 1.f / 2.4f) - 0.055f)
                         : alpha;
    px[i * 4 + 0] = px[i * 4 + 1] = px[i * 4 + 2] = static_cast<uint8_t>(std::lround(c * 255.f));
    px[i * 4 + 3] = static_cast<uint8_t>(std::lround(alpha * 255.f));
  };
  std::vector<uint8_t> beam(kBeamW * kBeamH * 4);
  for (uint32_t y = 0; y < kBeamH; ++y)
    for (uint32_t x = 0; x < kBeamW; ++x) {
      const float u = std::abs((x + 0.5f) / kBeamW * 2.f - 1.f);
      texel(beam, y * kBeamW + x, 1.f - u * u);
    }
  std::vector<uint8_t> dot(kDotSize * kDotSize * 4);
  for (uint32_t y = 0; y < kDotSize; ++y)
    for (uint32_t x = 0; x < kDotSize; ++x) {
      const float dx = (x + 0.5f) / kDotSize * 2.f - 1.f, dy = (y + 0.5f) / kDotSize * 2.f - 1.f;
      const float r = std::sqrt(dx * dx + dy * dy);
      texel(dot, y * kDotSize + x, std::clamp((1.f - r) / 0.2f, 0.f, 1.f));
    }
  // The bar: a pill, round at the ends.
  std::vector<uint8_t> bar(kBarW * kBarH * 4);
  for (uint32_t y = 0; y < kBarH; ++y)
    for (uint32_t x = 0; x < kBarW; ++x) {
      const float r = kBarH * 0.5f;
      const float px = std::clamp(x + 0.5f, r, kBarW - r), py = r;
      const float d = std::hypot(x + 0.5f - px, y + 0.5f - py);
      texel(bar, y * kBarW + x, std::clamp(r - d, 0.f, 1.5f) / 1.5f);
    }
  return create_static_swapchain(B.beamSwapchain, kBeamW, kBeamH, beam) &&
         create_static_swapchain(B.dotSwapchain, kDotSize, kDotSize, dot) &&
         create_static_swapchain(B.barSwapchain, kBarW, kBarH, bar);
}

constexpr size_t kMaxPointerLayers = 5; // a beam and a dot per hand, and the screen's bar

struct PointerLayers {
  std::array<XrCompositionLayerQuad, kMaxPointerLayers> quads;
  std::array<XrCompositionLayerColorScaleBiasKHR, kMaxPointerLayers> tints;
  uint32_t count = 0;
};

void add_pointer_quad(PointerLayers& out, XrSwapchain swapchain, uint32_t w, uint32_t h, XrVector3f center,
                      XrQuaternionf orientation, XrExtent2Df size, XrColor4f color) {
  auto& q = out.quads[out.count];
  q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
  q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  q.space = B.space;
  q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  q.subImage.swapchain = swapchain;
  q.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(w), static_cast<int32_t>(h)}};
  q.pose = {orientation, center};
  q.size = size;
  if (B.hasColorScaleBias) {
    auto& t = out.tints[out.count];
    t = {XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR};
    // Premultiplied: the color scales with the alpha.
    t.colorScale = {color.r * color.a, color.g * color.a, color.b * color.a, color.a};
    q.next = &t;
  }
  ++out.count;
}

// The screen's grab bar: faint, brighter while a laser or hand is on it,
// tinted while the screen is held.
void add_screen_bar(PointerLayers& out) {
  if (!B.barSwapchain)
    return;
  const ScreenPose& s = screen_pose();
  const BarRect b = bar_rect(s);
  const bool held = G.target == Target::Screen && (G.oneHand >= 0 || G.twoHands);
  const bool hover = std::any_of(G.hands.begin(), G.hands.end(), [](const Hand& h) { return h.on == Hand::On::Bar; });
  const XrColor4f color = held    ? XrColor4f{0.45f, 0.85f, 1.f, 1.f}
                          : hover ? XrColor4f{1.f, 1.f, 1.f, 0.95f}
                                  : XrColor4f{0.85f, 0.88f, 0.92f, 0.55f};
  add_pointer_quad(out, B.barSwapchain, kBarW, kBarH, s.pos + qrot(s.orientation, {0.f, b.y, 0.f}), s.orientation,
                   {b.hw * 2.f, b.hh * 2.f}, color);
}

// `onTargetOnly`: skip the hands that neither point at the target nor hold
// it. Adds to what `out` has.
void build_pointer_layers(PointerLayers& out, bool onTargetOnly = false) {
  if (!B.beamSwapchain || !B.dotSwapchain)
    return;
  for (int h = 0; h < 2; ++h) {
    const Hand& hand = G.hands[h];
    if (!hand.valid || (onTargetOnly && !hand.engaged && !hand.clicking && !hand.inside && hand.hit < 0.f))
      continue;
    const XrColor4f color = hand.engaged                     ? XrColor4f{1.f, 0.8f, 0.3f, 1.f}
                            : hand.clicking                  ? XrColor4f{1.f, 1.f, 1.f, 1.f}
                            : hand.inside || hand.hit >= 0.f ? XrColor4f{0.45f, 0.85f, 1.f, 1.f}
                                                             : XrColor4f{0.85f, 0.9f, 1.f, 0.5f};
    if (hand.engaged ? hand.direct : hand.inside) {
      // In the box: a dot at the touch point, facing the head.
      const XrVector3f dz = vnorm(B.head - hand.touch);
      const XrVector3f dx = vnorm(vcross({0.f, 1.f, 0.f}, dz));
      if (vlen(dx) > 0.5f)
        add_pointer_quad(out, B.dotSwapchain, kDotSize, kDotSize, hand.touch, quat_from_axes(dx, vcross(dz, dx), dz),
                         {0.012f, 0.012f}, color);
      continue;
    }
    const bool dragging = G.oneHand == h;
    const float length = dragging ? G.dist : hand.hit >= 0.f ? hand.hit : 1.f;
    // The beam: a thin quad along the laser, turned about it to face the head.
    const XrVector3f end = hand.origin + hand.dir * length;
    const XrVector3f center = hand.origin + hand.dir * (length * 0.5f);
    const XrVector3f toHead = B.head - center;
    const XrVector3f z = vnorm(toHead - hand.dir * vdot(toHead, hand.dir));
    if (vlen(z) > 0.5f)
      add_pointer_quad(out, B.beamSwapchain, kBeamW, kBeamH, center,
                       quat_from_axes(vcross(hand.dir, z), hand.dir, z), {0.004f, length}, color);
    if (hand.hit < 0.f && !dragging)
      continue;
    // The dot where the laser meets the target, facing the head; on the
    // screen's picture a smaller one, smaller still while it clicks.
    const XrVector3f dz = vnorm(B.head - end);
    const XrVector3f dx = vnorm(vcross({0.f, 1.f, 0.f}, dz));
    const float size = hand.on != Hand::On::Picture ? 0.02f : hand.clicking ? 0.01f : 0.014f;
    if (vlen(dx) > 0.5f)
      add_pointer_quad(out, B.dotSwapchain, kDotSize, kDotSize, end, quat_from_axes(dx, vcross(dz, dx), dz),
                       {size, size}, color);
  }
}

// ---------------------------------------------------------------- XR thread: placing the stage
//
// While the game holds a fight before it starts (aurora_xr_set_placing), the
// how-to cards float above and behind where the arena starts, and the button
// legend just below its front edge, both turned to face the starting head
// position. They stay put while the player moves the arena.

// Premultiplied for the compositor, in the swapchain's encoding.
bool upload_placing_image(PlacingImage& img) {
  const bool srgb = B.swapFormat == VK_FORMAT_R8G8B8A8_SRGB;
  std::array<float, 256> decode;
  for (int i = 0; i < 256; ++i) {
    const float c = i / 255.f;
    decode[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
  }
  constexpr int kSteps = 4096;
  std::vector<uint8_t> encode(kSteps + 1);
  for (int i = 0; i <= kSteps; ++i) {
    const float l = static_cast<float>(i) / kSteps;
    const float c = !srgb ? l : l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.f / 2.4f) - 0.055f;
    encode[i] = static_cast<uint8_t>(std::lround(c * 255.f));
  }
  std::vector<uint8_t> px(img.rgba.size());
  for (size_t i = 0; i + 3 < px.size(); i += 4) {
    const float a = img.rgba[i + 3] / 255.f;
    for (int c = 0; c < 3; ++c)
      px[i + c] = encode[static_cast<int>(std::lround(decode[img.rgba[i + c]] * a * kSteps))];
    px[i + 3] = img.rgba[i + 3];
  }
  return create_static_swapchain(img.swapchain, img.width, img.height, px);
}

constexpr size_t kMaxPlacingLayers = 2 + 3; // the cards, the legend, the clips

struct PlacingLayers {
  std::array<XrCompositionLayerQuad, kMaxPlacingLayers> quads;
  uint32_t count = 0;
};

void build_placing_layers(PlacingLayers& out, int mode) {
  out.count = 0;
  const ArenaPose d = [] {
    std::lock_guard lock{g_arenaMutex};
    init_arena();
    return g_defaultArena;
  }();
  // Where each sits against the arena's starting position, and its width
  // (meters). AURORA_XR_PLACE_CARDS / _LEGEND: "dx,dy,dz,width".
  struct Spot {
    const char* env;
    XrVector3f offset;
    float width;
  };
  const std::array<Spot, 2> spots{{{"AURORA_XR_PLACE_CARDS", {0.f, 0.42f, -0.3f}, 0.85f},
                                    {"AURORA_XR_PLACE_LEGEND", {0.f, -0.17f, 0.12f}, 0.34f}}};
  // Soft head lock, sideways only: the layout turns about where the head
  // stood when the hold began, lagging behind once the head has turned past
  // a dead zone, and settles when it catches up. Height and pitch stay put.
  static auto last = std::chrono::steady_clock::time_point{};
  static float yaw = 0.f;
  static bool following = false;
  static XrVector3f pivot{};
  const auto now = std::chrono::steady_clock::now();
  const float dt = std::chrono::duration<float>(now - last).count();
  last = now;
  const XrVector3f fwd = qrot(B.headOrientation, {0.f, 0.f, -1.f});
  const float headYaw = std::atan2(-fwd.x, -fwd.z); // rot_y's sense
  if (dt > 0.25f) { // a new hold: start where the head looks
    yaw = headYaw;
    following = false;
    pivot = {B.head.x, 0.f, B.head.z};
  } else {
    constexpr float kPi = 3.14159265f;
    float diff = headYaw - yaw;
    diff -= 2.f * kPi * std::floor((diff + kPi) / (2.f * kPi));
    if (std::fabs(diff) > 20.f * kPi / 180.f)
      following = true;
    if (following) {
      yaw += diff * std::min(1.f, dt * 4.f);
      if (std::fabs(diff) < 1.f * kPi / 180.f)
        following = false;
    }
  }
  // Hands, if one is tracked (a controller put down), else controllers.
  const bool hands = G.hands[0].tracked || G.hands[1].tracked;
  std::lock_guard lock{g_placingMutex};
  const auto ready = [](PlacingImage& img, const char* what) {
    if (img.width == 0)
      return false;
    if (!img.swapchain && !upload_placing_image(img)) {
      Log.warn("Placing picture {} unavailable", what);
      img.width = 0;
      return false;
    }
    return true;
  };
  for (int which = mode >= 2 ? 0 : 1; which < 2; ++which) {
    // The hands' version when there is one, else the controllers'.
    PlacingImage* img = &g_placingImages[which];
    if (hands && g_placingImages[which + 2].width > 0)
      img = &g_placingImages[which + 2];
    if (!ready(*img, which == 0 ? "cards" : "legend"))
      continue;
    Spot spot = spots[which];
    if (const char* v = std::getenv(spot.env))
      std::sscanf(v, "%f,%f,%f,%f", &spot.offset.x, &spot.offset.y, &spot.offset.z, &spot.width);
    const XrVector3f pos = pivot + rot_y(d.pos + spot.offset, yaw);
    const XrQuaternionf turn = facing(pos, pivot);
    const XrExtent2Df size{spot.width, spot.width * static_cast<float>(img->height) / static_cast<float>(img->width)};
    auto& q = out.quads[out.count++];
    q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.space = B.space;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = img->swapchain;
    q.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(img->width), static_cast<int32_t>(img->height)}};
    q.pose = {turn, pos};
    q.size = size;
    if (which != 0)
      continue;
    // The clips for these cards, each over its picture, a hair in front.
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const size_t first = img == &g_placingImages[2] ? 3 : 0;
    for (size_t c = first; c < first + 3; ++c) {
      PlacingClip& clip = g_placingClips[c];
      if (clip.frames <= 0 || !ready(clip.atlas, "clip"))
        continue;
      const int period = std::max(1, 2 * (clip.frames - 1));
      const int step = static_cast<int>(std::fmod(t * clip.fps, static_cast<double>(period)));
      const int frame = step < clip.frames ? step : period - step;
      const float u = (clip.x + clip.w * 0.5f) / static_cast<float>(img->width) - 0.5f;
      const float v = 0.5f - (clip.y + clip.h * 0.5f) / static_cast<float>(img->height);
      auto& cq = out.quads[out.count++];
      cq = q;
      cq.layerFlags = 0; // opaque
      cq.subImage.swapchain = clip.atlas.swapchain;
      cq.subImage.imageRect = {{(frame % clip.cols) * clip.frameW, (frame / clip.cols) * clip.frameH},
                               {clip.frameW, clip.frameH}};
      cq.pose.position = pos + qrot(turn, {u * size.width, v * size.height, 0.002f});
      cq.size = {size.width * clip.w / static_cast<float>(img->width),
                 size.height * clip.h / static_cast<float>(img->height)};
    }
  }
}

// Full VR stops passthrough outright (the cameras stop, not just the layer),
// mixed reality starts it again.
void sync_passthrough() {
  const bool want = g_passthroughWanted;
  if (!B.passthrough || want == B.passthroughRunning)
    return;
  const auto fn = want ? B.startPassthrough : B.pausePassthrough;
  if (!fn)
    return;
  const XrResult r = fn(B.passthrough);
  if (XR_FAILED(r)) {
    Log.warn("Passthrough {} failed: XrResult {}", want ? "start" : "pause", static_cast<int>(r));
    return;
  }
  B.passthroughRunning = want;
  Log.info("Passthrough {}", want ? "on" : "off");
}

// The Guardian boundary is hidden while passthrough shows the room (the
// runtime allows nothing else) and comes back in full VR.
// XR_META_boundary_visibility; AURORA_XR_BOUNDARY=1 keeps it.
void sync_boundary() {
#ifdef XR_META_boundary_visibility
  if (!B.requestBoundaryVisibility || !B.running)
    return;
  const bool want = B.passthroughRunning && g_passthroughWanted;
  // Refused (no passthrough layer shown yet): try again about once a second.
  static uint64_t retryAt = 0;
  if (want == B.boundarySuppressed || B.framesShown < retryAt)
    return;
  B.boundarySuppressed = want;
  const XrResult r = B.requestBoundaryVisibility(
      B.session, want ? XR_BOUNDARY_VISIBILITY_SUPPRESSED_META : XR_BOUNDARY_VISIBILITY_NOT_SUPPRESSED_META);
  if (r == XR_SUCCESS)
    Log.info("Boundary {} requested", want ? "suppression" : "restore");
  if (r == XR_BOUNDARY_VISIBILITY_SUPPRESSION_NOT_ALLOWED_META || XR_FAILED(r)) {
    static int warnings = 0;
    if (warnings++ < 3)
      Log.warn("Boundary {} refused: XrResult {}", want ? "suppression" : "restore", static_cast<int>(r));
    B.boundarySuppressed = !want;
    retryAt = B.framesShown + 72;
  }
#endif
}

bool render_xr_frame() {
  XrFrameState fs{XR_TYPE_FRAME_STATE};
  XR_TRY(xrWaitFrame(B.session, nullptr, &fs));
  free_finished_uploads();
  if (const int per = g_displayPerGameFrame; per > 0 && ++B.displayFrame % static_cast<uint64_t>(per) == 0) {
    {
      std::lock_guard lock{g_paceMutex};
      ++g_paceTick;
      g_lastTickFrame = B.displayFrame;
    }
    g_paceCv.notify_all();
  }
  update_input();
  publish_views(fs.predictedDisplayTime);
  XR_TRY(xrBeginFrame(B.session, nullptr));

  const auto now = std::chrono::steady_clock::now();
  // Hand finished images to the runtime, then line up the next ones for the
  // render worker.
  for (int i = 0; i < kStreamCount; ++i) {
    bool released = false;
    if (!release_ready(g_streams[i], released) || !acquire_ahead(g_streams[i]))
      return false;
    B.releasedSinceStats[i] += released ? 1 : 0;
  }
  // A fight is on while 3D frames keep coming; otherwise the virtual screen.
  const auto& stereo = g_streams[kStereo];
  const auto& hud = g_streams[kHud];
  const bool fight = stereo.haveImage && now - stereo.lastRelease < std::chrono::milliseconds(250);
  // Paused, or held before it starts for placing the stage.
  const int placing = g_placing.load();
  const bool held = g_fightPaused || placing > 0;
  {
    // Each 3D image is due for g_displayPerGameFrame display frames under
    // lock-step pacing; one shown longer is late. Counted once per image.
    static uint64_t lastReleases = 0;
    static int shownFor = 0;
    const int due = g_displayPerGameFrame.load();
    if (stereo.releases != lastReleases) {
      if (fight && !held && shownFor > 0)
        ++B.shownHistogram[std::min(shownFor, 4) - 1];
      lastReleases = stereo.releases;
      shownFor = 0;
    }
    if (fight && due > 0 && !held && ++shownFor == due + 1)
      ++g_stereoLate;
  }
  // Held fights place the arena; menus place the screen.
  const bool screenShown = !fight && g_streams[kScreen].haveImage;
  const bool pointing = fight && held && B.focused;
  const bool screenPointing = screenShown && B.focused;
  locate_hands(fs.predictedDisplayTime);
  update_grab(fight ? Target::Arena : Target::Screen, pointing || screenPointing);
  // On the screen the grips stay Z unless a laser is on it.
  const bool onScreen = screenPointing && std::any_of(G.hands.begin(), G.hands.end(), [](const Hand& h) {
                          return h.engaged || h.clicking || h.inside || h.hit >= 0.f;
                        });
  B.pointerMode = pointing || onScreen;
  B.triggersClick = onScreen;

  std::array<const XrCompositionLayerBaseHeader*, 3 + kMaxPlacingLayers + kMaxPointerLayers> layers{};
  uint32_t layerCount = 0;
  XrCompositionLayerPassthroughFB ptLayer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
  sync_passthrough();
  sync_boundary();
  if (B.passthroughLayer && B.passthroughRunning && g_passthroughWanted) {
    ptLayer.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    ptLayer.layerHandle = B.passthroughLayer;
    layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&ptLayer);
  }
  std::array<XrCompositionLayerProjectionView, 2> projViews{};
  XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerQuad hudQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  XrCompositionLayerQuad screenQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  PointerLayers pointers;
  PlacingLayers placingLayers;
  if (fs.shouldRender && fight) {
    // Premultiplied alpha (no UNPREMULTIPLIED bit): the arena's coverage
    // hides the room, effects outside it add light over passthrough.
    // Multiview: one layer per eye. Otherwise the eyes sit side by side.
    const int32_t eyeW = static_cast<int32_t>(stereo.layers > 1 ? stereo.width : stereo.width / 2);
    for (int i = 0; i < 2; ++i) {
      projViews[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
      projViews[i].pose = stereo.shownViews[i].pose;
      projViews[i].fov = stereo.shownViews[i].fov;
      projViews[i].subImage.swapchain = stereo.swapchain;
      const int32_t x = stereo.layers > 1 ? 0 : i * eyeW;
      // Dynamic resolution: only the rendered corner, which the compositor
      // stretches over the eye's field of view.
      const bool part = stereo.shownRect[0] > 0 && stereo.shownRect[1] > 0;
      projViews[i].subImage.imageRect = {
          {x, 0}, {part ? stereo.shownRect[0] : eyeW, part ? stereo.shownRect[1] : static_cast<int32_t>(stereo.height)}};
      projViews[i].subImage.imageArrayIndex = stereo.layers > 1 ? static_cast<uint32_t>(i) : 0;
    }
    proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    proj.space = B.space;
    proj.viewCount = 2;
    proj.views = projViews.data();
    // No HUD while the stage is held for placing, cards open or not.
    const bool showHud = hud.haveImage && now - hud.lastRelease < std::chrono::milliseconds(250) && placing == 0;
    if (showHud) {
      hudQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
      hudQuad.space = B.space;
      hudQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
      hudQuad.subImage.swapchain = hud.swapchain;
      hudQuad.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(hud.width), static_cast<int32_t>(hud.height)}};
      hud_placement(hudQuad.pose, hudQuad.size);
    }
    // Mixed reality: the HUD goes under the 3D view, which is see-through
    // wherever nothing drew, so fighters passing over it hide it. Nothing
    // of the stage reaches it (it sits above the highest floor). Full VR
    // draws the whole stage and its sky, so the HUD stays on top there.
    // AURORA_XR_HUD_ON_TOP=1 keeps it on top in mixed reality too.
    const bool hudUnder = g_passthroughWanted && !env_flag("AURORA_XR_HUD_ON_TOP", false);
    if (showHud && hudUnder)
      layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hudQuad);
    layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
    if (showHud && !hudUnder)
      layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hudQuad);
    if (placing > 0) {
      build_placing_layers(placingLayers, placing);
      for (uint32_t i = 0; i < placingLayers.count; ++i)
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&placingLayers.quads[i]);
    }
    if (pointing) {
      build_pointer_layers(pointers);
      for (uint32_t i = 0; i < pointers.count; ++i)
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&pointers.quads[i]);
    }
    ++B.fightFrames;
  } else if (fs.shouldRender && screenShown) {
    // A swapchain with no new release shows its last released image, so the
    // screen keeps its picture on display frames the game did not produce.
    const auto& screen = g_streams[kScreen];
    const ScreenPose& pose = screen_pose();
    screenQuad.space = B.space;
    screenQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    screenQuad.subImage.swapchain = screen.swapchain;
    screenQuad.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(screen.width), static_cast<int32_t>(screen.height)}};
    screenQuad.pose = {pose.orientation, pose.pos};
    screenQuad.size = {pose.width, pose.width * screen_aspect()};
    layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&screenQuad);
    add_screen_bar(pointers);
    if (onScreen)
      build_pointer_layers(pointers, true);
    for (uint32_t i = 0; i < pointers.count; ++i)
      layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&pointers.quads[i]);
  }
  XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
  fei.displayTime = fs.predictedDisplayTime;
  fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  fei.layerCount = layerCount;
  fei.layers = layers.data();
  XR_TRY(xrEndFrame(B.session, &fei));
  ++B.framesShown;

  const double secs = std::chrono::duration<double>(now - B.statsStart).count();
  if (secs >= 10.0) {
    Log.info("{:.1f} display fps ({:.0f}% 3D); frames/s released: screen {:.1f}, 3D {:.1f}, HUD {:.1f}; "
             "3D images shown 1/2/3/4+ display frames: {}/{}/{}/{}; presented {} display frames after the tick "
             "({} held)",
             B.framesShown / secs, 100.0 * B.fightFrames / std::max<uint64_t>(B.framesShown, 1),
             B.releasedSinceStats[kScreen] / secs, B.releasedSinceStats[kStereo] / secs,
             B.releasedSinceStats[kHud] / secs, B.shownHistogram[0], B.shownHistogram[1], B.shownHistogram[2],
             B.shownHistogram[3], g_present.latency, g_present.held);
    B.shownHistogram = {};
    g_present.held = 0;
    B.framesShown = 0;
    B.fightFrames = 0;
    B.releasedSinceStats = {};
    B.statsStart = now;
  }
  return true;
}

// Returns false when the thread should exit.
bool poll_events() {
  XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
  while (xrPollEvent(B.instance, &ev) == XR_SUCCESS) {
    if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
      const auto state = reinterpret_cast<const XrEventDataSessionStateChanged&>(ev).state;
      Log.info("Session state {}", static_cast<int>(state));
      if (state == XR_SESSION_STATE_READY) {
        XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
        bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        XR_TRY(xrBeginSession(B.session, &bi));
        B.running = true;
        request_refresh_rate();
        request_performance_levels();
        update_pacing();
        B.statsStart = std::chrono::steady_clock::now();
      } else if (state == XR_SESSION_STATE_STOPPING) {
        XR_TRY(xrEndSession(B.session));
        B.running = false;
      } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
        B.running = false;
        return false;
      }
      B.focused = state == XR_SESSION_STATE_FOCUSED;
      if (!B.focused) {
        std::lock_guard lock{g_padMutex};
        g_padValid = false;
      }
      g_sessionRunning = B.running;
    } else if (ev.type == XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB) {
      update_pacing();
    } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
      return false;
#ifdef XR_META_boundary_visibility
    } else if (ev.type == XR_TYPE_EVENT_DATA_BOUNDARY_VISIBILITY_CHANGED_META) {
      const auto vis = reinterpret_cast<const XrEventDataBoundaryVisibilityChangedMETA&>(ev).boundaryVisibility;
      Log.info("Boundary {}", vis == XR_BOUNDARY_VISIBILITY_SUPPRESSED_META ? "hidden" : "shown");
#endif
    }
    ev = {XR_TYPE_EVENT_DATA_BUFFER};
  }
  return true;
}

bool setup() {
  // The OpenXR instance and system already exist (prepare_device), and Dawn's
  // device was created through them.
  if (!B.prepared || !adopt_dawn_device() || !size_stereo_stream() || !create_session())
    return false;
  for (auto& st : g_streams)
    if (!create_swapchain(st))
      return false;
  if (!create_input())
    Log.warn("Controller input unavailable");
  if (!create_pointer_textures())
    Log.warn("Laser pointers unavailable");
  if (!create_hand_trackers())
    Log.info("Hand tracking unavailable");
  if (const char* dir = std::getenv("AURORA_XR_DUMP"); dir != nullptr && *dir != '\0')
    B.dumpDir = dir;
  // Not every exit path calls aurora::shutdown, so stop the XR thread from an
  // exit handler too. It must be registered only now: exit handlers run in
  // reverse order, and the runtime and Vulkan driver libraries loaded above
  // register their own teardown, which has to run after ours.
  std::atexit([] { shutdown(); });
  return true;
}

// Destroy everything the XR thread created, newest first. Dawn owns the
// device and instance; the OpenXR instance outlives them (release_instance).
void teardown() {
  // Only our queue: Dawn may be submitting on its own, and vkDeviceWaitIdle
  // would need every queue externally synchronized.
  if (B.queue)
    vkQueueWaitIdle(B.queue);
  if (B.dev)
    free_finished_uploads(true);
  if (B.dev) {
    for (auto& st : g_streams) {
      for (int i = 0; i < static_cast<int>(st.slots.size()); ++i)
        retire_slot(st, st.slots[i], i);
    }
  }
  for (auto& sp : B.aimSpace) {
    if (sp)
      xrDestroySpace(sp);
    sp = XR_NULL_HANDLE;
  }
  for (auto& ht : B.handTrackers) {
    if (ht && B.destroyHandTracker)
      B.destroyHandTracker(ht);
    ht = XR_NULL_HANDLE;
  }
  for (XrSwapchain* sc : {&B.beamSwapchain, &B.dotSwapchain, &B.barSwapchain}) {
    if (*sc)
      xrDestroySwapchain(*sc);
    *sc = XR_NULL_HANDLE;
  }
  {
    std::lock_guard lock{g_placingMutex};
    for (auto& clip : g_placingClips) {
      if (clip.atlas.swapchain)
        xrDestroySwapchain(clip.atlas.swapchain);
      clip.atlas.swapchain = XR_NULL_HANDLE;
    }
    for (auto& img : g_placingImages) {
      if (img.swapchain)
        xrDestroySwapchain(img.swapchain);
      img.swapchain = XR_NULL_HANDLE;
    }
  }
  if (B.actionSet)
    xrDestroyActionSet(B.actionSet); // destroys its actions too
  B.actionSet = XR_NULL_HANDLE;
  {
    std::lock_guard lock{g_padMutex};
    g_padValid = false;
  }
  if (B.passthroughLayer && B.destroyPassthroughLayer)
    B.destroyPassthroughLayer(B.passthroughLayer);
  if (B.passthrough && B.destroyPassthrough)
    B.destroyPassthrough(B.passthrough);
  B.passthroughLayer = XR_NULL_HANDLE;
  B.passthrough = XR_NULL_HANDLE;
  // Dawn's textures over the swapchain images never touch them again: XR is
  // stopped (Phase::Failed or g_stop) before this runs.
  for (auto& st : g_streams) {
    if (st.swapchain)
      xrDestroySwapchain(st.swapchain);
    st.swapchain = XR_NULL_HANDLE;
  }
  if (B.space)
    xrDestroySpace(B.space);
  if (B.session)
    xrDestroySession(B.session);
  B.space = XR_NULL_HANDLE;
  B.session = XR_NULL_HANDLE;
  if (B.dev) {
    for (auto& st : g_streams) {
      for (auto& s : st.slots) {
        if (s.fence)
          vkDestroyFence(B.dev, s.fence, nullptr);
        if (s.cmd)
          vkFreeCommandBuffers(B.dev, B.pool, 1, &s.cmd);
        s.fence = VK_NULL_HANDLE;
        s.cmd = VK_NULL_HANDLE;
      }
      if (st.dumpBuf)
        vkDestroyBuffer(B.dev, st.dumpBuf, nullptr);
      if (st.dumpMem)
        vkFreeMemory(B.dev, st.dumpMem, nullptr);
      st.dumpBuf = VK_NULL_HANDLE;
      st.dumpMem = VK_NULL_HANDLE;
    }
    if (B.pool)
      vkDestroyCommandPool(B.dev, B.pool, nullptr);
    B.pool = VK_NULL_HANDLE;
  }
}

void thread_main() {
  if (!setup()) {
    Log.error("OpenXR unavailable; presenting to the window instead");
    g_phase = Phase::Failed;
    teardown();
    return;
  }
  g_phase = Phase::SlotsReady;
  while (!g_stop) {
    if (!poll_events())
      break;
    if (!B.running) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    if (!render_xr_frame())
      break;
  }
  g_sessionRunning = false;
  g_displayPerGameFrame = 0;
  g_paceCv.notify_all();
  Log.info("XR thread exiting");
  // Stop the render worker taking images, and let a frame it is drawing into
  // one finish, before the swapchains go.
  g_phase = Phase::Failed;
  for (int i = 0; i < 50; ++i) {
    bool drawing = false;
    {
      std::lock_guard lock{g_mutex};
      for (const auto& st : g_streams)
        for (const auto& s : st.slots)
          drawing |= s.state == SlotState::Rendering;
    }
    if (!drawing)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  teardown();
}

// ---------------------------------------------------------------- render worker: swapchain images

// Wraps every swapchain image as Dawn shared texture memory, once, after the
// XR thread created the swapchains.
bool wrap_swapchains_into_dawn() {
  for (auto& st : g_streams) {
    const VkImageCreateInfo ici = swapchain_image_info(st);
    for (auto& s : st.slots) {
      s.stm = wgpu::SharedTextureMemory::Acquire(dawn::native::vulkan::CreateSharedTextureMemoryFromVkImage(
          webgpu::g_device.Get(), reinterpret_cast<uint64_t>(s.image), &ici, st.name));
      if (!s.stm) {
        Log.error("Wrapping a {} swapchain image for Dawn failed", st.name);
        return false;
      }
      s.texture = s.stm.CreateTexture();
      if (!s.texture) {
        Log.error("SharedTextureMemory::CreateTexture failed ({})", st.name);
        return false;
      }
    }
  }
  Log.info("Swapchain images wrapped for Dawn: screen {}x{}, 3D {}x{}, HUD {}x{} ({} images each)",
           g_streams[kScreen].width, g_streams[kScreen].height, g_streams[kStereo].width, g_streams[kStereo].height,
           g_streams[kHud].width, g_streams[kHud].height, g_streams[kScreen].slots.size());
  return true;
}

// The oldest image the XR thread has ready, so images come back in the
// order they were acquired.
int take_slot_locked(Stream& st) {
  int best = -1;
  for (int i = 0; i < static_cast<int>(st.slots.size()); ++i)
    if (st.slots[i].state == SlotState::Free &&
        (best < 0 || st.slots[i].acquireSeq < st.slots[best].acquireSeq))
      best = i;
  return best;
}

// Waits up to `wait` for the XR thread to line up an image: it releases the
// previous one and acquires the next on its display-frame loop, which can
// land just after the render worker asks.
wgpu::Texture acquire_slot(Stream& st, std::chrono::milliseconds wait = std::chrono::milliseconds(10)) {
  int index;
  {
    std::unique_lock lock{g_mutex};
    g_slotFreeCv.wait_for(lock, wait, [&] { return take_slot_locked(st) >= 0 || g_phase != Phase::Imported; });
    index = take_slot_locked(st);
    if (index < 0)
      return {};
    st.slots[index].state = SlotState::Rendering;
  }
  Slot& s = st.slots[index];
  // xrWaitSwapchainImage already returned: no fences. The contents are not
  // kept, so the image starts UNDEFINED.
  wgpu::SharedTextureMemoryVkImageLayoutBeginState bs{};
  bs.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  bs.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  wgpu::SharedTextureMemoryBeginAccessDescriptor bd{};
  bd.nextInChain = &bs;
  // Dynamic resolution renders a corner of the stereo image; marked
  // initialized, the rest doesn't force Dawn to a full render area (the
  // compositor only reads the corner, through imageRect).
  bd.initialized = &st == &g_streams[kStereo] && g_dynres.on;
  if (s.stm.BeginAccess(s.texture, &bd) != wgpu::Status::Success) {
    Log.error("BeginAccess failed ({}); XR presentation disabled", st.name);
    g_phase = Phase::Failed;
    return {};
  }
  st.renderingSlot = index;
  return s.texture;
}

void release_slot(Stream& st, const std::array<XrView, 2>* views, std::array<int32_t, 2> rect = {},
                  uint64_t tickFrame = 0) {
  if (st.renderingSlot < 0)
    return;
  Slot& s = st.slots[st.renderingSlot];
  st.renderingSlot = -1;
  wgpu::SharedTextureMemoryVkImageLayoutEndState es{};
  wgpu::SharedTextureMemoryEndAccessState state{};
  state.nextInChain = &es;
  if (s.stm.EndAccess(s.texture, &state) != wgpu::Status::Success) {
    Log.error("EndAccess failed ({}); XR presentation disabled", st.name);
    g_phase = Phase::Failed;
    return;
  }
  PendingAccess release;
  release.oldLayout = static_cast<VkImageLayout>(es.oldLayout);
  release.newLayout = static_cast<VkImageLayout>(es.newLayout);
  for (size_t i = 0; i < state.fenceCount; ++i) {
    DawnFenceExportInfo oi{};
    wgpu::SharedFenceExportInfo ei{};
    ei.nextInChain = &oi;
    state.fences[i].ExportInfo(&ei);
    if (ei.type != kDawnFenceType) {
      Log.error("Unexpected Dawn fence type {}", static_cast<int>(ei.type));
      continue;
    }
    if (oi.handle >= 0)
      release.fds.push_back(dup(oi.handle)); // the SharedFence keeps its own handle
  }
  std::lock_guard lock{g_mutex};
  s.fromDawn = std::move(release);
  if (views != nullptr)
    s.views = *views;
  s.rect = rect;
  s.tickFrame = tickFrame;
  s.readySeen = 0;
  s.state = SlotState::Ready;
}

// ---------------------------------------------------------------- render worker: 3D fights
//
// Every frame with fight geometry, the world draws are replayed once per eye
// with GX bind group 3 set to
//     C = P_eye · V_eye · A · V_game⁻¹
// which takes a vertex from the game camera's space (what the shader has
// after the position matrix) to the eye's clip space: undo the game camera,
// place the arena in the room (A), then look at it from the eye. Lighting,
// skinning, texgen and projected shadows happen before this, so they stay as
// the game computed them. Matrices here are column-vector math stored
// row-major, which is what `vec4(p, 1) * m` in WGSL expects.

using Mat4 = std::array<float, 16>;

Mat4 mul(const Mat4& a, const Mat4& b) {
  Mat4 r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k)
        r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
  return r;
}

// Inverse of an affine 3x4 (GX Mtx layout: rows, translation in column 3).
Mat4 inverse_affine(const std::array<float, 12>& m) {
  const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
  const float A = e * i - f * h, Bc = -(d * i - f * g), Cc = d * h - e * g;
  const float det = a * A + b * Bc + c * Cc;
  const float s = std::abs(det) > 1e-12f ? 1.f / det : 0.f;
  const std::array<float, 9> inv{A * s,  -(b * i - c * h) * s, (b * f - c * e) * s,
                                 Bc * s, (a * i - c * g) * s,  -(a * f - c * d) * s,
                                 Cc * s, -(a * h - b * g) * s, (a * e - b * d) * s};
  const float tx = m[3], ty = m[7], tz = m[11];
  return {inv[0], inv[1], inv[2], -(inv[0] * tx + inv[1] * ty + inv[2] * tz),
          inv[3], inv[4], inv[5], -(inv[3] * tx + inv[4] * ty + inv[5] * tz),
          inv[6], inv[7], inv[8], -(inv[6] * tx + inv[7] * ty + inv[8] * tz),
          0.f,    0.f,    0.f,    1.f};
}

// World (head-space) -> eye: the inverse of the eye's pose.
Mat4 view_from_pose(const XrPosef& p) {
  const float x = p.orientation.x, y = p.orientation.y, z = p.orientation.z, w = p.orientation.w;
  // Rotation matrix of the quaternion (rows), then transpose for the inverse.
  const std::array<float, 9> r{1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
                               2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                               2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)};
  const float tx = p.position.x, ty = p.position.y, tz = p.position.z;
  return {r[0], r[3], r[6], -(r[0] * tx + r[3] * ty + r[6] * tz),
          r[1], r[4], r[7], -(r[1] * tx + r[4] * ty + r[7] * tz),
          r[2], r[5], r[8], -(r[2] * tx + r[5] * ty + r[8] * tz),
          0.f,  0.f,  0.f,  1.f};
}

// Asymmetric perspective for an OpenXR fov, reversed Z with an infinite far
// plane (depth 1 at `near`, 0 at infinity), matching aurora's GX depth.
Mat4 projection(const XrFovf& fov, float near) {
  const float l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight);
  const float u = std::tan(fov.angleUp), d = std::tan(fov.angleDown);
  return {2.f / (r - l), 0.f, (r + l) / (r - l), 0.f,
          0.f, 2.f / (u - d), (u + d) / (u - d), 0.f,
          0.f, 0.f, 0.f, near,
          0.f, 0.f, -1.f, 0.f};
}

// Game units -> meters in the starting head space: T(pos) · R_y(yaw) ·
// S(scale). The scale starts at AURORA_XR_ARENA_SCALE (default 0.0035: Final
// Destination's ~170-unit stage is about 0.6 m wide); the player can move,
// turn and scale the arena during a pause.
Mat4 arena_transform() {
  const ArenaPose a = arena_pose();
  std::array<float, 3> ctr;
  {
    std::lock_guard lock{g_arenaMutex};
    ctr = g_arenaCenter;
  }
  const float c = std::cos(a.yaw) * a.scale, s = std::sin(a.yaw) * a.scale;
  // T(pos) · R_y(yaw) · S(scale) · T(-center)
  const float tx = a.pos.x - (c * ctr[0] + s * ctr[2]);
  const float ty = a.pos.y - a.scale * ctr[1];
  const float tz = a.pos.z - (-s * ctr[0] + c * ctr[2]);
  return {c, 0.f, s, tx, 0.f, a.scale, 0.f, ty, -s, 0.f, c, tz, 0.f, 0.f, 0.f, 1.f};
}

constexpr char kComposeShader[] = R"(
struct Params { offset: vec2f, mode: u32, alpha: f32 };
@group(0) @binding(0) var color_tex: texture_2d<f32>;
@group(0) @binding(1) var depth_tex: DEPTH_TYPE;
@group(0) @binding(2) var<uniform> params: Params;

@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
  let uv = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
  return vec4f(uv * 2.0 - 1.0, 0.0, 1.0);
}

// Premultiplied output for the compositor. 3D (mode 0): alpha is depth
// coverage (reversed Z, cleared to 0), so geometry hides the room and
// effects that write no depth add light over it. HUD (mode 1): opaque where
// the HUD drew something bright, see-through where it left black, over an
// optional constant backdrop (AURORA_XR_HUD_BACKDROP).
@fragment fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4f {
  let texel = vec2i(pos.xy - params.offset);
  let c = textureLoad(color_tex, texel, 0);
  if (params.mode == 1u) {
    let ink = clamp(max(c.r, max(c.g, c.b)) * 3.0, 0.0, 1.0);
    return vec4f(c.rgb, max(params.alpha, ink));
  }
  let d = textureLoad(depth_tex, texel, 0);
  return vec4f(c.rgb, select(0.0, 1.0, d > 0.0));
}
)";

struct ReplayTarget {
  gfx::XrReplayTarget target;
  std::vector<wgpu::Texture> textures; // keeps the views' textures alive
  wgpu::TextureView sampleColor;       // single-sample scene color
  wgpu::TextureView depth;
};

// Render worker only.
// Render-side GPU timing (AURORA_XR_TIMING, Dawn timestamp queries): each
// zone is a pass's begin/end pair, averaged and logged every 10 s.
// kZone3D times both eyes on the direct path (one pass), or the left eye on
// the fallback path, where kZone3DRight is the right eye.
// kZoneFrame has only a begin, written by the frame's first flat pass; the
// whole frame runs from it to the last end written (the GPU time dynamic
// resolution steers by, compositor preemptions included).
enum TimingZone : uint32_t { kZone3D, kZone3DRight, kZoneCompose3D, kZoneHud, kZoneComposeHud, kZoneFrame, kZoneCount };
constexpr std::array<const char*, kZoneCount> kZoneNames{"3D eyes", "3D right eye (fallback)", "3D compose", "HUD",
                                                         "HUD compose", "frame"};

struct GpuTiming {
  wgpu::QuerySet queries;
  wgpu::Buffer resolve;
  struct Readback {
    wgpu::Buffer buffer;
    uint32_t zones = 0; // zones written in the frame it holds
    bool busy = false;  // copied into or mapped; not yet read
  };
  std::array<Readback, 3> readbacks;
  int pending = -1;   // readback filled this frame, mapped after submit
  uint32_t zones = 0; // zones written this frame
  // Filled from map callbacks (render worker, via ProcessEvents).
  std::array<double, kZoneCount> ns{};
  std::array<uint64_t, kZoneCount> samples{};
  uint64_t frames = 0;
  double last3DNs = 0;    // the newest frame's kZone3D
  double lastFrameNs = 0; // the newest frame, first flat pass to last XR pass (dynamic resolution)
  uint64_t frameSeq = 0;  // frames measured, so the controller sees each once
  std::atomic<bool> frameBegun{false}; // this frame's first pass took the kZoneFrame begin
  uint64_t worldDraws = 0, worldFrames = 0; // draws replayed for the eyes (both views)
  std::chrono::steady_clock::time_point lastLog = std::chrono::steady_clock::now();
  std::array<wgpu::PassTimestampWrites, kZoneCount> writes{};
};

// The multiview shader's XrEye: one matrix per eye, the enabled flags (y:
// 1 - the clipped draws' opacity, as float bits), then the clip planes in
// game camera space (aurora_xr_world_clip) and their fade bands, four to a
// vec4.
static_assert(gfx::XrMaxClipPlanes == AURORA_XR_MAX_CLIPS && gfx::XrMaxClipPlanes % 4 == 0);
constexpr uint64_t kMultiviewEyeSize = 2 * 64 + 16 + gfx::XrMaxClipPlanes * 16 + gfx::XrMaxClipPlanes * 4;

struct Renderer3D {
  uint64_t layoutKey = 0;
  uint32_t sampleCount = 0;
  std::array<ReplayTarget, 3> targets; // eye 0, eye 1, HUD
  // Per eye, per world transform index (0 = none).
  std::array<std::array<wgpu::Buffer, gfx::XrMaxTransforms>, 2> eyeUniforms;
  std::array<std::array<wgpu::BindGroup, gfx::XrMaxTransforms>, 2> eyeGroups;
  // Multiview: both eyes' matrices in one uniform per world transform.
  std::array<wgpu::Buffer, gfx::XrMaxTransforms> mvUniforms;
  std::array<wgpu::BindGroup, gfx::XrMaxTransforms> mvGroups;
  wgpu::RenderPipeline compose;
  wgpu::BindGroupLayout composeLayout;
  std::array<wgpu::Buffer, 3> params;
  std::array<wgpu::BindGroup, 3> composeGroups;
  std::array<XrView, 2> renderedViews{};
  bool renderedStereo = false;
  // A fight frame that found no free 3D image: the headset keeps the last
  // one, and the flat frame is still nobody's to see.
  bool missedStereo = false;
  uint32_t directSamples = 0; // g_xrSamples the direct attachments were made for
  std::array<int32_t, 2> renderedRect{}; // dynamic resolution: this frame's eye size (0: all)
  uint64_t renderedTick = 0;              // the frame's pacing tick (FramePacket::xrTickFrame)
  bool failed = false;
  GpuTiming timing;
  // Direct path: both eyes straight into the shared 3D image, one pass.
  wgpu::Texture directDepth;
  std::vector<wgpu::Texture> directExtras; // non-scene attachments (normal buffer)
  std::array<wgpu::TextureView, gfx::MaxColorAttachments> directExtraViews{};
  wgpu::RenderPipeline coverClear, coverSet;
  uint64_t directKey = 0;
  bool loggedPath = false;
};
Renderer3D R;

const wgpu::PassTimestampWrites* zone_writes(TimingZone zone) {
  if (!R.timing.queries)
    return nullptr;
  R.timing.zones |= 1u << zone;
  return &R.timing.writes[zone];
}

void setup_timing() {
  auto& device = webgpu::g_device;
  if (!env_flag("AURORA_XR_TIMING", true) || !device.HasFeature(wgpu::FeatureName::TimestampQuery))
    return;
  auto& t = R.timing;
  const wgpu::QuerySetDescriptor qsd{.label = "XR timing", .type = wgpu::QueryType::Timestamp, .count = kZoneCount * 2};
  t.queries = device.CreateQuerySet(&qsd);
  const uint64_t bytes = kZoneCount * 2 * sizeof(uint64_t);
  const wgpu::BufferDescriptor rd{.label = "XR timing resolve",
                                  .usage = wgpu::BufferUsage::QueryResolve | wgpu::BufferUsage::CopySrc,
                                  .size = bytes};
  t.resolve = device.CreateBuffer(&rd);
  for (auto& rb : t.readbacks) {
    const wgpu::BufferDescriptor bd{.label = "XR timing readback",
                                    .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
                                    .size = bytes};
    rb.buffer = device.CreateBuffer(&bd);
  }
  for (uint32_t z = 0; z < kZoneCount; ++z)
    t.writes[z] = {.querySet = t.queries, .beginningOfPassWriteIndex = z * 2, .endOfPassWriteIndex = z * 2 + 1};
  t.writes[kZoneFrame].endOfPassWriteIndex = wgpu::kQuerySetIndexUndefined;
  gfx::set_xr_first_pass_timing([]() -> const wgpu::PassTimestampWrites* {
    if (!g_sessionRunning || R.timing.frameBegun.exchange(true))
      return nullptr;
    return zone_writes(kZoneFrame);
  });
}

// After the frame's passes are encoded: resolve this frame's timestamps into
// a free readback buffer (mapped in end_frame, after the submit).
void resolve_timing(const wgpu::CommandEncoder& cmd) {
  auto& t = R.timing;
  if (!t.queries || t.zones == 0)
    return;
  for (int i = 0; i < static_cast<int>(t.readbacks.size()); ++i) {
    auto& rb = t.readbacks[i];
    if (rb.busy)
      continue;
    cmd.ResolveQuerySet(t.queries, 0, kZoneCount * 2, t.resolve, 0);
    cmd.CopyBufferToBuffer(t.resolve, 0, rb.buffer, 0, kZoneCount * 2 * sizeof(uint64_t));
    rb.busy = true;
    rb.zones = t.zones;
    t.pending = i;
    break;
  }
  t.zones = 0;
  t.frameBegun = false;
}

void map_timing() {
  auto& t = R.timing;
  if (t.pending < 0)
    return;
  const int index = t.pending;
  t.pending = -1;
  t.readbacks[index].buffer.MapAsync(
      wgpu::MapMode::Read, 0, kZoneCount * 2 * sizeof(uint64_t), wgpu::CallbackMode::AllowProcessEvents,
      [index](wgpu::MapAsyncStatus status, wgpu::StringView) {
        auto& t = R.timing;
        auto& rb = t.readbacks[index];
        if (status == wgpu::MapAsyncStatus::Success) {
          const auto* ts = static_cast<const uint64_t*>(rb.buffer.GetConstMappedRange());
          for (uint32_t z = 0; z < kZoneCount; ++z) {
            if (z != kZoneFrame && (rb.zones & (1u << z)) && ts[z * 2 + 1] > ts[z * 2]) {
              t.ns[z] += static_cast<double>(ts[z * 2 + 1] - ts[z * 2]);
              ++t.samples[z];
              if (z == kZone3D)
                t.last3DNs = static_cast<double>(ts[z * 2 + 1] - ts[z * 2]);
            }
          }
          if (rb.zones & (1u << kZoneFrame)) {
            uint64_t end = 0;
            for (uint32_t z = 0; z < kZoneCount; ++z)
              if (z != kZoneFrame && (rb.zones & (1u << z)))
                end = std::max(end, ts[z * 2 + 1]);
            if (end > ts[kZoneFrame * 2]) {
              t.lastFrameNs = static_cast<double>(end - ts[kZoneFrame * 2]);
              ++t.frameSeq;
            }
          }
          ++t.frames;
          rb.buffer.Unmap();
        }
        rb.busy = false;
        const auto now = std::chrono::steady_clock::now();
        if (now - t.lastLog >= log_period() && t.frames > 0) {
          std::string line;
          double total = 0;
          for (uint32_t z = 0; z < kZoneCount; ++z) {
            const double us = t.samples[z] ? t.ns[z] / t.samples[z] / 1000.0 : 0.0;
            total += us;
            line += fmt::format("{}{} {:.0f}", z ? ", " : "", kZoneNames[z], us);
          }
          Log.info("GPU 3D passes per frame (us): {} (total {:.0f}); {:.0f} world draws per frame", line, total,
                   t.worldFrames ? static_cast<double>(t.worldDraws) / t.worldFrames : 0.0);
          t.worldDraws = 0;
          t.worldFrames = 0;
          t.ns = {};
          t.samples = {};
          t.frames = 0;
          t.lastLog = now;
        }
      });
}

// The 3D frame hook drew this frame (render worker), so the flat present can
// be skipped: nothing shows the virtual screen during a fight.
bool g_skipPresent = false;
uint32_t g_xrSamples = 1; // AURORA_XR_MSAA: the 3D eyes' sample count

ReplayTarget make_replay_target(const gfx::RenderTargetLayout& layout, uint32_t width, uint32_t height,
                                const char* label) {
  auto& device = webgpu::g_device;
  ReplayTarget rt;
  auto& t = rt.target;
  t.layout = layout;
  t.size = {width, height, 1};
  t.clearColor = {0, 0, 0, 0};
  t.clearDepth = gx::UseReversedZ ? 0.f : 1.f;
  for (uint32_t i = 0; i < layout.colorAttachmentCount; ++i) {
    const bool scene = i == gfx::SceneColorAttachmentIndex;
    wgpu::TextureDescriptor td{
        .label = label,
        .usage = wgpu::TextureUsage::RenderAttachment |
                 (scene && layout.sampleCount == 1 ? wgpu::TextureUsage::TextureBinding : wgpu::TextureUsage::None),
        .size = t.size,
        .format = layout.colorAttachments[i].format,
        .sampleCount = layout.sampleCount,
    };
    auto tex = device.CreateTexture(&td);
    t.colorViews[i] = tex.CreateView();
    rt.textures.push_back(tex);
    if (scene) {
      if (layout.sampleCount > 1) {
        td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
        td.sampleCount = 1;
        auto resolve = device.CreateTexture(&td);
        t.resolveViews[i] = resolve.CreateView();
        rt.sampleColor = t.resolveViews[i];
        rt.textures.push_back(resolve);
      } else {
        rt.sampleColor = t.colorViews[i];
      }
    }
  }
  const wgpu::TextureDescriptor dd{
      .label = label,
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
      .size = t.size,
      .format = layout.depthStencilFormat,
      .sampleCount = layout.sampleCount,
  };
  auto depth = device.CreateTexture(&dd);
  t.depthView = depth.CreateView();
  rt.depth = t.depthView;
  rt.textures.push_back(depth);
  return rt;
}

wgpu::Buffer make_buffer(uint64_t size, wgpu::BufferUsage usage, const char* label) {
  const wgpu::BufferDescriptor bd{.label = label, .usage = usage | wgpu::BufferUsage::CopyDst, .size = size};
  return webgpu::g_device.CreateBuffer(&bd);
}

bool ensure_renderer(const gfx::RenderTargetLayout& layout) {
  if (R.failed)
    return false;
  if (R.compose && R.layoutKey == layout.key && R.sampleCount == layout.sampleCount)
    return true;
  auto& device = webgpu::g_device;
  const uint32_t eyeW = g_streams[kStereo].width / 2, eyeH = g_streams[kStereo].height;
  R.targets[0] = make_replay_target(layout, eyeW, eyeH, "XR eye 0");
  R.targets[1] = make_replay_target(layout, eyeW, eyeH, "XR eye 1");
  R.targets[2] = make_replay_target(layout, g_streams[kHud].width, g_streams[kHud].height, "XR HUD");
  R.targets[2].target.fullViewport = false; // the HUD keeps the game's viewports

  for (int i = 0; i < 2; ++i) {
    for (uint32_t t = 0; t < gfx::XrMaxTransforms; ++t) {
      R.eyeUniforms[i][t] = make_buffer(gx::XrEyeUniformSize, wgpu::BufferUsage::Uniform, "XR eye matrix");
      const wgpu::BindGroupEntry e{.binding = 0, .buffer = R.eyeUniforms[i][t], .size = gx::XrEyeUniformSize};
      const wgpu::BindGroupDescriptor bg{.layout = gx::g_xrEyeBindGroupLayout, .entryCount = 1, .entries = &e};
      R.eyeGroups[i][t] = device.CreateBindGroup(&bg);
    }
    R.targets[i].target.views[0].xrBindGroups = R.eyeGroups[i];
  }
  if (g_multiview) {
    for (uint32_t t = 0; t < gfx::XrMaxTransforms; ++t) {
      R.mvUniforms[t] = make_buffer(kMultiviewEyeSize, wgpu::BufferUsage::Uniform, "XR eye matrices (multiview)");
      const wgpu::BindGroupEntry e{.binding = 0, .buffer = R.mvUniforms[t], .size = kMultiviewEyeSize};
      const wgpu::BindGroupDescriptor bg{.layout = gx::g_xrEyeBindGroupLayout, .entryCount = 1, .entries = &e};
      R.mvGroups[t] = device.CreateBindGroup(&bg);
    }
  }

  // Composite pipeline into the shared images (RGBA8Unorm).
  std::string source = kComposeShader;
  const std::string depthType =
      layout.sampleCount > 1 ? "texture_depth_multisampled_2d" : "texture_depth_2d";
  source.replace(source.find("DEPTH_TYPE"), std::string_view{"DEPTH_TYPE"}.size(), depthType);
  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = source.c_str();
  const wgpu::ShaderModuleDescriptor smd{.nextInChain = &wgsl, .label = "XR compose"};
  const auto module = device.CreateShaderModule(&smd);
  const std::array entries{
      wgpu::BindGroupLayoutEntry{.binding = 0,
                                 .visibility = wgpu::ShaderStage::Fragment,
                                 .texture = {.sampleType = wgpu::TextureSampleType::UnfilterableFloat,
                                             .viewDimension = wgpu::TextureViewDimension::e2D}},
      wgpu::BindGroupLayoutEntry{.binding = 1,
                                 .visibility = wgpu::ShaderStage::Fragment,
                                 .texture = {.sampleType = wgpu::TextureSampleType::Depth,
                                             .viewDimension = wgpu::TextureViewDimension::e2D,
                                             .multisampled = layout.sampleCount > 1}},
      wgpu::BindGroupLayoutEntry{.binding = 2,
                                 .visibility = wgpu::ShaderStage::Fragment,
                                 .buffer = {.type = wgpu::BufferBindingType::Uniform, .minBindingSize = 16}},
  };
  const wgpu::BindGroupLayoutDescriptor bgld{.label = "XR compose", .entryCount = entries.size(),
                                             .entries = entries.data()};
  R.composeLayout = device.CreateBindGroupLayout(&bgld);
  const wgpu::PipelineLayoutDescriptor pld{.bindGroupLayoutCount = 1, .bindGroupLayouts = &R.composeLayout};
  const auto pipelineLayout = device.CreatePipelineLayout(&pld);
  const wgpu::ColorTargetState colorTarget{.format = wgpu::TextureFormat::RGBA8Unorm};
  const wgpu::FragmentState fragment{.module = module, .entryPoint = "fs", .targetCount = 1, .targets = &colorTarget};
  const wgpu::RenderPipelineDescriptor rpd{
      .label = "XR compose",
      .layout = pipelineLayout,
      .vertex = {.module = module, .entryPoint = "vs"},
      .fragment = &fragment,
  };
  R.compose = device.CreateRenderPipeline(&rpd);

  const float hudAlpha = std::clamp(env_float("AURORA_XR_HUD_BACKDROP", 0.f), 0.f, 1.f);
  for (int i = 0; i < 3; ++i) {
    struct {
      float offset[2];
      uint32_t mode;
      float alpha;
    } p{{i == 1 ? static_cast<float>(eyeW) : 0.f, 0.f}, i == 2 ? 1u : 0u, hudAlpha};
    R.params[i] = make_buffer(sizeof(p), wgpu::BufferUsage::Uniform, "XR compose params");
    webgpu::g_queue.WriteBuffer(R.params[i], 0, &p, sizeof(p));
    const std::array bge{
        wgpu::BindGroupEntry{.binding = 0, .textureView = R.targets[i].sampleColor},
        wgpu::BindGroupEntry{.binding = 1, .textureView = R.targets[i].depth},
        wgpu::BindGroupEntry{.binding = 2, .buffer = R.params[i], .size = sizeof(p)},
    };
    const wgpu::BindGroupDescriptor bgd{.layout = R.composeLayout, .entryCount = bge.size(), .entries = bge.data()};
    R.composeGroups[i] = device.CreateBindGroup(&bgd);
  }
  if (!R.timing.queries)
    setup_timing();
  R.layoutKey = layout.key;
  R.sampleCount = layout.sampleCount;
  Log.info("Flat-layout targets ready (HUD, per-eye fallback): eyes {}x{}, HUD {}x{}, {}x MSAA", eyeW, eyeH,
           g_streams[kHud].width,
           g_streams[kHud].height, layout.sampleCount);
  return true;
}

// Coverage into alpha at the end of the direct 3D pass, in tile memory: two
// full-screen triangles at the far plane (reversed Z: depth 0) that write
// only alpha. Where the depth is still cleared ("equal") alpha becomes 0;
// where geometry wrote depth ("less" than it) alpha becomes 1. Depth-less
// draws (translucent effects) keep their color over alpha 0, which the
// premultiplied projection layer adds over passthrough as light.
constexpr char kCoverageShader[] = R"(
@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
  let uv = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
  return vec4f(uv * 2.0 - 1.0, 0.0, 1.0);
}
@fragment fn fs_clear() -> @location(0) vec4f { return vec4f(0.0, 0.0, 0.0, 0.0); }
@fragment fn fs_set() -> @location(0) vec4f { return vec4f(0.0, 0.0, 0.0, 1.0); }
)";

bool ensure_direct(const gfx::RenderTargetLayout& layout) {
  if (R.coverClear && R.directKey == layout.key && R.directSamples == g_xrSamples)
    return true;
  auto& device = webgpu::g_device;
  const auto& stereo = g_streams[kStereo];
  const wgpu::Extent3D size{stereo.width, stereo.height, stereo.layers};
  const wgpu::TextureViewDescriptor layered{.dimension = wgpu::TextureViewDimension::e2DArray,
                                            .arrayLayerCount = stereo.layers};
  const wgpu::TextureViewDescriptor* viewDesc = stereo.layers > 1 ? &layered : nullptr;
  // MSAA: every attachment multisampled and, where the device allows,
  // transient: on a tiler they then live only in tile memory, and only the
  // resolve into the shared image reaches memory.
  const bool msaa = g_xrSamples > 1;
  const auto usage = wgpu::TextureUsage::RenderAttachment |
                     (msaa && device.HasFeature(wgpu::FeatureName::TransientAttachments)
                          ? wgpu::TextureUsage::TransientAttachment
                          : wgpu::TextureUsage::None);
  const wgpu::TextureDescriptor dd{.label = "XR 3D depth",
                                   .usage = usage,
                                   .size = size,
                                   .format = layout.depthStencilFormat,
                                   .sampleCount = g_xrSamples};
  R.directDepth = device.CreateTexture(&dd);
  R.directExtras.clear();
  R.directExtraViews = {};
  for (uint32_t i = 0; i < layout.colorAttachmentCount; ++i) {
    if (i == gfx::SceneColorAttachmentIndex && !msaa)
      continue; // the shared image itself
    const wgpu::TextureDescriptor td{.label = i == gfx::SceneColorAttachmentIndex ? "XR 3D color (MSAA)"
                                                                                   : "XR 3D extra attachment",
                                     .usage = usage,
                                     .size = size,
                                     .format = layout.colorAttachments[i].format,
                                     .sampleCount = g_xrSamples};
    auto tex = device.CreateTexture(&td);
    R.directExtraViews[i] = tex.CreateView(viewDesc);
    R.directExtras.push_back(tex);
  }
  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = kCoverageShader;
  const wgpu::ShaderModuleDescriptor smd{.nextInChain = &wgsl, .label = "XR coverage"};
  const auto module = device.CreateShaderModule(&smd);
  const wgpu::PipelineLayoutDescriptor pld{.bindGroupLayoutCount = 0};
  const auto pipelineLayout = device.CreatePipelineLayout(&pld);
  std::array<wgpu::ColorTargetState, gfx::MaxColorAttachments> targets{};
  for (uint32_t i = 0; i < layout.colorAttachmentCount; ++i) {
    targets[i] = {.format = layout.colorAttachments[i].format,
                  .writeMask = i == gfx::SceneColorAttachmentIndex ? wgpu::ColorWriteMask::Alpha
                                                                   : wgpu::ColorWriteMask::None};
  }
  const auto make = [&](const char* entry, wgpu::CompareFunction compare) {
    const wgpu::FragmentState fragment{.module = module,
                                       .entryPoint = entry,
                                       .targetCount = layout.colorAttachmentCount,
                                       .targets = targets.data()};
    const wgpu::DepthStencilState depth{.format = layout.depthStencilFormat,
                                        .depthWriteEnabled = wgpu::OptionalBool::False,
                                        .depthCompare = compare};
    wgpu::RenderPipelineMultiview multiview{};
    multiview.viewMask = (1u << stereo.layers) - 1u;
    const wgpu::RenderPipelineDescriptor rpd{
        .nextInChain = stereo.layers > 1 ? &multiview : nullptr,
        .label = "XR coverage",
        .layout = pipelineLayout,
        .vertex = {.module = module, .entryPoint = "vs"},
        .depthStencil = &depth,
        .multisample = {.count = g_xrSamples},
        .fragment = &fragment,
    };
    return device.CreateRenderPipeline(&rpd);
  };
  // Reversed Z: cleared depth is 0 (far); the triangles sit at 0.
  R.coverClear = make("fs_clear", gx::UseReversedZ ? wgpu::CompareFunction::Equal : wgpu::CompareFunction::Equal);
  R.coverSet = make("fs_set", gx::UseReversedZ ? wgpu::CompareFunction::Less : wgpu::CompareFunction::Greater);
  R.directKey = layout.key;
  R.directSamples = g_xrSamples;
  return true;
}

void draw_coverage(const wgpu::RenderPassEncoder& pass, void*) {
  pass.SetPipeline(R.coverClear);
  pass.Draw(3);
  pass.SetPipeline(R.coverSet);
  pass.Draw(3);
}

void compose(const wgpu::CommandEncoder& cmd, const wgpu::Texture& dst, std::initializer_list<int> sources,
             TimingZone zone) {
  const auto view = dst.CreateView();
  const wgpu::RenderPassColorAttachment ca{
      .view = view,
      .loadOp = wgpu::LoadOp::Clear,
      .storeOp = wgpu::StoreOp::Store,
      .clearValue = {0, 0, 0, 0},
  };
  const wgpu::RenderPassDescriptor rpd{.label = "XR compose",
                                       .colorAttachmentCount = 1,
                                       .colorAttachments = &ca,
                                       .timestampWrites = zone_writes(zone)};
  auto pass = cmd.BeginRenderPass(&rpd);
  pass.SetPipeline(R.compose);
  for (int i : sources) {
    const auto& size = R.targets[i].target.size;
    const float x = i == 1 ? static_cast<float>(size.width) : 0.f;
    pass.SetViewport(x, 0.f, static_cast<float>(size.width), static_cast<float>(size.height), 0.f, 1.f);
    pass.SetBindGroup(0, R.composeGroups[i]);
    pass.Draw(3);
  }
  pass.End();
}

// gfx frame hook: runs on the render worker once every pass of a frame is
// encoded. Re-draws the fight per eye and the HUD into the shared images.
void render_3d_frame(const wgpu::CommandEncoder& cmd, gfx::detail::FramePacket& frame);

// Dynamic resolution's controller, once per 3D frame (render worker), after
// Martin Fuller's DRS practice (a live cost table): steer by the whole
// frame's GPU time (timestamps from the first flat pass to the last XR pass,
// compositor preemptions included, so the jumbotron's grab or anything else
// in the frame counts), record it per scale step for the stage and mode, and
// pick from what each step actually cost. Steps not measured lately are
// predicted from a fit of cost against pixels over the ones that were,
// blended with their own history by how much there is of it and how fresh.
// The fit is only trusted across a spread of scales; until then a quarter
// of the frame is taken to scale with pixels.
// Much of a frame's cost doesn't scale with pixels at all (Brinstar: about
// 9.6 of 11 ms), so a fixed model either gives resolution away for nothing
// or overshoots. Down past target + band at once, to the best step that
// fits; up one step at a time after a dwell, only to a step that fits. A
// run of missed swapchain images (two within a second) is a panic: the step
// is marked dearer than measured and the scale drops a step. Single misses
// come at the same rate at any scale (Brinstar: about one in 13 s), so they
// are only counted. Images the headset held long (g_stereoLate) are only
// logged.
namespace {
constexpr double kDynresFullSamples = 30;  // samples for full trust in a step
constexpr double kDynresForget = 1200;     // frames for a step's trust to fall to 1/e (20 s)
constexpr int kDynresUpDwell = 30;         // frames between steps up

double dynres_trust(const DynresCost& c, uint64_t frame) {
  if (c.samples <= 0)
    return 0;
  return std::min(c.samples / kDynresFullSamples, 1.0) *
         std::exp(-static_cast<double>(frame - c.lastFrame) / kDynresForget);
}

float dynres_scale(int level) { return g_dynres.minScale + kDynresStep * static_cast<float>(level); }

int dynres_levels() {
  const auto& dr = g_dynres;
  return std::clamp(static_cast<int>((dr.maxScale - dr.minScale) / kDynresStep + 0.5f) + 1, 1, kDynresLevels);
}

// Each step's predicted frame GPU time: its own history where trusted,
// otherwise a weighted fit ns = a + b * scale^2 over the trusted steps (b at
// least 0); with a single measured point, half of it is taken to scale with
// pixels until more are in.
void dynres_predict(const std::array<DynresCost, kDynresLevels>& costs, uint64_t frame, int levels,
                    std::array<double, kDynresLevels>& out) {
  double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (int i = 0; i < levels; ++i) {
    const double w = dynres_trust(costs[i], frame);
    if (w <= 0)
      continue;
    const double x = static_cast<double>(dynres_scale(i)) * dynres_scale(i);
    sw += w;
    sx += w * x;
    sy += w * costs[i].ns;
    sxx += w * x * x;
    sxy += w * x * costs[i].ns;
  }
  double a = 0, b = 0;
  if (sw > 0) {
    const double mx = sx / sw, my = sy / sw;
    const double var = sxx / sw - mx * mx;
    // A fit across a narrow spread of scales mostly measures how the scene
    // changed between visits: until the steps span enough, assume a quarter
    // of the frame scales with pixels (Brinstar measured about a tenth).
    double lo = 1e9, hi = 0;
    for (int i = 0; i < levels; ++i)
      if (dynres_trust(costs[i], frame) > 0) {
        const double x = static_cast<double>(dynres_scale(i)) * dynres_scale(i);
        lo = std::min(lo, x);
        hi = std::max(hi, x);
      }
    // At most half of the frame scales with pixels: a fit that says more is
    // reading a scene change as a resolution cost.
    b = hi - lo >= 0.1 && var > 1e-6 ? std::clamp((sxy / sw - mx * my) / var, 0.0, 0.5 * my / mx) : 0.25 * my / mx;
    a = my - b * mx;
  }
  for (int i = 0; i < levels; ++i) {
    const double x = static_cast<double>(dynres_scale(i)) * dynres_scale(i);
    const double w = dynres_trust(costs[i], frame);
    const double fit = sw > 0 ? a + b * x : 0;
    out[i] = sw > 0 ? w * costs[i].ns + (1 - w) * fit : 0;
  }
}
} // namespace

void update_dynamic_resolution(bool missed) {
  auto& dr = g_dynres;
  const auto now = std::chrono::steady_clock::now();
  ++dr.frameCount;
  if (dr.random) {
    // Exercise every size: resolution-dependent bugs show up as flicker.
    dr.scale = dr.minScale + (dr.maxScale - dr.minScale) * static_cast<float>(std::rand() % 1000) / 999.f;
  } else {
    int stage;
    {
      std::lock_guard lock{g_arenaMutex};
      stage = g_stage;
    }
    auto& costs = g_dynresCosts[stage * 2 + (g_passthroughWanted ? 1 : 0)];
    const int levels = dynres_levels();
    dr.level = std::clamp(dr.level, 0, levels - 1);
    // Shaders compiling (a stage's first fight of the session) stall the
    // frame on the CPU and inflate its span; less resolution can't help, and
    // those frames would make every step look dear for a long while. They're
    // left out of the costs and don't cut the scale (a panic still does).
    if (const uint32_t created = gfx::pipelines_created(); created != dr.pipelinesSeen) {
      dr.pipelinesSeen = created;
      dr.compileQuiet = 0;
    } else {
      ++dr.compileQuiet;
    }
    const bool compiling = dr.compileQuiet < 30;
    const auto& t = R.timing;
    if (t.frameSeq != dr.seenSeq && t.lastFrameNs > 0) {
      dr.seenSeq = t.frameSeq;
      // Readbacks lag a few frames: give a new scale time to show. A spike
      // half again over this visit's average (a hitch elsewhere) is left out.
      const double ns = t.lastFrameNs;
      const bool spike = dr.emaNs > 0 && ns > dr.emaNs * 1.5;
      if (++dr.settle > 4 && !compiling && !spike) {
        dr.emaNs = dr.emaNs == 0 ? ns : dr.emaNs * 0.85 + ns * 0.15;
        dr.nsSum += ns;
        ++dr.nsFrames;
        auto& c = costs[dr.level];
        // What is left of the old history, then this sample on top.
        c.samples *= std::exp(-static_cast<double>(dr.frameCount - c.lastFrame) / kDynresForget);
        c.ns = c.samples <= 0 ? ns : c.ns + (ns - c.ns) / std::min(c.samples + 1, kDynresFullSamples);
        c.samples = std::min(c.samples + 1, kDynresFullSamples);
        c.lastFrame = dr.frameCount;
      }
    }
    // A run of missed 3D images (two within a second) is a panic; a lone
    // miss comes at the same rate at any scale. (The render worker's own
    // cadence jitters, so a long gap between 3D frames is no signal.)
    bool panic = false;
    if (missed) {
      dr.recentMisses[dr.recentIndex++ % dr.recentMisses.size()] = dr.frameCount;
      int recent = 0;
      for (uint64_t f : dr.recentMisses)
        recent += f != 0 && dr.frameCount - f < 60 ? 1 : 0;
      panic = recent >= 2;
      if (!panic)
        ++dr.loneMisses;
    }
    int next = dr.level;
    if (panic) {
      // This step is dearer than it measured: remembered past the band.
      auto& c = costs[dr.level];
      c.ns = std::max(c.ns, dr.targetNs + dr.bandNs) + 0.5e6;
      c.samples = std::max(c.samples, kDynresFullSamples / 2);
      c.lastFrame = dr.frameCount;
      std::fill(dr.recentMisses.begin(), dr.recentMisses.end(), 0);
      next = dr.level - 1;
      ++dr.panics;
    } else if (dr.emaNs > 0 && dr.settle > 4) {
      std::array<double, kDynresLevels> predicted{};
      dynres_predict(costs, dr.frameCount, levels, predicted);
      if (dr.emaNs > dr.targetNs + dr.bandNs && !compiling) {
        // Down at once to the best step predicted to fit, at least one.
        next = 0;
        for (int i = dr.level - 1; i > 0; --i)
          if (predicted[i] <= dr.targetNs) {
            next = i;
            break;
          }
      } else if (++dr.sinceStep >= kDynresUpDwell && dr.level + 1 < levels &&
                 predicted[dr.level + 1] <= dr.targetNs) {
        next = dr.level + 1;
      }
    }
    next = std::clamp(next, 0, levels - 1);
    if (next != dr.level) {
      dr.level = next;
      dr.emaNs = 0;
      dr.settle = 0;
      dr.sinceStep = 0;
    }
    dr.scale = dynres_scale(dr.level);
  }
  dr.lastFrame = now;
  const uint32_t late = g_stereoLate.load();
  dr.lateImages += late - dr.lateSeen;
  dr.lateSeen = late;
  dr.scaleSum += dr.scale;
  ++dr.frames;
  dr.lowest = std::min(dr.lowest, dr.scale);
  dr.highest = std::max(dr.highest, dr.scale);
  if (now - dr.lastLog >= log_period()) {
    if (dr.lastLog.time_since_epoch().count() != 0 && dr.frames > 0) {
      // The stage's cost table: each step measured lately, as scale:ms.
      std::string table;
      if (!dr.random) {
        int stage;
        {
          std::lock_guard lock{g_arenaMutex};
          stage = g_stage;
        }
        const auto& costs = g_dynresCosts[stage * 2 + (g_passthroughWanted ? 1 : 0)];
        for (int i = 0; i < dynres_levels(); ++i)
          if (dynres_trust(costs[i], dr.frameCount) > 0.1)
            table += fmt::format(" {:.3f}:{:.1f}", dynres_scale(i), costs[i].ns / 1.0e6);
      }
      Log.info("Dynamic resolution: scale {:.2f} average ({:.2f}-{:.2f}); frame GPU {:.1f} ms average; {} panics, "
               "{} lone misses; {} images held long; costs{}",
               dr.scaleSum / dr.frames, dr.lowest, dr.highest, dr.nsFrames ? dr.nsSum / dr.nsFrames / 1.0e6 : 0.0,
               dr.panics, dr.loneMisses, dr.lateImages, table);
    }
    dr.scaleSum = dr.nsSum = 0;
    dr.frames = dr.nsFrames = dr.panics = dr.loneMisses = dr.lateImages = 0;
    dr.lowest = 99.f;
    dr.highest = 0.f;
    dr.lastLog = now;
  }
}

// Frame hook: the 3D views, then whether the next frame's flat world draws
// are needed (not while fights go to the headset with no flat present).
// A missed 3D image counts too: drawing the flat world then made the next
// frame heavier and the next miss likelier, so slow stages kept sliding.
void render_3d(const wgpu::CommandEncoder& cmd, gfx::detail::FramePacket& frame) {
  R.renderedStereo = false;
  R.missedStereo = false;
  R.renderedRect = {};
  R.renderedTick = frame.xrTickFrame;
  render_3d_frame(cmd, frame);
  gfx::set_xr_drop_flat_world((R.renderedStereo || R.missedStereo) && g_skipPresent &&
                              !env_flag("AURORA_XR_FLAT_WORLD", false));
}

void render_3d_frame(const wgpu::CommandEncoder& cmd, gfx::detail::FramePacket& frame) {
  if (g_phase != Phase::Imported || !g_sessionRunning || !frame.xrHasWorld ||
      !env_flag("AURORA_XR_3D", true))
    return;
  std::array<XrView, 2> views;
  {
    std::lock_guard lock{g_viewMutex};
    if (!g_viewsValid)
      return;
    views = g_latestViews;
  }
  const auto layout = gfx::scene_render_target_layout();
  if (!ensure_renderer(layout))
    return;

  // C = P_eye · V_eye · A · T · V_game⁻¹, with T a draw's extra placement
  // (aurora_xr_world_transform; identity for index 0).
  const Mat4 cameraToWorld = inverse_affine(frame.xrWorldView);
  const Mat4 arena = arena_transform();
  const size_t transforms = std::min<size_t>(frame.xrTransforms.size() + 1, gfx::XrMaxTransforms);
  struct {
    std::array<Mat4, 2> m;
    uint32_t enabled[4];
    float clip[gfx::XrMaxClipPlanes][4];
    float fade[gfx::XrMaxClipPlanes]; // bands (game units)
  } mv[gfx::XrMaxTransforms]{};
  static_assert(sizeof(mv[0]) == kMultiviewEyeSize);
  for (int eye = 0; eye < 2; ++eye) {
    const Mat4 eyeToClip = mul(projection(views[eye].fov, 0.05f), view_from_pose(views[eye].pose));
    for (size_t t = 0; t < transforms; ++t) {
      Mat4 world = cameraToWorld;
      // Clip plane, game world -> game camera space (the shader clips the
      // camera-space position): plane_cam = plane_world · V_game⁻¹.
      std::array<std::array<float, 4>, gfx::XrMaxClipPlanes> clipCam{};
      for (auto& c : clipCam)
        c = {0.f, 0.f, 0.f, 1.f};
      if (t > 0) {
        const auto& m = frame.xrTransforms[t - 1];
        world = mul(Mat4{m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], 0.f, 0.f, 0.f, 1.f},
                    cameraToWorld);
        for (int k = 0; k < gfx::XrMaxClipPlanes; ++k)
          for (int j = 0; j < 4; ++j) {
            clipCam[k][j] = 0.f;
            for (int i = 0; i < 4; ++i)
              clipCam[k][j] += m[12 + k * 4 + i] * cameraToWorld[i * 4 + j];
          }
      }
      struct {
        Mat4 m;
        uint32_t enabled[4];
      } u{mul(mul(eyeToClip, arena), world), {1, 0, 0, 0}};
      static_assert(sizeof(u) == gx::XrEyeUniformSize);
      if (g_multiview) {
        mv[t].m[eye] = u.m;
        mv[t].enabled[0] = 1;
        // The clipped geometry's dissolve, as 1 - opacity (0: solid).
        // The entry's layout: move 3x4, planes, fades, opacity.
        constexpr int kFades = 12 + gfx::XrMaxClipPlanes * 4, kOpacity = kFades + gfx::XrMaxClipPlanes;
        mv[t].enabled[1] = std::bit_cast<uint32_t>(t > 0 ? 1.f - frame.xrTransforms[t - 1][kOpacity] : 0.f);
        for (int k = 0; k < gfx::XrMaxClipPlanes; ++k) {
          std::copy(clipCam[k].begin(), clipCam[k].end(), mv[t].clip[k]);
          mv[t].fade[k] = t > 0 ? frame.xrTransforms[t - 1][kFades + k] : 0.f;
        }
      } else {
        webgpu::g_queue.WriteBuffer(R.eyeUniforms[eye][t], 0, &u, sizeof(u));
      }
    }
  }
  if (g_multiview) {
    for (size_t t = 0; t < transforms; ++t)
      webgpu::g_queue.WriteBuffer(R.mvUniforms[t], 0, &mv[t], sizeof(mv[t]));
  }

  // Direct path: the framebuffer's format matches the shared 3D image and
  // there is no MSAA to resolve, so both eyes render straight into the shared
  // image in one pass (no private eye images, no compose pass). Otherwise
  // each eye renders privately and a compose pass writes the shared image.
  auto& stereo = g_streams[kStereo];
  const auto stereoFormat = stereo.format == VK_FORMAT_B8G8R8A8_UNORM ? wgpu::TextureFormat::BGRA8Unorm
                                                                      : wgpu::TextureFormat::RGBA8Unorm;
  const bool direct = layout.sampleCount == 1 &&
                      layout.colorAttachments[gfx::SceneColorAttachmentIndex].format == stereoFormat &&
                      env_flag("AURORA_XR_DIRECT", true);
  if (!R.loggedPath) {
    Log.info("3D eyes render {}", direct ? "straight into the shared image" : "privately, then composed");
    R.loggedPath = true;
  }
  if (!direct && (stereoFormat != wgpu::TextureFormat::RGBA8Unorm || g_multiview)) {
    return; // fallback compose writes side-by-side RGBA8 only (MSAA switched on mid-session)
  }
  if (direct) {
    if (!ensure_direct(layout))
      return;
    if (auto dst = acquire_slot(stereo)) {
      gfx::XrReplayTarget t;
      t.layout = layout;
      t.size = {stereo.width, stereo.height, 1};
      const wgpu::TextureViewDescriptor layered{.dimension = wgpu::TextureViewDimension::e2DArray,
                                                .arrayLayerCount = stereo.layers};
      const auto* viewDesc = g_multiview ? &layered : nullptr;
      const auto dstView = dst.CreateView(viewDesc);
      t.colorViews = R.directExtraViews;
      if (g_xrSamples > 1) {
        // Draw into the MSAA image, resolve into the shared one, and keep
        // none of the samples.
        t.resolveViews[gfx::SceneColorAttachmentIndex] = dstView;
        t.colorStore = wgpu::StoreOp::Discard;
      } else {
        t.colorViews[gfx::SceneColorAttachmentIndex] = dstView;
      }
      t.depthView = R.directDepth.CreateView(viewDesc);
      t.clearColor = {0, 0, 0, 0};
      t.clearDepth = gx::UseReversedZ ? 0.f : 1.f;
      t.depthStore = wgpu::StoreOp::Discard;
      if (g_multiview) {
        // One replay draws both eyes: view mask 0b11 over the image's two layers.
        float vw = static_cast<float>(stereo.width), vh = static_cast<float>(stereo.height);
        if (g_dynres.on) {
          update_dynamic_resolution(false);
          const auto even = [](float v) { return (static_cast<uint32_t>(v + 0.5f) + 1u) & ~1u; };
          const uint32_t rw = std::min(stereo.width, even(static_cast<float>(g_dynres.recWidth) * g_dynres.scale));
          const uint32_t rh = std::min(stereo.height, even(static_cast<float>(g_dynres.recHeight) * g_dynres.scale));
          vw = static_cast<float>(rw);
          vh = static_cast<float>(rh);
          t.renderAreaWidth = rw;
          t.renderAreaHeight = rh;
          R.renderedRect = {static_cast<int32_t>(rw), static_cast<int32_t>(rh)};
        }
        t.views[0] = {R.mvGroups, 0.f, 0.f, vw, vh};
        t.viewCount = 1;
        t.viewMask = 0b11;
      } else {
        const float eyeW = static_cast<float>(stereo.width / 2), eyeH = static_cast<float>(stereo.height);
        t.views[0] = {R.eyeGroups[0], 0.f, 0.f, eyeW, eyeH};
        t.views[1] = {R.eyeGroups[1], eyeW, 0.f, eyeW, eyeH};
        // AURORA_XR_ONE_EYE: replay the left eye only (measuring per-eye cost).
        t.viewCount = env_flag("AURORA_XR_ONE_EYE", false) ? 1 : 2;
      }
      uint32_t draws = 0;
      t.drawCount = &draws;
      t.finish = &draw_coverage;
      t.timestampWrites = zone_writes(kZone3D);
      gfx::encode_xr_replay(cmd, frame, gfx::XrCategory::World, t);
      R.timing.worldDraws += draws;
      ++R.timing.worldFrames;
      R.renderedViews = views;
      R.renderedStereo = true;
      g_skipPresent = !env_flag("AURORA_XR_FIGHT_SCREEN", false);
    } else {
      // No 3D image this frame: still a fight, so don't present the flat
      // frame nobody sees (the headset keeps showing the last 3D image).
      R.missedStereo = true;
      if (g_dynres.on)
        update_dynamic_resolution(true);
      g_skipPresent = !env_flag("AURORA_XR_FIGHT_SCREEN", false);
    }
  } else {
    for (int eye = 0; eye < 2; ++eye) {
      R.targets[eye].target.timestampWrites = zone_writes(eye == 0 ? kZone3D : kZone3DRight);
      gfx::encode_xr_replay(cmd, frame, gfx::XrCategory::World, R.targets[eye].target);
    }
    if (auto dst = acquire_slot(stereo)) {
      compose(cmd, dst, {0, 1}, kZoneCompose3D);
      R.renderedViews = views;
      R.renderedStereo = true;
      g_skipPresent = !env_flag("AURORA_XR_FIGHT_SCREEN", false);
    } else {
      R.missedStereo = true;
      g_skipPresent = !env_flag("AURORA_XR_FIGHT_SCREEN", false);
    }
  }
  if (frame.xrHasHud) {
    R.targets[2].target.timestampWrites = zone_writes(kZoneHud);
    gfx::encode_xr_replay(cmd, frame, gfx::XrCategory::Hud, R.targets[2].target);
    if (auto dst = acquire_slot(g_streams[kHud])) {
      compose(cmd, dst, {2}, kZoneComposeHud);
    }
  }
  resolve_timing(cmd);
}

} // namespace

// ---------------------------------------------------------------- public API

bool wanted() noexcept {
#ifdef AURORA_XR_DEFAULT_ON
  static const bool want = env_flag("AURORA_XR", true);
#else
  static const bool want = env_flag("AURORA_XR", false);
#endif
  return want;
}

// Requested whether or not this process presents to XR: Dawn keys its blob
// cache on the device's enabled features, and the Quest launcher panel and
// the immersive game process share one cache. With different feature sets,
// every game pipeline missed what the panel built (about 85 ms each on the
// Quest instead of about 10) and the panel's prune deleted the game's entries.
void add_required_features(const wgpu::Adapter& adapter, std::vector<wgpu::FeatureName>& features) noexcept {
#ifdef __ANDROID__
  const std::array needed{wgpu::FeatureName::SharedTextureMemoryAHardwareBuffer, wgpu::FeatureName::SharedFenceSyncFD};
#else
  const std::array needed{wgpu::FeatureName::SharedTextureMemoryOpaqueFD,
                          wgpu::FeatureName::SharedFenceVkSemaphoreOpaqueFD};
#endif
  for (auto f : needed) {
    if (adapter.HasFeature(f) && std::find(features.begin(), features.end(), f) == features.end())
      features.push_back(f);
  }
  // Both eyes in one pass (Dawn fork multiview).
  if (adapter.HasFeature(wgpu::FeatureName::ChromiumExperimentalMultiview) &&
      std::find(features.begin(), features.end(), wgpu::FeatureName::ChromiumExperimentalMultiview) == features.end())
    features.push_back(wgpu::FeatureName::ChromiumExperimentalMultiview);
  // Dynamic resolution's partial render areas (AURORA_XR_DYNRES).
  if (adapter.HasFeature(wgpu::FeatureName::RenderPassRenderArea) &&
      std::find(features.begin(), features.end(), wgpu::FeatureName::RenderPassRenderArea) == features.end())
    features.push_back(wgpu::FeatureName::RenderPassRenderArea);
  // MSAA eye attachments that never leave tile memory (AURORA_XR_MSAA).
  if (adapter.HasFeature(wgpu::FeatureName::TransientAttachments) &&
      std::find(features.begin(), features.end(), wgpu::FeatureName::TransientAttachments) == features.end())
    features.push_back(wgpu::FeatureName::TransientAttachments);
  // XR GPU timing (AURORA_XR_TIMING).
  if (env_flag("AURORA_XR_TIMING", true) && adapter.HasFeature(wgpu::FeatureName::TimestampQuery) &&
      std::find(features.begin(), features.end(), wgpu::FeatureName::TimestampQuery) == features.end())
    features.push_back(wgpu::FeatureName::TimestampQuery);
}

bool active() noexcept { return g_phase == Phase::Imported && g_sessionRunning; }

bool skip_present() noexcept { return g_skipPresent; }

void screen_size(uint32_t contentWidth, uint32_t contentHeight, uint32_t& width, uint32_t& height) noexcept {
  static uint32_t fixedW = 0, fixedH = 0;
  if (fixedW == 0) {
    const auto vp = webgpu::calculate_present_viewport(8192, 8192, contentWidth, contentHeight);
    const float aspect = vp.height > 0.f ? vp.width / vp.height : 4.f / 3.f;
    fixedH = static_cast<uint32_t>(std::clamp(env_float("AURORA_XR_SCREEN_HEIGHT", 1080.f), 240.f, 4096.f));
    fixedW = (static_cast<uint32_t>(static_cast<float>(fixedH) * aspect + 0.5f) + 1u) & ~1u;
  }
  width = fixedW;
  height = fixedH;
  g_contentW = contentWidth;
  g_contentH = contentHeight;
}

wgpu::Texture begin_frame(uint32_t width, uint32_t height) noexcept {
  if (!wanted() || width == 0 || height == 0)
    return {};
  Phase phase = g_phase;
  if (phase == Phase::Idle) {
    if (!B.prepared) {
      Log.error("OpenXR was not set up before the GPU device (prepare_device); presenting to the window");
      g_phase = Phase::Failed;
      return {};
    }
    const auto h = dawn::native::vulkan::GetDeviceVkHandles(webgpu::g_device.Get());
    B.vkInstance = h.instance;
    B.phys = h.physicalDevice;
    B.dev = h.device;
    B.queueFamily = h.queueFamilyIndex;
    B.queueIndex = 1;
    B.queue = dawn::native::vulkan::GetExtraQueue(webgpu::g_device.Get(), 0);
    g_streams[kScreen].name = "screen";
    g_streams[kStereo].name = "3d";
    g_streams[kHud].name = "hud";
    g_streams[kScreen].width = width;
    g_streams[kScreen].height = height;
    // The HUD is big flat text on a small plane: half the screen's
    // resolution by default (AURORA_XR_HUD_SCALE).
    const float hudScale = std::clamp(env_float("AURORA_XR_HUD_SCALE", 0.5f), 0.1f, 2.f);
    g_streams[kHud].width = (static_cast<uint32_t>(static_cast<float>(width) * hudScale + 0.5f) + 1u) & ~1u;
    g_streams[kHud].height = (static_cast<uint32_t>(static_cast<float>(height) * hudScale + 0.5f) + 1u) & ~1u;
    const auto layout = gfx::scene_render_target_layout();
    g_sceneFormat = layout.colorAttachments[gfx::SceneColorAttachmentIndex].format;
    g_sceneSamples = layout.sampleCount;
    // Multiview needs the direct path (single-sample framebuffer).
    g_multiview = webgpu::g_device.HasFeature(wgpu::FeatureName::ChromiumExperimentalMultiview) &&
                  g_sceneSamples == 1 && env_flag("AURORA_XR_MULTIVIEW", true);
    {
      const char* v = std::getenv("AURORA_XR_FIXED_LATENCY");
      g_present.forced = v != nullptr && *v != '\0' ? std::max(0, std::atoi(v)) : -1;
      if (g_present.forced > 0)
        g_present.latency = g_present.forced;
    }
    // AURORA_XR_MSAA=<1|2|4>: samples for the 3D eyes alone (the flat frame
    // keeps its own), resolved into the shared image at the end of the pass.
    // 4 by default on Android, where it's measured.
    {
#ifdef __ANDROID__
      constexpr float kDefaultSamples = 4.f;
#else
      constexpr float kDefaultSamples = 1.f;
#endif
      const int n = static_cast<int>(env_float("AURORA_XR_MSAA", kDefaultSamples));
      g_xrSamples = g_multiview && (n == 2 || n == 4) ? static_cast<uint32_t>(n) : 1u;
    }
    Log.info("3D eyes: {}, {}x MSAA", g_multiview ? "multiview, both in one pass" : "side by side, one pass each",
             g_xrSamples);
    g_phase = Phase::Starting;
    g_thread = std::thread(thread_main);
    return {};
  }
  if (phase == Phase::SlotsReady) {
    if (!wrap_swapchains_into_dawn()) {
      Log.error("Virtual screen disabled; presenting to the window instead");
      g_phase = Phase::Failed;
      return {};
    }
    gfx::set_xr_frame_hook(&render_3d);
    g_phase = Phase::Imported;
    phase = Phase::Imported;
  }
  if (phase != Phase::Imported)
    return {};
  if (g_multiview) {
    // World draws recorded from now on resolve their multiview twins.
    auto mvLayout = gfx::scene_render_target_layout();
    mvLayout.viewCount = 2;
    mvLayout.sampleCount = g_xrSamples;
    gfx::detail::finalize_render_target_layout(mvLayout);
    gfx::set_xr_multiview_layout(mvLayout);
  }
  auto& screen = g_streams[kScreen];
  if (width != screen.width || height != screen.height) {
    static bool warned = false;
    if (!warned) {
      Log.warn("Frame size changed to {}x{}; the virtual screen stays {}x{} and the window takes over", width, height,
               screen.width, screen.height);
      warned = true;
    }
    return {};
  }
  auto texture = acquire_slot(screen);
  if (!texture) {
    // No image ready (the XR thread is a frame behind): drop this frame's
    // flat present rather than falling back to the window.
    g_skipPresent = true;
  }
  return texture;
}

void end_frame() noexcept {
  g_skipPresent = false;
  map_timing();
  release_slot(g_streams[kScreen], nullptr);
  release_slot(g_streams[kStereo], R.renderedStereo ? &R.renderedViews : nullptr, R.renderedRect, R.renderedTick);
  R.renderedStereo = false;
  release_slot(g_streams[kHud], nullptr, {}, R.renderedTick);
}

bool prepare_device() noexcept {
  if (!wanted() || B.prepared)
    return B.prepared;
  if (!create_instance()) {
    Log.error("OpenXR unavailable; presenting to the window instead");
    g_phase = Phase::Failed;
    return false;
  }
  auto getReqs = xr_proc<PFN_xrGetVulkanGraphicsRequirements2KHR>("xrGetVulkanGraphicsRequirements2KHR");
  B.createVulkanInstance = xr_proc<PFN_xrCreateVulkanInstanceKHR>("xrCreateVulkanInstanceKHR");
  B.createVulkanDevice = xr_proc<PFN_xrCreateVulkanDeviceKHR>("xrCreateVulkanDeviceKHR");
  B.getVulkanGraphicsDevice = xr_proc<PFN_xrGetVulkanGraphicsDevice2KHR>("xrGetVulkanGraphicsDevice2KHR");
  if (!getReqs || !B.createVulkanInstance || !B.createVulkanDevice || !B.getVulkanGraphicsDevice) {
    Log.error("XR_KHR_vulkan_enable2 entry points missing; presenting to the window instead");
    g_phase = Phase::Failed;
    return false;
  }
  // Required before the runtime creates anything Vulkan for us.
  XrGraphicsRequirementsVulkan2KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
  if (XR_FAILED(getReqs(B.instance, B.systemId, &reqs))) {
    Log.error("xrGetVulkanGraphicsRequirements2KHR failed; presenting to the window instead");
    g_phase = Phase::Failed;
    return false;
  }
  dawn::native::vulkan::ExternalVulkanHooks hooks;
  hooks.createInstance = &hook_create_instance;
  hooks.getPhysicalDevice = &hook_physical_device;
  hooks.createDevice = &hook_create_device;
  hooks.extraQueueCount = 1; // the runtime's queue (graphics binding)
  dawn::native::vulkan::SetExternalVulkanHooks(&hooks);
  B.prepared = true;
  return true;
}

void release_instance() noexcept {
  if (B.instance) {
    xrDestroyInstance(B.instance);
    B.instance = XR_NULL_HANDLE;
  }
  dawn::native::vulkan::SetExternalVulkanHooks(nullptr);
}

void shutdown() noexcept {
  g_stop = true;
  if (g_thread.joinable() && g_thread.get_id() != std::this_thread::get_id())
    g_thread.join();
}

} // namespace aurora::xr

extern "C" bool aurora_xr_get_pad(PADStatus* out) {
  std::lock_guard lock{aurora::xr::g_padMutex};
  if (!aurora::xr::g_padValid)
    return false;
  *out = aurora::xr::g_pad;
  return true;
}

extern "C" bool aurora_xr_screen_pointer(float* x, float* y, bool* pressed) {
  std::lock_guard lock{aurora::xr::g_padMutex};
  const auto& p = aurora::xr::g_screenPointer;
  if (!p.valid)
    return false;
  *x = p.x;
  *y = p.y;
  *pressed = p.pressed;
  return true;
}

extern "C" void aurora_xr_set_paused(bool paused) { aurora::xr::g_fightPaused = paused; }

extern "C" void aurora_xr_set_placing(int mode) { aurora::xr::g_placing = mode; }

extern "C" void aurora_xr_reset_stage(void) {
  using namespace aurora::xr;
  std::lock_guard lock{g_arenaMutex};
  init_arena();
  g_stageArenas.erase(g_stage);
  g_arena = g_defaultArena;
  g_arena.scale *= g_stageScale;
}

extern "C" void aurora_xr_set_placing_clip(int which, int width, int height, const unsigned char* rgba, int frames,
                                           int cols, int frameW, int frameH, float fps, int x, int y, int w, int h) {
  using namespace aurora::xr;
  if (which < 0 || which >= static_cast<int>(g_placingClips.size()) || width <= 0 || height <= 0 || !rgba ||
      frames <= 0 || cols <= 0 || frameW <= 0 || frameH <= 0 || cols * frameW > width ||
      (frames + cols - 1) / cols * frameH > height)
    return;
  std::lock_guard lock{g_placingMutex};
  PlacingClip& clip = g_placingClips[which];
  if (clip.atlas.swapchain)
    return; // already shown; clips are given once
  clip.atlas.width = static_cast<uint32_t>(width);
  clip.atlas.height = static_cast<uint32_t>(height);
  clip.atlas.rgba.assign(rgba, rgba + static_cast<size_t>(width) * height * 4);
  clip.frames = frames;
  clip.cols = cols;
  clip.frameW = frameW;
  clip.frameH = frameH;
  clip.fps = fps > 0.f ? fps : 12.f;
  clip.x = x;
  clip.y = y;
  clip.w = w;
  clip.h = h;
}

extern "C" void aurora_xr_set_placing_image(int which, int width, int height, const unsigned char* rgba) {
  using namespace aurora::xr;
  if (which < 0 || which >= static_cast<int>(g_placingImages.size()) || width <= 0 || height <= 0 || !rgba)
    return;
  std::lock_guard lock{g_placingMutex};
  PlacingImage& img = g_placingImages[which];
  if (img.swapchain)
    return; // already shown; pictures are given once
  img.width = static_cast<uint32_t>(width);
  img.height = static_cast<uint32_t>(height);
  img.rgba.assign(rgba, rgba + static_cast<size_t>(width) * height * 4);
}

extern "C" void aurora_xr_set_passthrough(bool on) { aurora::xr::g_passthroughWanted = on; }

extern "C" bool aurora_xr_active(void) { return aurora::xr::active(); }

extern "C" void aurora_xr_set_stage_top(float y) {
  std::lock_guard lock{aurora::xr::g_arenaMutex};
  aurora::xr::g_stageTop = y;
}

extern "C" void aurora_xr_set_stage(int stage, float x, float y, float z, float scale) {
  using namespace aurora::xr;
  std::lock_guard lock{g_arenaMutex};
  g_stage = stage;
  g_arenaCenter = {x, y, z};
  g_stageScale = scale > 0.f ? scale : 1.f;
  init_arena();
  if (const auto it = g_stageArenas.find(stage); it != g_stageArenas.end()) {
    g_arena = it->second;
  } else {
    g_arena = g_defaultArena;
    g_arena.scale *= g_stageScale;
  }
}

extern "C" bool aurora_xr_pace(void) {
  using namespace aurora::xr;
  if (g_displayPerGameFrame <= 0 || !g_sessionRunning)
    return false;
  std::unique_lock lock{g_paceMutex};
  const uint64_t tick = g_paceTick;
  // Bounded: a paused or stopped session falls back to the game's own timer.
  const bool ticked = g_paceCv.wait_for(lock, std::chrono::milliseconds(50), [&] {
    return g_paceTick != tick || g_displayPerGameFrame <= 0 || !g_sessionRunning;
  }) && g_paceTick != tick;
  // The frame the game records next carries this tick (fixed-latency
  // presentation; 0 without one).
  aurora::gfx::set_xr_game_frame_tick(ticked ? g_lastTickFrame.load() : 0);
  return ticked;
}
