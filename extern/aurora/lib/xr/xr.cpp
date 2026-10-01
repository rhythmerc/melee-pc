#include "xr.hpp"

#include <aurora/xr.h>

#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
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
};

enum StreamId : int { kScreen = 0, kStereo = 1, kHud = 2, kStreamCount = 3 };

struct Stream {
  const char* name = "";
  std::array<Slot, kSlotCount> slots;
  uint32_t width = 0, height = 0; // set before the XR thread starts
  uint64_t readySeq = 0;          // g_mutex
  int renderingSlot = -1;         // render worker

  // XR thread only.
  XrSwapchain swapchain = XR_NULL_HANDLE;
  std::vector<VkImage> swapImages;
  bool haveImage = false;
  std::chrono::steady_clock::time_point lastCopy{};
  uint64_t copies = 0;
  std::array<XrView, 2> shownViews{}; // Stereo: poses of the image in the swapchain
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

// Latest predicted eye poses, from the XR thread for the render worker.
std::mutex g_viewMutex;
std::array<XrView, 2> g_latestViews{};
bool g_viewsValid = false;

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
  bool focused = false;

  uint64_t framesShown = 0, fightFrames = 0;
  std::array<uint64_t, kStreamCount> copiedSinceStats{};
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
VkImageCreateInfo shared_image_info(VkExternalMemoryImageCreateInfo& ext, uint32_t width, uint32_t height) {
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
  ici.format = VK_FORMAT_R8G8B8A8_UNORM;
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
  }
  if (!hasVk2) {
    Log.error("OpenXR runtime lacks XR_KHR_vulkan_enable2");
    return false;
  }
  std::vector<const char*> exts{XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
  if (B.hasPassthroughExt)
    exts.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
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
  Log.info("Bridge VkDevice: {} [{:04x}:{:04x}]", pp.deviceName, pp.vendorID, pp.deviceID);
  return true;
}

bool create_slot(Slot& s, uint32_t width, uint32_t height) {
  VkExternalMemoryImageCreateInfo ext;
  VkImageCreateInfo ici = shared_image_info(ext, width, height);
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
  ci.format = B.swapFormat;
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
// AURORA_XR_EYE_SCALE. 0.7 by default: on a Quest 3 the full 1680x1760 per
// eye took 21-27 ms of GPU per frame (about 35 fps); 0.7 (1176x1232) holds
// 72 Hz at about 10.5 ms.
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
  const float scale = std::clamp(env_float("AURORA_XR_EYE_SCALE", 0.7f), 0.25f, 2.f);
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
    if (!create_swapchain(st))
      return false;

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
  Log.info("Swapchains: screen {}x{}, 3D {}x{} (two {}x{} eyes), HUD {}x{} (format {}), passthrough {}",
           g_streams[kScreen].width, g_streams[kScreen].height, g_streams[kStereo].width, g_streams[kStereo].height,
           g_streams[kStereo].width / 2, g_streams[kStereo].height, g_streams[kHud].width, g_streams[kHud].height,
           B.swapFormat, B.passthroughLayer ? "on" : "off");
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
  std::array<VkImageMemoryBarrier, 3> pre{};
  uint32_t npre = 0;
  // Acquire from Dawn: must mirror Dawn's release barrier exactly.
  pre[npre++] = barrier(s.image, dawnRelease.oldLayout, dawnRelease.newLayout, 0, VK_ACCESS_TRANSFER_READ_BIT,
                        kExternalQueueFamily, B.queueFamily);
  if (dawnRelease.newLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
    pre[npre++] = barrier(s.image, dawnRelease.newLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  // OpenXR hands Vulkan swapchain images over in COLOR_ATTACHMENT_OPTIMAL.
  pre[npre++] = barrier(swapImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, npre, pre.data());
  VkImageCopy region{};
  region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.extent = {st.width, st.height, 1};
  vkCmdCopyImage(s.cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 1, &region);
  if (!B.dumpDir.empty() && !st.dumped && st.dumpSlot < 0 && st.copies >= 300 &&
      (st.dumpBuf || create_dump_buffer(st))) {
    VkBufferImageCopy rb{};
    rb.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rb.imageExtent = {st.width, st.height, 1};
    vkCmdCopyImageToBuffer(s.cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, st.dumpBuf, 1, &rb);
    st.dumpSlot = index;
  }
  std::array<VkImageMemoryBarrier, 2> post{
      barrier(swapImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT),
      // Release back to Dawn, which acquires with the same layouts.
      barrier(s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_ACCESS_TRANSFER_READ_BIT, 0, B.queueFamily, kExternalQueueFamily),
  };
  vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                       nullptr, static_cast<uint32_t>(post.size()), post.data());
  VK_TRY(vkEndCommandBuffer(s.cmd));

  VkExportSemaphoreCreateInfo esci{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
  esci.handleTypes = kSemHandle;
  VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  sci.pNext = &esci;
  VK_TRY(vkCreateSemaphore(B.dev, &sci, nullptr, &s.signalSem));
  std::vector<VkPipelineStageFlags> stages(s.waitSems.size(), VK_PIPELINE_STAGE_TRANSFER_BIT);
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
    s.forDawn.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
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
    } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
      return false;
    }
    ev = {XR_TYPE_EVENT_DATA_BUFFER};
  }
  return true;
}

bool setup() {
  if (!create_instance() || !create_bridge_device() || !size_stereo_stream())
    return false;
  for (auto& st : g_streams)
    for (auto& s : st.slots)
      if (!create_slot(s, st.width, st.height))
        return false;
  if (!create_session())
    return false;
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
      VkImageCreateInfo ici = shared_image_info(ext, st.width, st.height);
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
};
Renderer3D R;

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
    R.targets[i].target.xrBindGroup = R.eyeGroups[i];
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
  R.layoutKey = layout.key;
  R.sampleCount = layout.sampleCount;
  Log.info("3D fight targets ready: eyes {}x{}, HUD {}x{}, {}x MSAA", eyeW, eyeH, g_streams[kHud].width,
           g_streams[kHud].height, layout.sampleCount);
  return true;
}

void compose(const wgpu::CommandEncoder& cmd, const wgpu::Texture& dst, std::initializer_list<int> sources) {
  const auto view = dst.CreateView();
  const wgpu::RenderPassColorAttachment ca{
      .view = view,
      .loadOp = wgpu::LoadOp::Clear,
      .storeOp = wgpu::StoreOp::Store,
      .clearValue = {0, 0, 0, 0},
  };
  const wgpu::RenderPassDescriptor rpd{.label = "XR compose", .colorAttachmentCount = 1, .colorAttachments = &ca};
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
    gfx::encode_xr_replay(cmd, frame, gfx::XrCategory::World, R.targets[eye].target);
  }
  if (auto dst = acquire_slot(g_streams[kStereo])) {
    compose(cmd, dst, {0, 1});
    R.renderedViews = views;
    R.renderedStereo = true;
  }
  if (frame.xrHasHud) {
    gfx::encode_xr_replay(cmd, frame, gfx::XrCategory::Hud, R.targets[2].target);
    if (auto dst = acquire_slot(g_streams[kHud])) {
      compose(cmd, dst, {2});
    }
  }
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
}

bool active() noexcept { return g_phase == Phase::Imported && g_sessionRunning; }

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
    g_streams[kScreen].width = g_streams[kHud].width = width;
    g_streams[kScreen].height = g_streams[kHud].height = height;
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
