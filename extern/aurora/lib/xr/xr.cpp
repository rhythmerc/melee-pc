#include "xr.hpp"

#include <aurora/xr.h>

#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
#include "../gfx/xr_replay.hpp"
#include "../gx/gx.hpp"
#include "shaders/copy_spv.hpp"

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
#include <mutex>
#include <string>
#include <thread>

namespace aurora::xr {
namespace {
Module Log("aurora::xr");

// ---------------------------------------------------------------- platform
//
// How the two devices share memory and synchronize (Dawn's choices; see
// src/dawn/native/vulkan/SharedTextureMemoryVk.cpp and SharedFenceVk.cpp):
//   Linux    OPAQUE_FD memory, OPAQUE_FD semaphores, VK_QUEUE_FAMILY_EXTERNAL
//   Android  AHardwareBuffer memory, SYNC_FD semaphores, VK_QUEUE_FAMILY_FOREIGN_EXT
#ifdef __ANDROID__
constexpr VkExternalSemaphoreHandleTypeFlagBits kSemHandle = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
constexpr VkSemaphoreImportFlags kSemImportFlags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT; // required for SYNC_FD
constexpr uint32_t kExternalQueueFamily = VK_QUEUE_FAMILY_FOREIGN_EXT;
constexpr wgpu::SharedFenceType kDawnFenceType = wgpu::SharedFenceType::SyncFD;
using DawnFenceExportInfo = wgpu::SharedFenceSyncFDExportInfo;
using DawnFenceDescriptor = wgpu::SharedFenceSyncFDDescriptor;
#else
constexpr VkExternalSemaphoreHandleTypeFlagBits kSemHandle = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
constexpr VkSemaphoreImportFlags kSemImportFlags = 0;
constexpr uint32_t kExternalQueueFamily = VK_QUEUE_FAMILY_EXTERNAL;
constexpr wgpu::SharedFenceType kDawnFenceType = wgpu::SharedFenceType::VkSemaphoreOpaqueFD;
using DawnFenceExportInfo = wgpu::SharedFenceVkSemaphoreOpaqueFDExportInfo;
using DawnFenceDescriptor = wgpu::SharedFenceVkSemaphoreOpaqueFDDescriptor;
#endif

// ---------------------------------------------------------------- state
//
// Three image streams go from the render worker to the XR thread, each with
// its own shared images and swapchain:
//   Screen  the presented frame, on the virtual screen (menus, or any time
//           the game draws no fight)
//   Stereo  3D fights: both eyes side by side, submitted as a projection
//           layer with the head poses they were rendered for
//   Hud     the HUD of a 3D fight, on a quad above the arena

constexpr int kSlotCount = 3;

enum class SlotState {
  Free,      // render worker may take it
  Rendering, // render worker is drawing into it
  Ready,     // finished frame waiting for the XR thread
  Copying,   // XR thread is copying it into the swapchain
};

struct PendingAccess {
  // What the next acquire of this slot must wait on and acquire with. Set by
  // whichever side released the image last.
  std::vector<int> fds;
  VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct Slot {
  // Allocated on the bridge device by the XR thread.
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize allocationSize = 0;
  uint32_t memoryTypeIndex = 0;
  int exportedMemoryFd = -1; // Linux: handed to Dawn once
#ifdef __ANDROID__
  AHardwareBuffer* ahb = nullptr;
#endif

  // Render worker only.
  wgpu::SharedTextureMemory stm;
  wgpu::Texture texture;
  bool initialized = false;

  // Guarded by g_mutex.
  SlotState state = SlotState::Free;
  uint64_t readySeq = 0;
  PendingAccess forDawn;   // next Dawn BeginAccess
  PendingAccess forBridge; // Dawn's release, for the XR thread's acquire
  std::array<XrView, 2> views{}; // Stereo: the eye poses this image was rendered for

  // XR thread only.
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool inFlight = false;
  std::vector<VkSemaphore> waitSems;
  VkSemaphore signalSem = VK_NULL_HANDLE;
  VkSemaphore prevSignalSem = VK_NULL_HANDLE;
  VkImageView srcView = VK_NULL_HANDLE;           // shader copy source
  VkDescriptorSet srcSet = VK_NULL_HANDLE;
};

enum StreamId : int { kScreen = 0, kStereo = 1, kHud = 2, kStreamCount = 3 };

struct Stream {
  const char* name = "";
  std::array<Slot, kSlotCount> slots;
  uint32_t width = 0, height = 0; // set before the XR thread starts
  VkFormat format = VK_FORMAT_R8G8B8A8_UNORM; // shared images
  int64_t swapFormat = 0;                     // swapchain (the sRGB twin of `format`)
  uint64_t readySeq = 0;          // g_mutex
  int renderingSlot = -1;         // render worker

  // XR thread only.
  XrSwapchain swapchain = XR_NULL_HANDLE;
  std::vector<VkImage> swapImages;
  bool haveImage = false;
  std::chrono::steady_clock::time_point lastCopy{};
  uint64_t copies = 0;
  std::array<XrView, 2> shownViews{}; // Stereo: poses of the image in the swapchain
  // Shader copy (AURORA_XR_SHADER_COPY): draws the shared image into the
  // swapchain image instead of vkCmdCopyImage.
  VkRenderPass copyPass = VK_NULL_HANDLE;
  VkPipeline copyPipeline = VK_NULL_HANDLE;
  std::vector<VkImageView> swapViews;
  std::vector<VkFramebuffer> swapFramebuffers;
  // Debug dump (AURORA_XR_DUMP)
  VkBuffer dumpBuf = VK_NULL_HANDLE;
  VkDeviceMemory dumpMem = VK_NULL_HANDLE;
  void* dumpPtr = nullptr;
  int dumpSlot = -1;
  bool dumped = false;
};

enum class Phase { Idle, Starting, SlotsReady, Imported, Failed };

std::mutex g_mutex;
std::atomic<Phase> g_phase{Phase::Idle};
std::atomic<bool> g_stop{false};
std::atomic<bool> g_sessionRunning{false};
std::thread g_thread;
std::array<Stream, kStreamCount> g_streams;
uint32_t g_bridgeVendor = 0, g_bridgeDevice = 0;
// The game framebuffer's format and sample count, read by begin_frame before
// the XR thread starts: when they allow it, both eyes render straight into
// the shared 3D image, which then has to be in the framebuffer's format.
wgpu::TextureFormat g_sceneFormat = wgpu::TextureFormat::RGBA8Unorm;
uint32_t g_sceneSamples = 1;

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

// Controller input, written by the XR thread, read by the game thread.
std::mutex g_padMutex;
PADStatus g_pad{};
bool g_padValid = false;

// XR thread only.
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

  VkInstance vkInstance = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice dev = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t queueFamily = 0;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memProps{};
  PFN_vkGetSemaphoreFdKHR getSemaphoreFd = nullptr;
  PFN_vkImportSemaphoreFdKHR importSemaphoreFd = nullptr;
#ifdef __ANDROID__
  PFN_vkGetAndroidHardwareBufferPropertiesANDROID getAhbProps = nullptr;
#else
  PFN_vkGetMemoryFdKHR getMemoryFd = nullptr;
#endif

  bool hasRefreshRateExt = false;
  PFN_xrEnumerateDisplayRefreshRatesFB enumerateRefreshRates = nullptr;
  PFN_xrRequestDisplayRefreshRateFB requestRefreshRate = nullptr;
  PFN_xrGetDisplayRefreshRateFB getRefreshRate = nullptr;
  uint64_t displayFrame = 0;

  PFN_xrCreatePassthroughFB createPassthrough = nullptr;
  PFN_xrDestroyPassthroughFB destroyPassthrough = nullptr;
  PFN_xrCreatePassthroughLayerFB createPassthroughLayer = nullptr;
  PFN_xrDestroyPassthroughLayerFB destroyPassthroughLayer = nullptr;

  std::string dumpDir; // AURORA_XR_DUMP

  VkSampler copySampler = VK_NULL_HANDLE;
  VkDescriptorSetLayout copySetLayout = VK_NULL_HANDLE;
  VkPipelineLayout copyLayout = VK_NULL_HANDLE;
  VkDescriptorPool copyPool = VK_NULL_HANDLE;
  VkShaderModule copyVert = VK_NULL_HANDLE, copyFrag = VK_NULL_HANDLE;

  // Controller input
  XrActionSet actionSet = XR_NULL_HANDLE;
  XrAction stickMain = XR_NULL_HANDLE, stickC = XR_NULL_HANDLE;
  XrAction btnA = XR_NULL_HANDLE, btnB = XR_NULL_HANDLE, btnX = XR_NULL_HANDLE, btnY = XR_NULL_HANDLE;
  XrAction btnZ = XR_NULL_HANDLE, btnStart = XR_NULL_HANDLE;
  XrAction trigL = XR_NULL_HANDLE, trigR = XR_NULL_HANDLE;
  bool focused = false;

  uint64_t framesShown = 0, fightFrames = 0;
  std::array<uint64_t, kStreamCount> copiedSinceStats{};
  // Bridge copy timing (AURORA_XR_TIMING): 2 timestamps per slot per stream.
  VkQueryPool timestamps = VK_NULL_HANDLE;
  float timestampPeriodNs = 1.f;
  std::array<double, kStreamCount> copyNs{};
  std::array<uint64_t, kStreamCount> copySamples{};
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

// The shared image's create info. The render worker rebuilds the identical
// struct for Dawn's opaque-FD import, so it lives in one place.
VkImageCreateInfo shared_image_info(VkExternalMemoryImageCreateInfo& ext, uint32_t width, uint32_t height,
                                    VkFormat format) {
  ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
#ifdef __ANDROID__
  ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
#else
  ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.pNext = &ext;
#ifndef __ANDROID__
  // Dawn requires MUTABLE_FORMAT to offer RGBA8Unorm/RGBA8UnormSrgb views.
  ici.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
#endif
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = format;
  ici.extent = {width, height, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
              VK_IMAGE_USAGE_SAMPLED_BIT;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  return ici;
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
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
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
  return true;
}

bool create_bridge_device() {
  auto getReqs = xr_proc<PFN_xrGetVulkanGraphicsRequirements2KHR>("xrGetVulkanGraphicsRequirements2KHR");
  auto createInst = xr_proc<PFN_xrCreateVulkanInstanceKHR>("xrCreateVulkanInstanceKHR");
  auto getDev = xr_proc<PFN_xrGetVulkanGraphicsDevice2KHR>("xrGetVulkanGraphicsDevice2KHR");
  auto createDev = xr_proc<PFN_xrCreateVulkanDeviceKHR>("xrCreateVulkanDeviceKHR");
  if (!getReqs || !createInst || !getDev || !createDev) {
    Log.error("XR_KHR_vulkan_enable2 entry points missing");
    return false;
  }
  XrGraphicsRequirementsVulkan2KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
  XR_TRY(getReqs(B.instance, B.systemId, &reqs));

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "melee-pc xr bridge";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  XrVulkanInstanceCreateInfoKHR xici{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
  xici.systemId = B.systemId;
  xici.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
  xici.vulkanCreateInfo = &ici;
  VkResult vr = VK_SUCCESS;
  XR_TRY(createInst(B.instance, &xici, &B.vkInstance, &vr));
  VK_TRY(vr);

  XrVulkanGraphicsDeviceGetInfoKHR gi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
  gi.systemId = B.systemId;
  gi.vulkanInstance = B.vkInstance;
  XR_TRY(getDev(B.instance, &gi, &B.phys));
  vkGetPhysicalDeviceMemoryProperties(B.phys, &B.memProps);
  VkPhysicalDeviceProperties pp;
  vkGetPhysicalDeviceProperties(B.phys, &pp);
  g_bridgeVendor = pp.vendorID;
  g_bridgeDevice = pp.deviceID;

  uint32_t n = 0;
  vkEnumerateDeviceExtensionProperties(B.phys, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> have(n);
  vkEnumerateDeviceExtensionProperties(B.phys, nullptr, &n, have.data());
#ifdef __ANDROID__
  std::vector<const char*> need{VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
                                VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
                                VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
#else
  std::vector<const char*> need{VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
#endif
  for (const char* e : need) {
    if (std::none_of(have.begin(), have.end(), [&](const auto& p) { return !std::strcmp(p.extensionName, e); })) {
      Log.error("Bridge GPU lacks {}", e);
      return false;
    }
  }

  vkGetPhysicalDeviceQueueFamilyProperties(B.phys, &n, nullptr);
  std::vector<VkQueueFamilyProperties> qfs(n);
  vkGetPhysicalDeviceQueueFamilyProperties(B.phys, &n, qfs.data());
  B.queueFamily = UINT32_MAX;
  for (uint32_t i = 0; i < n && B.queueFamily == UINT32_MAX; ++i)
    if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
      B.queueFamily = i;
  if (B.queueFamily == UINT32_MAX) {
    Log.error("Bridge GPU has no graphics queue");
    return false;
  }
  const float prio = 1.f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = B.queueFamily;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = static_cast<uint32_t>(need.size());
  dci.ppEnabledExtensionNames = need.data();
  XrVulkanDeviceCreateInfoKHR xdci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
  xdci.systemId = B.systemId;
  xdci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
  xdci.vulkanPhysicalDevice = B.phys;
  xdci.vulkanCreateInfo = &dci;
  XR_TRY(createDev(B.instance, &xdci, &B.dev, &vr));
  VK_TRY(vr);
  vkGetDeviceQueue(B.dev, B.queueFamily, 0, &B.queue);

  B.getSemaphoreFd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(B.dev, "vkGetSemaphoreFdKHR"));
  B.importSemaphoreFd =
      reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(vkGetDeviceProcAddr(B.dev, "vkImportSemaphoreFdKHR"));
#ifdef __ANDROID__
  B.getAhbProps = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
      vkGetDeviceProcAddr(B.dev, "vkGetAndroidHardwareBufferPropertiesANDROID"));
  if (!B.getAhbProps) {
    Log.error("vkGetAndroidHardwareBufferPropertiesANDROID missing");
    return false;
  }
#else
  B.getMemoryFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(B.dev, "vkGetMemoryFdKHR"));
  if (!B.getMemoryFd) {
    Log.error("vkGetMemoryFdKHR missing");
    return false;
  }
#endif
  if (!B.getSemaphoreFd || !B.importSemaphoreFd) {
    Log.error("Semaphore fd entry points missing");
    return false;
  }

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = B.queueFamily;
  VK_TRY(vkCreateCommandPool(B.dev, &pci, nullptr, &B.pool));
  if (env_flag("AURORA_XR_TIMING", true) && qfs[B.queueFamily].timestampValidBits > 0) {
    VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpci.queryCount = kStreamCount * kSlotCount * 2;
    if (vkCreateQueryPool(B.dev, &qpci, nullptr, &B.timestamps) == VK_SUCCESS)
      B.timestampPeriodNs = pp.limits.timestampPeriod;
  }
  Log.info("Bridge VkDevice: {} [{:04x}:{:04x}]", pp.deviceName, pp.vendorID, pp.deviceID);
  return true;
}

bool create_slot(Slot& s, uint32_t width, uint32_t height, VkFormat format) {
  VkExternalMemoryImageCreateInfo ext;
  VkImageCreateInfo ici = shared_image_info(ext, width, height, format);
#ifdef __ANDROID__
  AHardwareBuffer_Desc ad{};
  ad.width = width;
  ad.height = height;
  ad.layers = 1;
  ad.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
  ad.usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
  if (AHardwareBuffer_allocate(&ad, &s.ahb) != 0) {
    Log.error("AHardwareBuffer_allocate failed");
    return false;
  }
  VkAndroidHardwareBufferFormatPropertiesANDROID fp{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
  VkAndroidHardwareBufferPropertiesANDROID hp{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
  hp.pNext = &fp;
  VK_TRY(B.getAhbProps(B.dev, s.ahb, &hp));
  VK_TRY(vkCreateImage(B.dev, &ici, nullptr, &s.image));
  VkImportAndroidHardwareBufferInfoANDROID imp{VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
  imp.buffer = s.ahb;
  VkMemoryDedicatedAllocateInfo dai{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  dai.pNext = &imp;
  dai.image = s.image;
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.pNext = &dai;
  mai.allocationSize = hp.allocationSize;
  mai.memoryTypeIndex = find_memory_type(hp.memoryTypeBits, 0);
#else
  VK_TRY(vkCreateImage(B.dev, &ici, nullptr, &s.image));
  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(B.dev, s.image, &mr);
  VkExportMemoryAllocateInfo emai{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
  emai.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkMemoryDedicatedAllocateInfo dai{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  dai.pNext = &emai;
  dai.image = s.image;
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.pNext = &dai;
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex = find_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
#endif
  if (mai.memoryTypeIndex == UINT32_MAX) {
    Log.error("No memory type for the shared image");
    return false;
  }
  VK_TRY(vkAllocateMemory(B.dev, &mai, nullptr, &s.memory));
  VK_TRY(vkBindImageMemory(B.dev, s.image, s.memory, 0));
  s.allocationSize = mai.allocationSize;
  s.memoryTypeIndex = mai.memoryTypeIndex;
#ifndef __ANDROID__
  VkMemoryGetFdInfoKHR gfi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
  gfi.memory = s.memory;
  gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  VK_TRY(B.getMemoryFd(B.dev, &gfi, &s.exportedMemoryFd));
#endif
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = B.pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  VK_TRY(vkAllocateCommandBuffers(B.dev, &cai, &s.cmd));
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VK_TRY(vkCreateFence(B.dev, &fci, nullptr, &s.fence));
  return true;
}

bool create_swapchain(Stream& st) {
  XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                  XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
  ci.format = st.swapFormat;
  ci.sampleCount = 1;
  ci.width = st.width;
  ci.height = st.height;
  ci.faceCount = 1;
  ci.arraySize = 1;
  ci.mipCount = 1;
  XR_TRY(xrCreateSwapchain(B.session, &ci, &st.swapchain));
  uint32_t n = 0;
  XR_TRY(xrEnumerateSwapchainImages(st.swapchain, 0, &n, nullptr));
  std::vector<XrSwapchainImageVulkan2KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
  XR_TRY(xrEnumerateSwapchainImages(st.swapchain, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data())));
  for (const auto& im : imgs)
    st.swapImages.push_back(im.image);
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
  g_streams[kStereo].width = eyeW * 2;
  g_streams[kStereo].height = eyeH;
  return true;
}

bool create_session() {
  XrGraphicsBindingVulkan2KHR gb{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
  gb.instance = B.vkInstance;
  gb.physicalDevice = B.phys;
  gb.device = B.dev;
  gb.queueFamilyIndex = B.queueFamily;
  gb.queueIndex = 0;
  XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
  sci.next = &gb;
  sci.systemId = B.systemId;
  XR_TRY(xrCreateSession(B.instance, &sci, &B.session));

  XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  rsci.poseInReferenceSpace.orientation.w = 1.f;
  XR_TRY(xrCreateReferenceSpace(B.session, &rsci, &B.space));

  // The game's frames are already sRGB-encoded bytes in RGBA8Unorm textures.
  // Copying those bytes unchanged into *_SRGB swapchains makes the runtime
  // decode them correctly; a blit would re-encode them and wash them out.
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
  // The copy into the swapchain moves bytes unchanged, so the swapchain must
  // match too.
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
           g_streams[kStereo].width / 2, g_streams[kStereo].height, g_streams[kStereo].swapFormat,
           g_streams[kHud].width, g_streams[kHud].height, B.swapFormat, B.passthroughLayer ? "on" : "off");
  return true;
}

// Shader copy: on Adreno, vkCmdCopyImage of the 3D image ran at ~13 GB/s
// (3.7 ms for 3360x1760 each way); a full-screen draw goes through the 3D
// pipe and the tile memory instead.
bool create_shader_copy() {
  const auto module = [](const uint32_t* code, size_t bytes, VkShaderModule& out) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    return vkCreateShaderModule(B.dev, &ci, nullptr, &out);
  };
  VK_TRY(module(kCopyVertSpv, sizeof(kCopyVertSpv), B.copyVert));
  VK_TRY(module(kCopyFragSpv, sizeof(kCopyFragSpv), B.copyFrag));
  VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
  sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VK_TRY(vkCreateSampler(B.dev, &sci, nullptr, &B.copySampler));
  VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                       &B.copySampler};
  VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dslci.bindingCount = 1;
  dslci.pBindings = &binding;
  VK_TRY(vkCreateDescriptorSetLayout(B.dev, &dslci, nullptr, &B.copySetLayout));
  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &B.copySetLayout;
  VK_TRY(vkCreatePipelineLayout(B.dev, &plci, nullptr, &B.copyLayout));
  VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kStreamCount * kSlotCount};
  VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.maxSets = kStreamCount * kSlotCount;
  dpci.poolSizeCount = 1;
  dpci.pPoolSizes = &poolSize;
  VK_TRY(vkCreateDescriptorPool(B.dev, &dpci, nullptr, &B.copyPool));

  for (auto& st : g_streams) {
    // Render pass: the swapchain image is fully overwritten (UNDEFINED in)
    // and handed back in COLOR_ATTACHMENT_OPTIMAL, as OpenXR expects.
    VkAttachmentDescription att{};
    att.format = static_cast<VkFormat>(st.swapFormat);
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    std::array<VkSubpassDependency, 2> deps{{
        {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0},
        {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT, 0},
    }};
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = static_cast<uint32_t>(deps.size());
    rpci.pDependencies = deps.data();
    VK_TRY(vkCreateRenderPass(B.dev, &rpci, nullptr, &st.copyPass));

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, B.copyVert,
         "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, B.copyFrag,
         "main", nullptr},
    }};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0.f, 0.f, static_cast<float>(st.width), static_cast<float>(st.height), 0.f, 1.f};
    VkRect2D scissor{{0, 0}, {st.width, st.height}};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.pViewports = &viewport;
    vp.scissorCount = 1;
    vp.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                         VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = static_cast<uint32_t>(stages.size());
    gp.pStages = stages.data();
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.layout = B.copyLayout;
    gp.renderPass = st.copyPass;
    VK_TRY(vkCreateGraphicsPipelines(B.dev, VK_NULL_HANDLE, 1, &gp, nullptr, &st.copyPipeline));

    for (VkImage img : st.swapImages) {
      VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      ivci.image = img;
      ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
      ivci.format = static_cast<VkFormat>(st.swapFormat);
      ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkImageView view = VK_NULL_HANDLE;
      VK_TRY(vkCreateImageView(B.dev, &ivci, nullptr, &view));
      st.swapViews.push_back(view);
      VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fci.renderPass = st.copyPass;
      fci.attachmentCount = 1;
      fci.pAttachments = &view;
      fci.width = st.width;
      fci.height = st.height;
      fci.layers = 1;
      VkFramebuffer fb = VK_NULL_HANDLE;
      VK_TRY(vkCreateFramebuffer(B.dev, &fci, nullptr, &fb));
      st.swapFramebuffers.push_back(fb);
    }
    for (auto& s : st.slots) {
      VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      ivci.image = s.image;
      ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
      ivci.format = st.format;
      ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VK_TRY(vkCreateImageView(B.dev, &ivci, nullptr, &s.srcView));
      VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      dsai.descriptorPool = B.copyPool;
      dsai.descriptorSetCount = 1;
      dsai.pSetLayouts = &B.copySetLayout;
      VK_TRY(vkAllocateDescriptorSets(B.dev, &dsai, &s.srcSet));
      VkDescriptorImageInfo dii{VK_NULL_HANDLE, s.srcView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = s.srcSet;
      w.descriptorCount = 1;
      w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      w.pImageInfo = &dii;
      vkUpdateDescriptorSets(B.dev, 1, &w, 0, nullptr);
    }
  }
  return true;
}

// AURORA_XR_DUMP=<dir>: write each stream's image once (after 300 copies) as
// <dir>/xr_<stream>.ppm and <dir>/xr_<stream>_alpha.pgm, for checking what
// reaches the headset without one.
bool create_dump_buffer(Stream& st) {
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = static_cast<VkDeviceSize>(st.width) * st.height * 4;
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
    std::fprintf(f, "P6\n%u %u\n255\n", st.width, st.height);
    for (size_t i = 0; i < static_cast<size_t>(st.width) * st.height; ++i)
      std::fwrite(px + i * 4, 1, 3, f);
    std::fclose(f);
  }
  if (FILE* f = std::fopen((base + "_alpha.pgm").c_str(), "wb")) {
    std::fprintf(f, "P5\n%u %u\n255\n", st.width, st.height);
    for (size_t i = 0; i < static_cast<size_t>(st.width) * st.height; ++i)
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
      !create_action(B.trigR, "r", "R", XR_ACTION_TYPE_FLOAT_INPUT))
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
                             });
  // Minimal fallback for runtimes and simulators without Touch controllers
  // (Monado's keyboard/mouse controllers): select is A, menu is Start.
  suggest("/interaction_profiles/khr/simple_controller", {
                                                             {B.btnA, "/user/hand/right/input/select/click"},
                                                             {B.btnB, "/user/hand/left/input/select/click"},
                                                             {B.btnStart, "/user/hand/left/input/menu/click"},
                                                             {B.btnStart, "/user/hand/right/input/menu/click"},
                                                         });
  XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  attach.countActionSets = 1;
  attach.actionSets = &B.actionSet;
  XR_TRY(xrAttachSessionActionSets(B.session, &attach));
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
  buttons |= action_bool(B.btnZ) ? PAD_TRIGGER_Z : 0;
  buttons |= action_bool(B.btnStart) ? PAD_BUTTON_START : 0;
  buttons |= l > 0.9f ? PAD_TRIGGER_L : 0;
  buttons |= r > 0.9f ? PAD_TRIGGER_R : 0;
  pad.button = buttons;
  std::lock_guard lock{g_padMutex};
  g_pad = pad;
  g_padValid = true;
}

// ---------------------------------------------------------------- XR thread: per frame

// Wait for the slot's previous copy and release what it no longer needs.
void retire_slot(Stream& st, Slot& s, int index) {
  if (!s.inFlight)
    return;
  vkWaitForFences(B.dev, 1, &s.fence, VK_TRUE, UINT64_MAX);
  vkResetFences(B.dev, 1, &s.fence);
  s.inFlight = false;
  for (VkSemaphore sem : s.waitSems)
    vkDestroySemaphore(B.dev, sem, nullptr);
  s.waitSems.clear();
  // Dawn waited on prevSignalSem before producing the frame this finished
  // copy consumed, so nothing references it any more.
  if (s.prevSignalSem)
    vkDestroySemaphore(B.dev, s.prevSignalSem, nullptr);
  s.prevSignalSem = s.signalSem;
  s.signalSem = VK_NULL_HANDLE;
  if (B.timestamps) {
    const auto streamIndex = static_cast<uint32_t>(&st - g_streams.data());
    const uint32_t q = (streamIndex * kSlotCount + static_cast<uint32_t>(index)) * 2;
    uint64_t ts[2];
    if (vkGetQueryPoolResults(B.dev, B.timestamps, q, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) ==
            VK_SUCCESS &&
        ts[1] > ts[0]) {
      B.copyNs[streamIndex] += static_cast<double>(ts[1] - ts[0]) * B.timestampPeriodNs;
      ++B.copySamples[streamIndex];
    }
  }
  if (st.dumpSlot == index) {
    st.dumpSlot = -1;
    st.dumped = true;
    write_dump(st);
  }
}

// Copy the stream's newest finished frame into its swapchain. Returns false
// on a fatal error; `copied` says whether there was a new frame.
bool copy_latest(Stream& st, bool& copied) {
  copied = false;
  int index = -1;
  PendingAccess dawnRelease;
  std::array<XrView, 2> views{};
  {
    std::lock_guard lock{g_mutex};
    uint64_t best = 0;
    for (int i = 0; i < kSlotCount; ++i) {
      if (st.slots[i].state == SlotState::Ready && st.slots[i].readySeq > best) {
        best = st.slots[i].readySeq;
        index = i;
      }
    }
    if (index < 0)
      return true;
    st.slots[index].state = SlotState::Copying;
    dawnRelease = std::move(st.slots[index].forBridge);
    st.slots[index].forBridge = {};
    views = st.slots[index].views;
  }
  Slot& s = st.slots[index];
  retire_slot(st, s, index);

  for (int fd : dawnRelease.fds) {
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

  uint32_t swapIndex = 0;
  XR_TRY(xrAcquireSwapchainImage(st.swapchain, nullptr, &swapIndex));
  XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wi.timeout = XR_INFINITE_DURATION;
  XR_TRY(xrWaitSwapchainImage(st.swapchain, &wi));
  VkImage swapImg = st.swapImages[swapIndex];

  VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_TRY(vkResetCommandBuffer(s.cmd, 0));
  VK_TRY(vkBeginCommandBuffer(s.cmd, &cbi));
  const uint32_t tsQuery =
      (static_cast<uint32_t>(&st - g_streams.data()) * kSlotCount + static_cast<uint32_t>(index)) * 2;
  if (B.timestamps)
    vkCmdResetQueryPool(s.cmd, B.timestamps, tsQuery, 2);
  // Dump frames read the swapchain image back after the copy, so the dump
  // shows exactly what the runtime gets.
  const bool dumpNow = !B.dumpDir.empty() && !st.dumped && st.dumpSlot < 0 && st.copies >= 300 &&
                       (st.dumpBuf || create_dump_buffer(st));
  const bool shaderCopy = st.copyPipeline != VK_NULL_HANDLE;
  // The layout the shared image is used in here, and handed back to Dawn
  // from (Dawn acquires with the same pair).
  const VkImageLayout srcLayout =
      shaderCopy ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  const VkAccessFlags srcAccess = shaderCopy ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_TRANSFER_READ_BIT;
  const VkPipelineStageFlags srcStage =
      shaderCopy ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT;
  std::array<VkImageMemoryBarrier, 3> pre{};
  uint32_t npre = 0;
  // Acquire from Dawn: must mirror Dawn's release barrier exactly.
  pre[npre++] = barrier(s.image, dawnRelease.oldLayout, dawnRelease.newLayout, 0, srcAccess, kExternalQueueFamily,
                        B.queueFamily);
  if (dawnRelease.newLayout != srcLayout)
    pre[npre++] = barrier(s.image, dawnRelease.newLayout, srcLayout, srcAccess, srcAccess);
  if (!shaderCopy) {
    // OpenXR hands Vulkan swapchain images over in COLOR_ATTACHMENT_OPTIMAL.
    pre[npre++] = barrier(swapImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  }
  vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, srcStage | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                       nullptr, 0, nullptr, npre, pre.data());
  if (B.timestamps)
    vkCmdWriteTimestamp(s.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, B.timestamps, tsQuery);
  if (shaderCopy) {
    VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpbi.renderPass = st.copyPass;
    rpbi.framebuffer = st.swapFramebuffers[swapIndex];
    rpbi.renderArea = {{0, 0}, {st.width, st.height}};
    vkCmdBeginRenderPass(s.cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, st.copyPipeline);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, B.copyLayout, 0, 1, &s.srcSet, 0, nullptr);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(s.cmd);
  } else {
    VkImageCopy region{};
    region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {st.width, st.height, 1};
    vkCmdCopyImage(s.cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapImg,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  }
  if (B.timestamps)
    vkCmdWriteTimestamp(s.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, B.timestamps, tsQuery + 1);
  // The swapchain image is now in COLOR_ATTACHMENT_OPTIMAL (shader copy) or
  // TRANSFER_DST_OPTIMAL (transfer copy).
  VkImageLayout swapLayout =
      shaderCopy ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  if (dumpNow) {
    const VkImageMemoryBarrier toSrc =
        barrier(swapImg, swapLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);
    VkBufferImageCopy rb{};
    rb.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rb.imageExtent = {st.width, st.height, 1};
    vkCmdCopyImageToBuffer(s.cmd, swapImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, st.dumpBuf, 1, &rb);
    st.dumpSlot = index;
    swapLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  }
  std::array<VkImageMemoryBarrier, 2> post{};
  uint32_t npost = 0;
  if (swapLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
    post[npost++] = barrier(swapImg, swapLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
  }
  // Release back to Dawn, which acquires with the same layouts.
  post[npost++] = barrier(s.image, srcLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, srcAccess, 0, B.queueFamily,
                          kExternalQueueFamily);
  vkCmdPipelineBarrier(s.cmd, srcStage | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, npost, post.data());
  VK_TRY(vkEndCommandBuffer(s.cmd));

  VkExportSemaphoreCreateInfo esci{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
  esci.handleTypes = kSemHandle;
  VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  sci.pNext = &esci;
  VK_TRY(vkCreateSemaphore(B.dev, &sci, nullptr, &s.signalSem));
  std::vector<VkPipelineStageFlags> stages(s.waitSems.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.waitSemaphoreCount = static_cast<uint32_t>(s.waitSems.size());
  si.pWaitSemaphores = s.waitSems.data();
  si.pWaitDstStageMask = stages.data();
  si.commandBufferCount = 1;
  si.pCommandBuffers = &s.cmd;
  si.signalSemaphoreCount = 1;
  si.pSignalSemaphores = &s.signalSem;
  VK_TRY(vkQueueSubmit(B.queue, 1, &si, s.fence));
  s.inFlight = true;
  XR_TRY(xrReleaseSwapchainImage(st.swapchain, nullptr));

  VkSemaphoreGetFdInfoKHR gfi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
  gfi.semaphore = s.signalSem;
  gfi.handleType = kSemHandle;
  int fd = -1;
  VK_TRY(B.getSemaphoreFd(B.dev, &gfi, &fd)); // SYNC_FD may give -1: already signalled
  {
    std::lock_guard lock{g_mutex};
    s.forDawn.fds.clear();
    if (fd >= 0)
      s.forDawn.fds.push_back(fd);
    s.forDawn.oldLayout = srcLayout;
    s.forDawn.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    s.state = SlotState::Free;
  }
  st.haveImage = true;
  st.lastCopy = std::chrono::steady_clock::now();
  st.shownViews = views;
  ++st.copies;
  copied = true;
  return true;
}

// The game draws 60 frames a second, so the display should run at a
// multiple of 60 to show every game frame for the same number of display
// frames (a 72 Hz display repeats every fifth one: visible stutter). Prefer
// AURORA_XR_REFRESH if offered, then 60, then 120.
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
  }
}

// Where the arena sits in the room: AURORA_XR_ARENA_POS ("x,y,z" meters in
// the starting head space; default a little below eye level, 1 m ahead).
XrVector3f arena_position() {
  XrVector3f pos{0.f, -0.45f, -1.0f};
  if (const char* v = std::getenv("AURORA_XR_ARENA_POS")) {
    std::sscanf(v, "%f,%f,%f", &pos.x, &pos.y, &pos.z);
  }
  return pos;
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
  if (fs.shouldRender) {
    for (int i = 0; i < kStreamCount; ++i) {
      bool copied = false;
      if (!copy_latest(g_streams[i], copied))
        return false;
      B.copiedSinceStats[i] += copied ? 1 : 0;
    }
  }
  // A fight is on while 3D frames keep coming; otherwise the virtual screen.
  const auto& stereo = g_streams[kStereo];
  const auto& hud = g_streams[kHud];
  const bool fight = stereo.haveImage && now - stereo.lastCopy < std::chrono::milliseconds(250);

  std::array<const XrCompositionLayerBaseHeader*, 3> layers{};
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
  if (fs.shouldRender && fight) {
    // Premultiplied alpha (no UNPREMULTIPLIED bit): the arena's coverage
    // hides the room, effects outside it add light over passthrough.
    const int32_t eyeW = static_cast<int32_t>(stereo.width / 2);
    for (int i = 0; i < 2; ++i) {
      projViews[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
      projViews[i].pose = stereo.shownViews[i].pose;
      projViews[i].fov = stereo.shownViews[i].fov;
      projViews[i].subImage.swapchain = stereo.swapchain;
      projViews[i].subImage.imageRect = {{i * eyeW, 0}, {eyeW, static_cast<int32_t>(stereo.height)}};
    }
    proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    proj.space = B.space;
    proj.viewCount = 2;
    proj.views = projViews.data();
    layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
    if (hud.haveImage && now - hud.lastCopy < std::chrono::milliseconds(250)) {
      // The HUD floats above the arena's back edge, like a scoreboard.
      const XrVector3f arena = arena_position();
      const float width = env_float("AURORA_XR_HUD_WIDTH", 0.9f);
      hudQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
      hudQuad.space = B.space;
      hudQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
      hudQuad.subImage.swapchain = hud.swapchain;
      hudQuad.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(hud.width), static_cast<int32_t>(hud.height)}};
      hudQuad.pose.orientation.w = 1.f;
      hudQuad.pose.position = {arena.x, arena.y + env_float("AURORA_XR_HUD_HEIGHT", 0.55f), arena.z - 0.15f};
      hudQuad.size = {width, width * static_cast<float>(hud.height) / static_cast<float>(hud.width)};
      layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hudQuad);
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
    Log.info("{:.1f} display fps ({:.0f}% 3D); frames/s copied: screen {:.1f}, 3D {:.1f}, HUD {:.1f}",
             B.framesShown / secs, 100.0 * B.fightFrames / std::max<uint64_t>(B.framesShown, 1),
             B.copiedSinceStats[kScreen] / secs, B.copiedSinceStats[kStereo] / secs, B.copiedSinceStats[kHud] / secs);
    if (B.timestamps) {
      const auto avg = [](int i) { return B.copySamples[i] ? B.copyNs[i] / B.copySamples[i] / 1000.0 : 0.0; };
      Log.info("GPU bridge copy per frame (us): screen {:.0f}, 3D {:.0f}, HUD {:.0f}", avg(kScreen), avg(kStereo),
               avg(kHud));
      B.copyNs = {};
      B.copySamples = {};
    }
    B.framesShown = 0;
    B.fightFrames = 0;
    B.copiedSinceStats = {};
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
  if (!create_instance() || !create_bridge_device() || !size_stereo_stream() || !create_session())
    return false;
  for (auto& st : g_streams) {
    for (auto& s : st.slots)
      if (!create_slot(s, st.width, st.height, st.format))
        return false;
    if (!create_swapchain(st))
      return false;
  }
  if (env_flag("AURORA_XR_SHADER_COPY", true) && !create_shader_copy()) {
    Log.warn("Shader copy unavailable; copying with vkCmdCopyImage");
    for (auto& st : g_streams)
      st.copyPipeline = VK_NULL_HANDLE;
  }
  if (!create_input())
    Log.warn("Controller input unavailable");
  if (const char* dir = std::getenv("AURORA_XR_DUMP"); dir != nullptr && *dir != '\0')
    B.dumpDir = dir;
  // Not every exit path calls aurora::shutdown, so stop the XR thread from an
  // exit handler too. It must be registered only now: exit handlers run in
  // reverse order, and the runtime and Vulkan driver libraries loaded above
  // register their own teardown, which has to run after ours.
  std::atexit([] { shutdown(); });
  return true;
}

// Destroy everything the XR thread created, newest first. Leaving the
// session or instance alive at exit lets the loader unload the runtime while
// the runtime's own threads still run, which crashes the process.
void teardown() {
  if (B.dev)
    vkDeviceWaitIdle(B.dev);
  if (B.dev) {
    // Views and framebuffers of the shared and swapchain images go first.
    for (auto& st : g_streams) {
      for (auto fb : st.swapFramebuffers)
        vkDestroyFramebuffer(B.dev, fb, nullptr);
      for (auto v : st.swapViews)
        vkDestroyImageView(B.dev, v, nullptr);
      st.swapFramebuffers.clear();
      st.swapViews.clear();
      if (st.copyPipeline)
        vkDestroyPipeline(B.dev, st.copyPipeline, nullptr);
      if (st.copyPass)
        vkDestroyRenderPass(B.dev, st.copyPass, nullptr);
      st.copyPipeline = VK_NULL_HANDLE;
      st.copyPass = VK_NULL_HANDLE;
      for (auto& s : st.slots) {
        if (s.srcView)
          vkDestroyImageView(B.dev, s.srcView, nullptr);
        s.srcView = VK_NULL_HANDLE;
      }
    }
    if (B.copyPool)
      vkDestroyDescriptorPool(B.dev, B.copyPool, nullptr);
    if (B.copyLayout)
      vkDestroyPipelineLayout(B.dev, B.copyLayout, nullptr);
    if (B.copySetLayout)
      vkDestroyDescriptorSetLayout(B.dev, B.copySetLayout, nullptr);
    if (B.copySampler)
      vkDestroySampler(B.dev, B.copySampler, nullptr);
    for (auto m : {B.copyVert, B.copyFrag})
      if (m)
        vkDestroyShaderModule(B.dev, m, nullptr);
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
    // Dawn holds its own imports of the shared images, so releasing the
    // bridge's copies does not affect frames it is still drawing.
    for (auto& st : g_streams) {
      for (auto& s : st.slots) {
        for (VkSemaphore sem : s.waitSems)
          vkDestroySemaphore(B.dev, sem, nullptr);
        s.waitSems.clear();
        for (VkSemaphore sem : {s.signalSem, s.prevSignalSem})
          if (sem)
            vkDestroySemaphore(B.dev, sem, nullptr);
        s.signalSem = s.prevSignalSem = VK_NULL_HANDLE;
        if (s.fence)
          vkDestroyFence(B.dev, s.fence, nullptr);
        if (s.image)
          vkDestroyImage(B.dev, s.image, nullptr);
        if (s.memory)
          vkFreeMemory(B.dev, s.memory, nullptr);
        s.fence = VK_NULL_HANDLE;
        s.image = VK_NULL_HANDLE;
        s.memory = VK_NULL_HANDLE;
      }
      if (st.dumpBuf)
        vkDestroyBuffer(B.dev, st.dumpBuf, nullptr);
      if (st.dumpMem)
        vkFreeMemory(B.dev, st.dumpMem, nullptr);
      st.dumpBuf = VK_NULL_HANDLE;
      st.dumpMem = VK_NULL_HANDLE;
    }
    if (B.timestamps)
      vkDestroyQueryPool(B.dev, B.timestamps, nullptr);
    B.timestamps = VK_NULL_HANDLE;
    if (B.pool)
      vkDestroyCommandPool(B.dev, B.pool, nullptr);
    vkDestroyDevice(B.dev, nullptr);
    B.dev = VK_NULL_HANDLE;
  }
  if (B.vkInstance) {
    vkDestroyInstance(B.vkInstance, nullptr);
    B.vkInstance = VK_NULL_HANDLE;
  }
  if (B.instance) {
    xrDestroyInstance(B.instance);
    B.instance = XR_NULL_HANDLE;
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
  teardown();
}

// ---------------------------------------------------------------- render worker: shared images

bool import_slots_into_dawn() {
  auto& device = webgpu::g_device;
  if (webgpu::g_adapterInfo.vendorID != g_bridgeVendor || webgpu::g_adapterInfo.deviceID != g_bridgeDevice) {
    Log.error("Dawn uses GPU [{:04x}:{:04x}] but the OpenXR runtime wants [{:04x}:{:04x}]",
              webgpu::g_adapterInfo.vendorID, webgpu::g_adapterInfo.deviceID, g_bridgeVendor, g_bridgeDevice);
    return false;
  }
#ifdef __ANDROID__
  const bool featuresOk = device.HasFeature(wgpu::FeatureName::SharedTextureMemoryAHardwareBuffer) &&
                          device.HasFeature(wgpu::FeatureName::SharedFenceSyncFD);
#else
  const bool featuresOk = device.HasFeature(wgpu::FeatureName::SharedTextureMemoryOpaqueFD) &&
                          device.HasFeature(wgpu::FeatureName::SharedFenceVkSemaphoreOpaqueFD);
#endif
  if (!featuresOk) {
    Log.error("Dawn device lacks shared texture/fence features");
    return false;
  }
  for (auto& st : g_streams) {
    for (auto& s : st.slots) {
      wgpu::SharedTextureMemoryDescriptor sd{};
      sd.label = st.name;
#ifdef __ANDROID__
      wgpu::SharedTextureMemoryAHardwareBufferDescriptor ahbd{};
      ahbd.handle = s.ahb;
      sd.nextInChain = &ahbd;
      s.stm = device.ImportSharedTextureMemory(&sd);
#else
      VkExternalMemoryImageCreateInfo ext;
      VkImageCreateInfo ici = shared_image_info(ext, st.width, st.height, st.format);
      wgpu::SharedTextureMemoryOpaqueFDDescriptor od{};
      od.vkImageCreateInfo = &ici;
      od.memoryFD = s.exportedMemoryFd;
      od.memoryTypeIndex = s.memoryTypeIndex;
      od.allocationSize = s.allocationSize;
      od.dedicatedAllocation = true;
      sd.nextInChain = &od;
      s.stm = device.ImportSharedTextureMemory(&sd);
      close(s.exportedMemoryFd); // Dawn duplicates the handle on import
      s.exportedMemoryFd = -1;
#endif
      if (!s.stm) {
        Log.error("ImportSharedTextureMemory failed ({})", st.name);
        return false;
      }
      s.texture = s.stm.CreateTexture();
      if (!s.texture) {
        Log.error("SharedTextureMemory::CreateTexture failed ({})", st.name);
        return false;
      }
    }
  }
  Log.info("Shared images imported into Dawn: screen {}x{}, 3D {}x{}, HUD {}x{}", g_streams[kScreen].width,
           g_streams[kScreen].height, g_streams[kStereo].width, g_streams[kStereo].height, g_streams[kHud].width,
           g_streams[kHud].height);
  return true;
}

// Pick a slot for the next frame (render worker, g_mutex held).
int take_slot_locked(Stream& st) {
  for (int i = 0; i < kSlotCount; ++i)
    if (st.slots[i].state == SlotState::Free)
      return i;
  // No free slot: reclaim the oldest finished frame the XR thread skipped.
  // Nothing on the bridge side touched it, so Dawn waits on its own release.
  int oldest = -1;
  for (int i = 0; i < kSlotCount; ++i)
    if (st.slots[i].state == SlotState::Ready &&
        (oldest < 0 || st.slots[i].readySeq < st.slots[oldest].readySeq))
      oldest = i;
  if (oldest >= 0) {
    Slot& s = st.slots[oldest];
    s.forDawn = std::move(s.forBridge);
    s.forBridge = {};
  }
  return oldest;
}

// Begin Dawn access to a free slot of the stream; null if none.
wgpu::Texture acquire_slot(Stream& st) {
  int index;
  PendingAccess access;
  {
    std::lock_guard lock{g_mutex};
    index = take_slot_locked(st);
    if (index < 0)
      return {};
    st.slots[index].state = SlotState::Rendering;
    access = std::move(st.slots[index].forDawn);
    st.slots[index].forDawn = {};
  }
  Slot& s = st.slots[index];
  std::vector<wgpu::SharedFence> fences;
  for (int fd : access.fds) {
    DawnFenceDescriptor fdd{};
    fdd.handle = fd;
    wgpu::SharedFenceDescriptor desc{};
    desc.nextInChain = &fdd;
    fences.push_back(webgpu::g_device.ImportSharedFence(&desc));
    close(fd); // Dawn duplicates on import
  }
  std::vector<uint64_t> values(fences.size(), 1);
  wgpu::SharedTextureMemoryVkImageLayoutBeginState bs{};
  bs.oldLayout = access.oldLayout;
  bs.newLayout = access.newLayout;
  wgpu::SharedTextureMemoryBeginAccessDescriptor bd{};
  bd.nextInChain = &bs;
  bd.initialized = s.initialized;
  bd.fenceCount = fences.size();
  bd.fences = fences.data();
  bd.signaledValueCount = values.size();
  bd.signaledValues = values.data();
  if (s.stm.BeginAccess(s.texture, &bd) != wgpu::Status::Success) {
    Log.error("BeginAccess failed ({}); XR presentation disabled", st.name);
    g_phase = Phase::Failed;
    return {};
  }
  st.renderingSlot = index;
  return s.texture;
}

// End Dawn access after the frame's submit and hand the slot to the XR thread.
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
  s.initialized = true;
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
  s.forBridge = std::move(release);
  if (views != nullptr)
    s.views = *views;
  s.readySeq = ++st.readySeq;
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

// Game units -> meters in the starting head space (AURORA_XR_ARENA_SCALE,
// default 0.006: Final Destination's ~170-unit stage is about 1 m wide).
Mat4 arena_transform() {
  const float s = env_float("AURORA_XR_ARENA_SCALE", 0.006f);
  const XrVector3f p = arena_position();
  return {s, 0.f, 0.f, p.x, 0.f, s, 0.f, p.y, 0.f, 0.f, s, p.z, 0.f, 0.f, 0.f, 1.f};
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
  std::chrono::steady_clock::time_point lastLog = std::chrono::steady_clock::now();
  std::array<wgpu::PassTimestampWrites, kZoneCount> writes{};
};

struct Renderer3D {
  uint64_t layoutKey = 0;
  uint32_t sampleCount = 0;
  std::array<ReplayTarget, 3> targets; // eye 0, eye 1, HUD
  std::array<wgpu::Buffer, 2> eyeUniforms;
  std::array<wgpu::BindGroup, 2> eyeGroups;
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
          Log.info("GPU 3D passes per frame (us): {} (total {:.0f})", line, total);
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
    R.eyeUniforms[i] = make_buffer(gx::XrEyeUniformSize, wgpu::BufferUsage::Uniform, "XR eye matrix");
    const wgpu::BindGroupEntry e{.binding = 0, .buffer = R.eyeUniforms[i], .size = gx::XrEyeUniformSize};
    const wgpu::BindGroupDescriptor bg{.layout = gx::g_xrEyeBindGroupLayout, .entryCount = 1, .entries = &e};
    R.eyeGroups[i] = device.CreateBindGroup(&bg);
    R.targets[i].target.views[0].xrBindGroup = R.eyeGroups[i];
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
  const wgpu::Extent3D size{stereo.width, stereo.height, 1};
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
    R.directExtraViews[i] = tex.CreateView();
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
    const wgpu::RenderPipelineDescriptor rpd{
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
void render_3d(const wgpu::CommandEncoder& cmd, gfx::detail::FramePacket& frame) {
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

  const Mat4 cameraToArena = mul(arena_transform(), inverse_affine(frame.xrWorldView));
  for (int eye = 0; eye < 2; ++eye) {
    struct {
      Mat4 m;
      uint32_t enabled[4];
    } u{mul(mul(projection(views[eye].fov, 0.05f), view_from_pose(views[eye].pose)), cameraToArena), {1, 0, 0, 0}};
    static_assert(sizeof(u) == gx::XrEyeUniformSize);
    webgpu::g_queue.WriteBuffer(R.eyeUniforms[eye], 0, &u, sizeof(u));
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
  if (!direct && stereoFormat != wgpu::TextureFormat::RGBA8Unorm) {
    return; // fallback compose writes RGBA8 only (MSAA switched on mid-session)
  }
  if (direct) {
    if (!ensure_direct(layout))
      return;
    if (auto dst = acquire_slot(stereo)) {
      gfx::XrReplayTarget t;
      t.layout = layout;
      t.size = {stereo.width, stereo.height, 1};
      const auto dstView = dst.CreateView();
      t.colorViews = R.directExtraViews;
      t.colorViews[gfx::SceneColorAttachmentIndex] = dstView;
      t.depthView = R.directDepth.CreateView();
      t.clearColor = {0, 0, 0, 0};
      t.clearDepth = gx::UseReversedZ ? 0.f : 1.f;
      t.depthStore = wgpu::StoreOp::Discard;
      const float eyeW = static_cast<float>(stereo.width / 2), eyeH = static_cast<float>(stereo.height);
      t.views[0] = {R.eyeGroups[0], 0.f, 0.f, eyeW, eyeH};
      t.views[1] = {R.eyeGroups[1], eyeW, 0.f, eyeW, eyeH};
      t.viewCount = 2;
      t.finish = &draw_coverage;
      t.timestampWrites = zone_writes(kZone3D);
      gfx::encode_xr_replay(cmd, frame, gfx::XrCategory::World, t);
      R.renderedViews = views;
      R.renderedStereo = true;
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

void add_required_features(const wgpu::Adapter& adapter, std::vector<wgpu::FeatureName>& features) noexcept {
  if (!wanted())
    return;
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
    g_phase = Phase::Starting;
    g_thread = std::thread(thread_main);
    return {};
  }
  if (phase == Phase::SlotsReady) {
    if (!import_slots_into_dawn()) {
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
  return acquire_slot(screen);
}

void end_frame() noexcept {
  g_skipPresent = false;
  map_timing();
  release_slot(g_streams[kScreen], nullptr);
  release_slot(g_streams[kStereo], R.renderedStereo ? &R.renderedViews : nullptr);
  R.renderedStereo = false;
  release_slot(g_streams[kHud], nullptr);
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
