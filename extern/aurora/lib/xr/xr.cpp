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
#include <cmath>
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

// Where the arena sits in the room (meters, starting head space): A =
// T(pos) · R_y(yaw) · S(scale) takes game units to the room. The XR thread
// moves it while the player drags the arena during a pause; the render
// worker reads it for every 3D frame. Kept for the whole session.
struct ArenaPose {
  XrVector3f pos{0.f, -0.45f, -1.f};
  float yaw = 0.f;
  float scale = 0.006f;
};
std::mutex g_arenaMutex;
ArenaPose g_arena;
// The game point (game units) placed at the arena position: a stage whose
// geometry sits far from the origin is centered by aurora_xr_set_arena_center.
std::array<float, 3> g_arenaCenter{};
bool g_arenaInit = false;
float g_defaultArenaScale = 0.006f;

// Set by the game every fight frame (aurora_xr_set_paused): the fight is
// paused, so the controllers point and grab instead of playing.
std::atomic<bool> g_fightPaused{false};

// Controller input, written by the XR thread, read by the game thread.
std::mutex g_padMutex;
PADStatus g_pad{};
bool g_padValid = false;

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
  bool pointerMode = false; // last frame: paused fight, the grips grab instead of pressing Z
  XrVector3f head{};        // between the eyes, from the latest views
  bool focused = false;

  // Laser layers: static textures, drawn as quads at display rate.
  XrSwapchain beamSwapchain = XR_NULL_HANDLE, dotSwapchain = XR_NULL_HANDLE;
  bool hasColorScaleBias = false;

  uint64_t framesShown = 0, fightFrames = 0;
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
  }
  if (B.hasRefreshRateExt) {
    B.enumerateRefreshRates = xr_proc<PFN_xrEnumerateDisplayRefreshRatesFB>("xrEnumerateDisplayRefreshRatesFB");
    B.requestRefreshRate = xr_proc<PFN_xrRequestDisplayRefreshRateFB>("xrRequestDisplayRefreshRateFB");
    B.getRefreshRate = xr_proc<PFN_xrGetDisplayRefreshRateFB>("xrGetDisplayRefreshRateFB");
  }
  if (B.hasPerfSettingsExt)
    B.setPerformanceLevel = xr_proc<PFN_xrPerfSettingsSetPerformanceLevelEXT>("xrPerfSettingsSetPerformanceLevelEXT");
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
  const uint32_t eyeW = even(static_cast<float>(views[0].recommendedImageRectWidth) * scale);
  const uint32_t eyeH = even(static_cast<float>(views[0].recommendedImageRectHeight) * scale);
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
  // Minimal fallback for runtimes and simulators without Touch controllers
  // (Monado's keyboard/mouse controllers): select is A, menu is Start.
  suggest("/interaction_profiles/khr/simple_controller", {
                                                             {B.btnA, "/user/hand/right/input/select/click"},
                                                             {B.btnB, "/user/hand/left/input/select/click"},
                                                             {B.btnStart, "/user/hand/left/input/menu/click"},
                                                             {B.btnStart, "/user/hand/right/input/menu/click"},
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
  pad.stickX = to_axis(main.x);
  pad.stickY = to_axis(main.y);
  pad.substickX = to_axis(c.x);
  pad.substickY = to_axis(c.y);
  const float l = action_float(B.trigL);
  const float r = action_float(B.trigR);
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

// Releases drawn images in acquisition order: waits (on the GPU, on our
// queue) for Dawn's semaphores, puts the image back in the layout the
// runtime expects, then hands it to the runtime.
bool release_ready(Stream& st, bool& released) {
  released = false;
  for (;;) {
    int index = -1;
    PendingAccess fromDawn;
    std::array<XrView, 2> views{};
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
      fromDawn = std::move(st.slots[index].fromDawn);
      st.slots[index].fromDawn = {};
      views = st.slots[index].views;
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
    ++st.releases;
    released = true;
  }
}

// CPU and GPU clock levels (XR_EXT_performance_settings), off by default.
// On a Quest 3 a sustained-high request succeeded but left the GPU at the
// level the runtime already picked (2, 640 MHz) and changed nothing measurable
// (Temple 4P, 2026-10-01), so the runtime keeps its dynamic clocks.
// AURORA_XR_PERF_GPU / AURORA_XR_PERF_CPU = low, high, boost opt in.
void request_performance_levels() {
  if (!B.setPerformanceLevel) {
    Log.info("XR_EXT_performance_settings unavailable; the runtime picks clock levels");
    return;
  }
  const auto level = [](const char* name) {
    const char* v = std::getenv(name);
    const std::string_view s = v != nullptr ? v : "high";
    if (s == "low")
      return XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT;
    if (s == "boost")
      return XR_PERF_SETTINGS_LEVEL_BOOST_EXT;
    return XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
  };
  const char* gpuEnv = std::getenv("AURORA_XR_PERF_GPU");
  if (gpuEnv == nullptr || *gpuEnv == '\0' || std::string_view{gpuEnv} == "off")
    return; // the runtime's own clock levels
  const auto cpu = level("AURORA_XR_PERF_CPU"), gpu = level("AURORA_XR_PERF_GPU");
  const XrResult rc = B.setPerformanceLevel(B.session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, cpu);
  const XrResult rg = B.setPerformanceLevel(B.session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, gpu);
  Log.info("Performance levels requested: CPU {} ({}), GPU {} ({})", static_cast<int>(cpu), static_cast<int>(rc),
           static_cast<int>(gpu), static_cast<int>(rg));
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
  }
}

// ---------------------------------------------------------------- XR thread: arena placement
//
// While a fight is paused each controller shows a laser. Squeezing a grip
// while its laser is on the arena grabs it:
//   one hand   the arena hangs off the laser at the grabbed point and turns
//              its front (the game camera's side) to the player; letting go
//              leaves it there
//   two hands  squeezing the other grip too scales the arena and turns it
//              about the vertical axis, around the point between the hands;
//              letting go of either hand ends the grab
// Starting position and scale: AURORA_XR_ARENA_POS, AURORA_XR_ARENA_SCALE.

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
float wrap_angle(float a) {
  constexpr float kPi = 3.14159265f;
  while (a > kPi)
    a -= 2.f * kPi;
  while (a < -kPi)
    a += 2.f * kPi;
  return a;
}

ArenaPose arena_pose() {
  std::lock_guard lock{g_arenaMutex};
  if (!g_arenaInit) {
    g_arenaInit = true;
    if (const char* v = std::getenv("AURORA_XR_ARENA_POS"))
      std::sscanf(v, "%f,%f,%f", &g_arena.pos.x, &g_arena.pos.y, &g_arena.pos.z);
    g_arena.scale = g_defaultArenaScale = env_float("AURORA_XR_ARENA_SCALE", 0.006f);
    g_arena.yaw = env_float("AURORA_XR_ARENA_YAW", 0.f) * 3.14159265f / 180.f; // degrees
  }
  return g_arena;
}

void set_arena_pose(const ArenaPose& a) {
  std::lock_guard lock{g_arenaMutex};
  g_arena = a;
}

// What the lasers can grab, in game units around the stage's origin: wider
// and taller than any stage's main platform, so pointing near it is enough.
constexpr XrVector3f kGrabBoxMin{-120.f, -80.f, -60.f};
constexpr XrVector3f kGrabBoxMax{120.f, 100.f, 60.f};
// Meters per game unit: Final Destination from about 25 cm to 8.5 m wide.
constexpr float kMinArenaScale = 0.0015f, kMaxArenaScale = 0.05f;

// Distance along the ray (meters) to the arena's grab box, or -1 for a miss.
float hit_arena(const ArenaPose& a, XrVector3f origin, XrVector3f dir) {
  XrVector3f o = rot_y(origin - a.pos, -a.yaw) * (1.f / a.scale);
  {
    std::lock_guard lock{g_arenaMutex};
    o = o + XrVector3f{g_arenaCenter[0], g_arenaCenter[1], g_arenaCenter[2]};
  }
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

struct Hand {
  bool valid = false;
  XrVector3f origin{};
  XrVector3f dir{0.f, 0.f, -1.f};
  bool held = false, wasHeld = false; // grip past its threshold, with hysteresis
  float hit = -1.f;                   // meters along the laser to the arena, -1 = none
};

struct Grab {
  std::array<Hand, 2> hands;
  int oneHand = -1; // the hand dragging the arena alone
  bool twoHands = false;
  ArenaPose start;
  float dist = 0.f;  // one hand: the grabbed point's distance along the laser
  XrVector3f offset; // one hand: arena center minus grabbed point, unrotated by the arena's yaw
  XrVector3f mid0;   // two hands: midpoint, span and heading at the start
  float span0 = 1.f, heading0 = 0.f;
  std::chrono::steady_clock::time_point last;
};
Grab G;

void locate_hands(XrTime time) {
  for (int h = 0; h < 2; ++h) {
    Hand& hand = G.hands[h];
    hand.wasHeld = hand.held;
    hand.valid = false;
    if (!B.aimSpace[h] || !B.focused) {
      hand.held = false;
      continue;
    }
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    constexpr XrSpaceLocationFlags kValid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if (XR_SUCCEEDED(xrLocateSpace(B.aimSpace[h], B.space, time, &loc)) && (loc.locationFlags & kValid) == kValid) {
      hand.valid = true;
      hand.origin = loc.pose.position;
      hand.dir = vnorm(qrot(loc.pose.orientation, {0.f, 0.f, -1.f}));
    }
    const float grip = action_float(B.grab[h]);
    hand.held = hand.held ? grip > 0.35f : grip > 0.65f;
  }
}

void end_grab() {
  if (G.oneHand < 0 && !G.twoHands)
    return;
  G.oneHand = -1;
  G.twoHands = false;
  const ArenaPose a = arena_pose();
  Log.info("Arena placed at {:.2f},{:.2f},{:.2f}, yaw {:.0f} deg, scale {:.4f}", a.pos.x, a.pos.y, a.pos.z,
           a.yaw * 57.2958f, a.scale);
}

void begin_one_hand(int h, const ArenaPose& a) {
  const Hand& hand = G.hands[h];
  G.oneHand = h;
  G.twoHands = false;
  G.start = a;
  G.dist = hand.hit;
  G.offset = rot_y(a.pos - (hand.origin + hand.dir * hand.hit), -a.yaw);
}

void begin_two_hands(const ArenaPose& a) {
  const XrVector3f l = G.hands[0].origin, r = G.hands[1].origin;
  const XrVector3f span = r - l;
  G.oneHand = -1;
  G.twoHands = true;
  G.start = a;
  G.mid0 = (l + r) * 0.5f;
  G.span0 = std::max(vlen(span), 0.05f);
  G.heading0 = std::atan2(-span.z, span.x);
}

// Per display frame. `active`: a fight is paused and on the headset.
void update_grab(bool active) {
  const auto now = std::chrono::steady_clock::now();
  const float dt = std::clamp(std::chrono::duration<float>(now - G.last).count(), 0.f, 0.1f);
  G.last = now;
  ArenaPose a = arena_pose();
  for (Hand& hand : G.hands)
    hand.hit = hand.valid ? hit_arena(a, hand.origin, hand.dir) : -1.f;
  if (!active) {
    end_grab();
    return;
  }
  const auto pressed = [](const Hand& h) { return h.valid && h.held && !h.wasHeld; };
  const auto released = [](const Hand& h) { return !h.valid || !h.held; };

  if (G.twoHands) {
    if (released(G.hands[0]) || released(G.hands[1]))
      end_grab();
  } else if (G.oneHand >= 0) {
    if (released(G.hands[G.oneHand]))
      end_grab();
    else if (pressed(G.hands[1 - G.oneHand]))
      begin_two_hands(a);
  } else {
    for (int h = 0; h < 2; ++h) {
      if (pressed(G.hands[h]) && G.hands[h].hit >= 0.f) {
        begin_one_hand(h, a);
        break;
      }
    }
  }

  if (G.oneHand >= 0) {
    const Hand& hand = G.hands[G.oneHand];
    const XrVector3f point = hand.origin + hand.dir * G.dist;
    // Ease the front of the stage (+Z) round to face the player.
    const float facing = std::atan2(B.head.x - a.pos.x, B.head.z - a.pos.z);
    a.yaw += wrap_angle(facing - a.yaw) * (1.f - std::exp(-dt * 12.f));
    a.pos = point + rot_y(G.offset, a.yaw);
    set_arena_pose(a);
  } else if (G.twoHands) {
    const XrVector3f span = G.hands[1].origin - G.hands[0].origin;
    a.scale = std::clamp(G.start.scale * vlen(span) / G.span0, kMinArenaScale, kMaxArenaScale);
    const float ratio = a.scale / G.start.scale;
    const float turn = std::atan2(-span.z, span.x) - G.heading0;
    a.yaw = G.start.yaw + turn;
    a.pos = G.mid0 + rot_y((G.start.pos - G.mid0) * ratio, turn);
    set_arena_pose(a);
  }
}

// ---------------------------------------------------------------- XR thread: lasers

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
    VK_TRY(vkQueueSubmit(B.queue, 1, &si, fence));
    VK_TRY(vkWaitForFences(B.dev, 1, &fence, VK_TRUE, UINT64_MAX));
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XR_TRY(xrReleaseSwapchainImage(out, &ri));
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

constexpr uint32_t kBeamW = 16, kBeamH = 4, kDotSize = 64;

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
  return create_static_swapchain(B.beamSwapchain, kBeamW, kBeamH, beam) &&
         create_static_swapchain(B.dotSwapchain, kDotSize, kDotSize, dot);
}

constexpr size_t kMaxPointerLayers = 4; // a beam and a dot per hand

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

void build_pointer_layers(PointerLayers& out) {
  out.count = 0;
  if (!B.beamSwapchain || !B.dotSwapchain)
    return;
  for (int h = 0; h < 2; ++h) {
    const Hand& hand = G.hands[h];
    if (!hand.valid)
      continue;
    const bool grabbing = G.oneHand == h || G.twoHands;
    const float length = G.oneHand == h ? G.dist : hand.hit >= 0.f ? hand.hit : 1.f;
    const XrColor4f color = grabbing         ? XrColor4f{1.f, 0.8f, 0.3f, 1.f}
                            : hand.hit >= 0.f ? XrColor4f{0.45f, 0.85f, 1.f, 1.f}
                                              : XrColor4f{0.85f, 0.9f, 1.f, 0.5f};
    // The beam: a thin quad along the laser, turned about it to face the head.
    const XrVector3f end = hand.origin + hand.dir * length;
    const XrVector3f center = hand.origin + hand.dir * (length * 0.5f);
    const XrVector3f toHead = B.head - center;
    const XrVector3f z = vnorm(toHead - hand.dir * vdot(toHead, hand.dir));
    if (vlen(z) > 0.5f)
      add_pointer_quad(out, B.beamSwapchain, kBeamW, kBeamH, center,
                       quat_from_axes(vcross(hand.dir, z), hand.dir, z), {0.004f, length}, color);
    if (hand.hit < 0.f && G.oneHand != h)
      continue;
    // The dot where the laser meets the arena, facing the head.
    const XrVector3f dz = vnorm(B.head - end);
    const XrVector3f dx = vnorm(vcross({0.f, 1.f, 0.f}, dz));
    if (vlen(dx) > 0.5f)
      add_pointer_quad(out, B.dotSwapchain, kDotSize, kDotSize, end, quat_from_axes(dx, vcross(dz, dx), dz),
                       {0.02f, 0.02f}, color);
  }
}

bool render_xr_frame() {
  XrFrameState fs{XR_TYPE_FRAME_STATE};
  XR_TRY(xrWaitFrame(B.session, nullptr, &fs));
  if (const int per = g_displayPerGameFrame; per > 0 && ++B.displayFrame % static_cast<uint64_t>(per) == 0) {
    {
      std::lock_guard lock{g_paceMutex};
      ++g_paceTick;
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
  const bool pointing = fight && g_fightPaused && B.focused;
  locate_hands(fs.predictedDisplayTime);
  update_grab(pointing);
  B.pointerMode = pointing;

  std::array<const XrCompositionLayerBaseHeader*, 3 + kMaxPointerLayers> layers{};
  uint32_t layerCount = 0;
  XrCompositionLayerPassthroughFB ptLayer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
  if (B.passthroughLayer) {
    ptLayer.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    ptLayer.layerHandle = B.passthroughLayer;
    layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&ptLayer);
  }
  std::array<XrCompositionLayerProjectionView, 2> projViews{};
  XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerQuad hudQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  XrCompositionLayerQuad screenQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  PointerLayers pointers;
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
      projViews[i].subImage.imageRect = {{x, 0}, {eyeW, static_cast<int32_t>(stereo.height)}};
      projViews[i].subImage.imageArrayIndex = stereo.layers > 1 ? static_cast<uint32_t>(i) : 0;
    }
    proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    proj.space = B.space;
    proj.viewCount = 2;
    proj.views = projViews.data();
    layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
    if (hud.haveImage && now - hud.lastRelease < std::chrono::milliseconds(250)) {
      // The HUD floats above the arena's back edge, like a scoreboard, and
      // moves, turns and scales with it.
      const ArenaPose arena = arena_pose();
      const float k = arena.scale / g_defaultArenaScale;
      const float width = env_float("AURORA_XR_HUD_WIDTH", 0.9f) * k;
      hudQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
      hudQuad.space = B.space;
      hudQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
      hudQuad.subImage.swapchain = hud.swapchain;
      hudQuad.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(hud.width), static_cast<int32_t>(hud.height)}};
      hudQuad.pose.orientation = yaw_quat(arena.yaw);
      hudQuad.pose.position =
          arena.pos + rot_y({0.f, env_float("AURORA_XR_HUD_HEIGHT", 0.55f) * k, -0.15f * k}, arena.yaw);
      hudQuad.size = {width, width * static_cast<float>(hud.height) / static_cast<float>(hud.width)};
      layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hudQuad);
    }
    if (pointing) {
      build_pointer_layers(pointers);
      for (uint32_t i = 0; i < pointers.count; ++i)
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&pointers.quads[i]);
    }
    ++B.fightFrames;
  } else if (fs.shouldRender && g_streams[kScreen].haveImage) {
    // A swapchain with no new release shows its last released image, so the
    // screen keeps its picture on display frames the game did not produce.
    const auto& screen = g_streams[kScreen];
    const float width = env_float("AURORA_XR_SCREEN_WIDTH", 1.6f);
    screenQuad.space = B.space;
    screenQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    screenQuad.subImage.swapchain = screen.swapchain;
    screenQuad.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(screen.width), static_cast<int32_t>(screen.height)}};
    screenQuad.pose.orientation.w = 1.f;
    screenQuad.pose.position = {0.f, env_float("AURORA_XR_SCREEN_Y", 0.f), -env_float("AURORA_XR_SCREEN_DISTANCE", 1.5f)};
    screenQuad.size = {width, width * static_cast<float>(screen.height) / static_cast<float>(screen.width)};
    layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&screenQuad);
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
    Log.info("{:.1f} display fps ({:.0f}% 3D); frames/s released: screen {:.1f}, 3D {:.1f}, HUD {:.1f}",
             B.framesShown / secs, 100.0 * B.fightFrames / std::max<uint64_t>(B.framesShown, 1),
             B.releasedSinceStats[kScreen] / secs, B.releasedSinceStats[kStereo] / secs,
             B.releasedSinceStats[kHud] / secs);
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
  for (XrSwapchain* sc : {&B.beamSwapchain, &B.dotSwapchain}) {
    if (*sc)
      xrDestroySwapchain(*sc);
    *sc = XR_NULL_HANDLE;
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
  bd.initialized = false;
  if (s.stm.BeginAccess(s.texture, &bd) != wgpu::Status::Success) {
    Log.error("BeginAccess failed ({}); XR presentation disabled", st.name);
    g_phase = Phase::Failed;
    return {};
  }
  st.renderingSlot = index;
  return s.texture;
}

void release_slot(Stream& st, const std::array<XrView, 2>* views) {
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
// S(scale). The scale starts at AURORA_XR_ARENA_SCALE (default 0.006: Final
// Destination's ~170-unit stage is about 1 m wide); the player can move,
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
enum TimingZone : uint32_t { kZone3D, kZone3DRight, kZoneCompose3D, kZoneHud, kZoneComposeHud, kZoneCount };
constexpr std::array<const char*, kZoneCount> kZoneNames{"3D eyes", "3D right eye (fallback)", "3D compose", "HUD",
                                                         "HUD compose"};

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
  uint64_t worldDraws = 0, worldFrames = 0; // draws replayed for the eyes (both views)
  std::chrono::steady_clock::time_point lastLog = std::chrono::steady_clock::now();
  std::array<wgpu::PassTimestampWrites, kZoneCount> writes{};
};

// The multiview shader's XrEye: one matrix per eye, the enabled flags, then
// a clip plane in game camera space (aurora_xr_world_clip).
constexpr uint64_t kMultiviewEyeSize = 2 * 64 + 16 + 16 + 16 + 16; // ... clip planes, fade bands

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
            if ((rb.zones & (1u << z)) && ts[z * 2 + 1] > ts[z * 2]) {
              t.ns[z] += static_cast<double>(ts[z * 2 + 1] - ts[z * 2]);
              ++t.samples[z];
            }
          }
          ++t.frames;
          rb.buffer.Unmap();
        }
        rb.busy = false;
        const auto now = std::chrono::steady_clock::now();
        if (now - t.lastLog >= std::chrono::seconds(10) && t.frames > 0) {
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
  Log.info("3D fight targets ready: eyes {}x{}, HUD {}x{}, {}x MSAA", eyeW, eyeH, g_streams[kHud].width,
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
  if (R.coverClear && R.directKey == layout.key)
    return true;
  auto& device = webgpu::g_device;
  const auto& stereo = g_streams[kStereo];
  const wgpu::Extent3D size{stereo.width, stereo.height, stereo.layers};
  const wgpu::TextureViewDescriptor layered{.dimension = wgpu::TextureViewDimension::e2DArray,
                                            .arrayLayerCount = stereo.layers};
  const wgpu::TextureViewDescriptor* viewDesc = stereo.layers > 1 ? &layered : nullptr;
  const wgpu::TextureDescriptor dd{.label = "XR 3D depth",
                                   .usage = wgpu::TextureUsage::RenderAttachment,
                                   .size = size,
                                   .format = layout.depthStencilFormat};
  R.directDepth = device.CreateTexture(&dd);
  R.directExtras.clear();
  R.directExtraViews = {};
  for (uint32_t i = 0; i < layout.colorAttachmentCount; ++i) {
    if (i == gfx::SceneColorAttachmentIndex)
      continue;
    const wgpu::TextureDescriptor td{.label = "XR 3D extra attachment",
                                     .usage = wgpu::TextureUsage::RenderAttachment,
                                     .size = size,
                                     .format = layout.colorAttachments[i].format};
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
        .fragment = &fragment,
    };
    return device.CreateRenderPipeline(&rpd);
  };
  // Reversed Z: cleared depth is 0 (far); the triangles sit at 0.
  R.coverClear = make("fs_clear", gx::UseReversedZ ? wgpu::CompareFunction::Equal : wgpu::CompareFunction::Equal);
  R.coverSet = make("fs_set", gx::UseReversedZ ? wgpu::CompareFunction::Less : wgpu::CompareFunction::Greater);
  R.directKey = layout.key;
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

// Frame hook: the 3D views, then whether the next frame's flat world draws
// are needed (not while fights go to the headset with no flat present).
void render_3d(const wgpu::CommandEncoder& cmd, gfx::detail::FramePacket& frame) {
  R.renderedStereo = false;
  render_3d_frame(cmd, frame);
  gfx::set_xr_drop_flat_world(R.renderedStereo && g_skipPresent && !env_flag("AURORA_XR_FLAT_WORLD", false));
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
    float clip[4];
    float clip2[4];
    float fade[4]; // x, y: bands (game units)
  } mv[gfx::XrMaxTransforms]{};
  static_assert(sizeof(mv[0]) == kMultiviewEyeSize);
  for (int eye = 0; eye < 2; ++eye) {
    const Mat4 eyeToClip = mul(projection(views[eye].fov, 0.05f), view_from_pose(views[eye].pose));
    for (size_t t = 0; t < transforms; ++t) {
      Mat4 world = cameraToWorld;
      // Clip plane, game world -> game camera space (the shader clips the
      // camera-space position): plane_cam = plane_world · V_game⁻¹.
      std::array<float, 4> clipCam{0.f, 0.f, 0.f, 1.f}, clipCam2{0.f, 0.f, 0.f, 1.f};
      if (t > 0) {
        const auto& m = frame.xrTransforms[t - 1];
        world = mul(Mat4{m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], 0.f, 0.f, 0.f, 1.f},
                    cameraToWorld);
        for (int j = 0; j < 4; ++j) {
          clipCam[j] = clipCam2[j] = 0.f;
          for (int i = 0; i < 4; ++i) {
            clipCam[j] += m[12 + i] * cameraToWorld[i * 4 + j];
            clipCam2[j] += m[16 + i] * cameraToWorld[i * 4 + j];
          }
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
        std::copy(clipCam.begin(), clipCam.end(), mv[t].clip);
        std::copy(clipCam2.begin(), clipCam2.end(), mv[t].clip2);
        mv[t].fade[0] = t > 0 ? frame.xrTransforms[t - 1][20] : 0.f;
        mv[t].fade[1] = t > 0 ? frame.xrTransforms[t - 1][21] : 0.f;
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
      t.colorViews[gfx::SceneColorAttachmentIndex] = dstView;
      t.depthView = R.directDepth.CreateView(viewDesc);
      t.clearColor = {0, 0, 0, 0};
      t.clearDepth = gx::UseReversedZ ? 0.f : 1.f;
      t.depthStore = wgpu::StoreOp::Discard;
      if (g_multiview) {
        // One replay draws both eyes: view mask 0b11 over the image's two layers.
        t.views[0] = {R.mvGroups, 0.f, 0.f, static_cast<float>(stereo.width), static_cast<float>(stereo.height)};
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
    Log.info("3D eyes: {}", g_multiview ? "multiview, both in one pass" : "side by side, one pass each");
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
  release_slot(g_streams[kStereo], R.renderedStereo ? &R.renderedViews : nullptr);
  R.renderedStereo = false;
  release_slot(g_streams[kHud], nullptr);
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

extern "C" void aurora_xr_set_paused(bool paused) { aurora::xr::g_fightPaused = paused; }

extern "C" bool aurora_xr_active(void) { return aurora::xr::active(); }

extern "C" void aurora_xr_set_arena_center(float x, float y, float z) {
  std::lock_guard lock{aurora::xr::g_arenaMutex};
  aurora::xr::g_arenaCenter = {x, y, z};
}

extern "C" bool aurora_xr_pace(void) {
  using namespace aurora::xr;
  if (g_displayPerGameFrame <= 0 || !g_sessionRunning)
    return false;
  std::unique_lock lock{g_paceMutex};
  const uint64_t tick = g_paceTick;
  // Bounded: a paused or stopped session falls back to the game's own timer.
  return g_paceCv.wait_for(lock, std::chrono::milliseconds(50), [&] {
    return g_paceTick != tick || g_displayPerGameFrame <= 0 || !g_sessionRunning;
  }) && g_paceTick != tick;
}
