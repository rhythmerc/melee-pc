#include "xr.hpp"

#include <aurora/xr.h>

#include "../internal.hpp"
#include "../webgpu/gpu.hpp"

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

constexpr int kSlotCount = 3;

enum class SlotState {
  Free,      // render worker may take it
  Rendering, // render worker is drawing into it
  Ready,     // finished frame waiting for the XR thread
  Copying,   // XR thread is copying it into the swapchain
};

struct PendingAccess {
  // What Dawn's next BeginAccess on this slot must wait on and acquire with.
  // Set by whichever side released the image last.
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
  PendingAccess forDawn;     // next Dawn BeginAccess
  PendingAccess forBridge;   // Dawn's release, for the XR thread's acquire

  // XR thread only.
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool inFlight = false;
  std::vector<VkSemaphore> waitSems;
  VkSemaphore signalSem = VK_NULL_HANDLE;
  VkSemaphore prevSignalSem = VK_NULL_HANDLE;
};

enum class Phase { Idle, Starting, SlotsReady, Imported, Failed };

std::mutex g_mutex;
std::atomic<Phase> g_phase{Phase::Idle};
std::atomic<bool> g_stop{false};
std::atomic<bool> g_sessionRunning{false};
std::thread g_thread;
std::array<Slot, kSlotCount> g_slots;
uint32_t g_width = 0, g_height = 0;
uint64_t g_readySeq = 0;
int g_renderingSlot = -1;
uint32_t g_bridgeVendor = 0, g_bridgeDevice = 0;

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
  XrSwapchain swapchain = XR_NULL_HANDLE;
  std::vector<VkImage> swapImages;
  int64_t swapFormat = 0;
  bool hasPassthroughExt = false;
  XrPassthroughFB passthrough = XR_NULL_HANDLE;
  XrPassthroughLayerFB passthroughLayer = XR_NULL_HANDLE;
  bool running = false;
  bool haveImage = false;

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

  // Debug readback (AURORA_XR_DEBUG=1)
  bool debug = false;
  VkBuffer readbackBuf = VK_NULL_HANDLE;
  VkDeviceMemory readbackMem = VK_NULL_HANDLE;
  void* readbackPtr = nullptr;
  int readbackSlot = -1;

  // Controller input
  XrActionSet actionSet = XR_NULL_HANDLE;
  XrAction stickMain = XR_NULL_HANDLE, stickC = XR_NULL_HANDLE;
  XrAction btnA = XR_NULL_HANDLE, btnB = XR_NULL_HANDLE, btnX = XR_NULL_HANDLE, btnY = XR_NULL_HANDLE;
  XrAction btnZ = XR_NULL_HANDLE, btnStart = XR_NULL_HANDLE;
  XrAction trigL = XR_NULL_HANDLE, trigR = XR_NULL_HANDLE;
  bool focused = false;

  uint64_t framesShown = 0, framesCopied = 0;
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
VkImageCreateInfo shared_image_info(VkExternalMemoryImageCreateInfo& ext) {
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
  ici.extent = {g_width, g_height, 1};
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

bool create_slot(Slot& s) {
  VkExternalMemoryImageCreateInfo ext;
  VkImageCreateInfo ici = shared_image_info(ext);
#ifdef __ANDROID__
  AHardwareBuffer_Desc ad{};
  ad.width = g_width;
  ad.height = g_height;
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

  // The game's frame is already sRGB-encoded bytes in an RGBA8Unorm texture.
  // Copying those bytes unchanged into an *_SRGB swapchain makes the runtime
  // decode them correctly; a blit would re-encode them and wash the image out.
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
  XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                  XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
  ci.format = B.swapFormat;
  ci.sampleCount = 1;
  ci.width = g_width;
  ci.height = g_height;
  ci.faceCount = 1;
  ci.arraySize = 1;
  ci.mipCount = 1;
  XR_TRY(xrCreateSwapchain(B.session, &ci, &B.swapchain));
  XR_TRY(xrEnumerateSwapchainImages(B.swapchain, 0, &n, nullptr));
  std::vector<XrSwapchainImageVulkan2KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
  XR_TRY(xrEnumerateSwapchainImages(B.swapchain, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data())));
  for (const auto& im : imgs)
    B.swapImages.push_back(im.image);

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
  Log.info("Virtual screen swapchain {}x{} (format {}), passthrough {}", g_width, g_height, B.swapFormat,
           B.passthroughLayer ? "on" : "off");
  return true;
}

bool create_debug_readback() {
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = 4;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VK_TRY(vkCreateBuffer(B.dev, &bci, nullptr, &B.readbackBuf));
  VkMemoryRequirements mr;
  vkGetBufferMemoryRequirements(B.dev, B.readbackBuf, &mr);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex =
      find_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VK_TRY(vkAllocateMemory(B.dev, &mai, nullptr, &B.readbackMem));
  VK_TRY(vkBindBufferMemory(B.dev, B.readbackBuf, B.readbackMem, 0));
  VK_TRY(vkMapMemory(B.dev, B.readbackMem, 0, 4, 0, &B.readbackPtr));
  return true;
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
void retire_slot(Slot& s, int index) {
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
  if (B.readbackSlot == index) {
    B.readbackSlot = -1;
    const auto* p = static_cast<const uint8_t*>(B.readbackPtr);
    Log.info("debug: frame {} centre pixel rgba({}, {}, {}, {})", B.framesCopied, p[0], p[1], p[2], p[3]);
  }
}

// Copy the newest finished frame into the swapchain. Returns false on a
// fatal error.
bool copy_latest_frame() {
  int index = -1;
  PendingAccess dawnRelease;
  {
    std::lock_guard lock{g_mutex};
    uint64_t best = 0;
    for (int i = 0; i < kSlotCount; ++i) {
      if (g_slots[i].state == SlotState::Ready && g_slots[i].readySeq > best) {
        best = g_slots[i].readySeq;
        index = i;
      }
    }
    if (index < 0)
      return true;
    g_slots[index].state = SlotState::Copying;
    dawnRelease = std::move(g_slots[index].forBridge);
    g_slots[index].forBridge = {};
  }
  Slot& s = g_slots[index];
  retire_slot(s, index);

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
  XR_TRY(xrAcquireSwapchainImage(B.swapchain, nullptr, &swapIndex));
  XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wi.timeout = XR_INFINITE_DURATION;
  XR_TRY(xrWaitSwapchainImage(B.swapchain, &wi));
  VkImage swapImg = B.swapImages[swapIndex];

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
  region.extent = {g_width, g_height, 1};
  vkCmdCopyImage(s.cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 1, &region);
  if (B.debug && B.readbackSlot < 0 && B.framesCopied % 300 == 0) {
    VkBufferImageCopy rb{};
    rb.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rb.imageOffset = {static_cast<int32_t>(g_width / 2), static_cast<int32_t>(g_height / 2), 0};
    rb.imageExtent = {1, 1, 1};
    vkCmdCopyImageToBuffer(s.cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, B.readbackBuf, 1, &rb);
    B.readbackSlot = index;
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
  XR_TRY(xrReleaseSwapchainImage(B.swapchain, nullptr));

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
  B.haveImage = true;
  ++B.framesCopied;
  return true;
}

bool render_xr_frame() {
  XrFrameState fs{XR_TYPE_FRAME_STATE};
  XR_TRY(xrWaitFrame(B.session, nullptr, &fs));
  update_input();
  XR_TRY(xrBeginFrame(B.session, nullptr));
  if (fs.shouldRender && !copy_latest_frame())
    return false;

  std::array<const XrCompositionLayerBaseHeader*, 2> layers{};
  uint32_t layerCount = 0;
  XrCompositionLayerPassthroughFB ptLayer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
  if (B.passthroughLayer) {
    ptLayer.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    ptLayer.layerHandle = B.passthroughLayer;
    layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&ptLayer);
  }
  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  if (fs.shouldRender && B.haveImage) {
    // A swapchain with no new release shows its last released image, so the
    // screen keeps its picture on display frames the game did not produce.
    const float width = env_float("AURORA_XR_SCREEN_WIDTH", 1.6f);
    quad.space = B.space;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = B.swapchain;
    quad.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(g_width), static_cast<int32_t>(g_height)}};
    quad.pose.orientation.w = 1.f;
    quad.pose.position = {0.f, env_float("AURORA_XR_SCREEN_Y", 0.f), -env_float("AURORA_XR_SCREEN_DISTANCE", 1.5f)};
    quad.size = {width, width * static_cast<float>(g_height) / static_cast<float>(g_width)};
    layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
  }
  XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
  fei.displayTime = fs.predictedDisplayTime;
  fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  fei.layerCount = layerCount;
  fei.layers = layers.data();
  XR_TRY(xrEndFrame(B.session, &fei));
  ++B.framesShown;

  const auto now = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(now - B.statsStart).count();
  if (secs >= 10.0) {
    Log.info("{:.1f} display fps, {:.1f} game frames/s copied to the virtual screen", B.framesShown / secs,
             B.framesCopied / secs);
    B.framesShown = 0;
    B.framesCopied = 0;
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
  if (!create_instance() || !create_bridge_device())
    return false;
  for (auto& s : g_slots)
    if (!create_slot(s))
      return false;
  if (!create_session())
    return false;
  if (!create_input())
    Log.warn("Controller input unavailable");
  B.debug = env_flag("AURORA_XR_DEBUG", false);
  if (B.debug && !create_debug_readback())
    B.debug = false;
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
  if (B.passthroughLayer && B.destroyPassthroughLayer)
    B.destroyPassthroughLayer(B.passthroughLayer);
  if (B.passthrough && B.destroyPassthrough)
    B.destroyPassthrough(B.passthrough);
  if (B.actionSet)
    xrDestroyActionSet(B.actionSet); // destroys its actions too
  B.actionSet = XR_NULL_HANDLE;
  {
    std::lock_guard lock{g_padMutex};
    g_padValid = false;
  }
  if (B.swapchain)
    xrDestroySwapchain(B.swapchain);
  if (B.space)
    xrDestroySpace(B.space);
  if (B.session)
    xrDestroySession(B.session);
  B.passthroughLayer = XR_NULL_HANDLE;
  B.passthrough = XR_NULL_HANDLE;
  B.swapchain = XR_NULL_HANDLE;
  B.space = XR_NULL_HANDLE;
  B.session = XR_NULL_HANDLE;
  if (B.dev) {
    // Dawn holds its own imports of the shared images, so releasing the
    // bridge's copies does not affect frames it is still drawing.
    for (auto& s : g_slots) {
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
    if (B.readbackBuf)
      vkDestroyBuffer(B.dev, B.readbackBuf, nullptr);
    if (B.readbackMem)
      vkFreeMemory(B.dev, B.readbackMem, nullptr);
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

// ---------------------------------------------------------------- render worker helpers

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
  for (int i = 0; i < kSlotCount; ++i) {
    Slot& s = g_slots[i];
    wgpu::SharedTextureMemoryDescriptor sd{};
    sd.label = "XR virtual screen";
#ifdef __ANDROID__
    wgpu::SharedTextureMemoryAHardwareBufferDescriptor ahbd{};
    ahbd.handle = s.ahb;
    sd.nextInChain = &ahbd;
    s.stm = device.ImportSharedTextureMemory(&sd);
#else
    VkExternalMemoryImageCreateInfo ext;
    VkImageCreateInfo ici = shared_image_info(ext);
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
      Log.error("ImportSharedTextureMemory failed");
      return false;
    }
    s.texture = s.stm.CreateTexture();
    if (!s.texture) {
      Log.error("SharedTextureMemory::CreateTexture failed");
      return false;
    }
  }
  Log.info("Virtual screen: {} shared {}x{} images imported into Dawn", kSlotCount, g_width, g_height);
  return true;
}

// Pick a slot for the next frame (render worker, g_mutex held).
int take_slot_locked() {
  for (int i = 0; i < kSlotCount; ++i)
    if (g_slots[i].state == SlotState::Free)
      return i;
  // No free slot: reclaim the oldest finished frame the XR thread skipped.
  // Nothing on the bridge side touched it, so Dawn waits on its own release.
  int oldest = -1;
  for (int i = 0; i < kSlotCount; ++i)
    if (g_slots[i].state == SlotState::Ready && (oldest < 0 || g_slots[i].readySeq < g_slots[oldest].readySeq))
      oldest = i;
  if (oldest >= 0) {
    Slot& s = g_slots[oldest];
    s.forDawn = std::move(s.forBridge);
    s.forBridge = {};
  }
  return oldest;
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
    g_width = width;
    g_height = height;
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
    g_phase = Phase::Imported;
    phase = Phase::Imported;
  }
  if (phase != Phase::Imported)
    return {};
  if (width != g_width || height != g_height) {
    static bool warned = false;
    if (!warned) {
      Log.warn("Frame size changed to {}x{}; the virtual screen stays {}x{} and the window takes over", width, height,
               g_width, g_height);
      warned = true;
    }
    return {};
  }

  int index;
  PendingAccess access;
  {
    std::lock_guard lock{g_mutex};
    index = take_slot_locked();
    if (index < 0)
      return {};
    g_slots[index].state = SlotState::Rendering;
    access = std::move(g_slots[index].forDawn);
    g_slots[index].forDawn = {};
  }
  Slot& s = g_slots[index];
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
    Log.error("BeginAccess failed; virtual screen disabled");
    g_phase = Phase::Failed;
    return {};
  }
  g_renderingSlot = index;
  return s.texture;
}

void end_frame() noexcept {
  if (g_renderingSlot < 0)
    return;
  Slot& s = g_slots[g_renderingSlot];
  wgpu::SharedTextureMemoryVkImageLayoutEndState es{};
  wgpu::SharedTextureMemoryEndAccessState st{};
  st.nextInChain = &es;
  if (s.stm.EndAccess(s.texture, &st) != wgpu::Status::Success) {
    Log.error("EndAccess failed; virtual screen disabled");
    g_phase = Phase::Failed;
    g_renderingSlot = -1;
    return;
  }
  s.initialized = true;
  PendingAccess release;
  release.oldLayout = static_cast<VkImageLayout>(es.oldLayout);
  release.newLayout = static_cast<VkImageLayout>(es.newLayout);
  for (size_t i = 0; i < st.fenceCount; ++i) {
    DawnFenceExportInfo oi{};
    wgpu::SharedFenceExportInfo ei{};
    ei.nextInChain = &oi;
    st.fences[i].ExportInfo(&ei);
    if (ei.type != kDawnFenceType) {
      Log.error("Unexpected Dawn fence type {}", static_cast<int>(ei.type));
      continue;
    }
    if (oi.handle >= 0)
      release.fds.push_back(dup(oi.handle)); // the SharedFence keeps its own handle
  }
  {
    std::lock_guard lock{g_mutex};
    s.forBridge = std::move(release);
    s.readySeq = ++g_readySeq;
    s.state = SlotState::Ready;
  }
  g_renderingSlot = -1;
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
