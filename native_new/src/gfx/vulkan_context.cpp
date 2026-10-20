#include "gfx/vulkan_context.h"

#include <stdexcept>
#include <string>
#include <vector>

#include "common/log.h"
#include "common/xr_result.h"

namespace xrift::gfx {
namespace {

void CheckVk(VkResult result, const char* what) {
  if (result == VK_SUCCESS) {
    return;
  }
  // volk 経由の呼び出しは XrResult ではないため、数値付きの例外にする。
  throw std::runtime_error(std::string(what) + " に失敗 (VkResult=" +
                           std::to_string(static_cast<int>(result)) + ")");
}

// ランタイムが返す拡張名一覧を string の配列に分解する。
// 区切りは NUL と空白のどちらも受け付ける。仕様上は空白区切りであり、
// SteamVR は 1 回の呼び出しで空白区切りの複数名を返すことを実測で確認した。
std::vector<std::string> SplitExtensionList(const char* data, size_t size) {
  std::vector<std::string> out;
  std::string current;
  for (size_t i = 0; i < size; ++i) {
    const char c = data[i];
    if (c == '\0' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      if (!current.empty()) {
        out.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) {
    out.push_back(current);
  }
  return out;
}

// 拡張関数 (2 コール式) の文字列取得を共通化する。第 1 引数で件数取得と本文取得を切り替える。
template <typename GetProc>
std::vector<std::string> QueryExtensionNames(GetProc proc, const xr::Instance& xr, const char* what) {
  uint32_t size = 0;
  CheckXr(proc(xr.handle(), xr.systemId(), 0, &size, nullptr), what);
  std::vector<char> buffer(size);
  uint32_t written = 0;
  // size が 0 の場合 data() は有効な空範囲を指すため、そのまま渡してよい。
  CheckXr(proc(xr.handle(), xr.systemId(), size, &written, buffer.data()), what);
  buffer.resize(written);
  return SplitExtensionList(buffer.data(), buffer.size());
}

void LogVersion(const char* label, XrVersion version) {
  Info("%s: %u.%u.%u", label, static_cast<unsigned>(XR_VERSION_MAJOR(version)),
       static_cast<unsigned>(XR_VERSION_MINOR(version)), static_cast<unsigned>(XR_VERSION_PATCH(version)));
}

}  // namespace

VulkanContext::VulkanContext(const xr::Instance& xr) {
  // 1. volk を初期化する(libvulkan の読み込みを含む)。
  CheckVk(volkInitialize(), "volkInitialize");

  // 2. ランタイムの Vulkan 要件を取得する。
  XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
  CheckXr(xr.vulkanRequirementsProc()(xr.handle(), xr.systemId(), &requirements),
          "xrGetVulkanGraphicsRequirementsKHR");
  LogVersion("Vulkan 要件 min", requirements.minApiVersionSupported);
  LogVersion("Vulkan 要件 max", requirements.maxApiVersionSupported);

  // 3. ランタイム指定の VkInstance 拡張を取得する。
  const std::vector<std::string> instance_ext_names =
      QueryExtensionNames(xr.vulkanInstanceExtensionsProc(), xr, "xrGetVulkanInstanceExtensionsKHR");
  std::vector<const char*> instance_exts;
  for (const std::string& name : instance_ext_names) {
    instance_exts.push_back(name.c_str());
  }
  Info("VkInstance 拡張: %u 件", static_cast<unsigned>(instance_ext_names.size()));
  for (const std::string& name : instance_ext_names) {
    Info("  %s", name.c_str());
  }

  // 4. VkInstance を作り、volk に関数群を読み込ませる。
  VkApplicationInfo app_info{};
  app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app_info.pApplicationName = "xrift_new";
  app_info.applicationVersion = 1;
  app_info.pEngineName = "xrift";
  app_info.engineVersion = 1;
  // 要件の最小版を使う。XrVersion と Vk apiVersion は符号化が異なるため組み立て直す。
  // 0 が返る異常系では 1.0 に退避する。
  app_info.apiVersion = requirements.minApiVersionSupported != 0
                            ? VK_MAKE_VERSION(XR_VERSION_MAJOR(requirements.minApiVersionSupported),
                                              XR_VERSION_MINOR(requirements.minApiVersionSupported),
                                              XR_VERSION_PATCH(requirements.minApiVersionSupported))
                            : VK_API_VERSION_1_0;
  VkInstanceCreateInfo instance_info{};
  instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instance_info.pApplicationInfo = &app_info;
  instance_info.enabledExtensionCount = static_cast<uint32_t>(instance_exts.size());
  instance_info.ppEnabledExtensionNames = instance_exts.data();
  CheckVk(vkCreateInstance(&instance_info, nullptr, &instance_), "vkCreateInstance");
  volkLoadInstance(instance_);

  // 5. ランタイムに物理デバイスを選ばせる。
  CheckXr(xr.vulkanGraphicsDeviceProc()(xr.handle(), xr.systemId(), instance_, &physical_device_),
          "xrGetVulkanGraphicsDeviceKHR");

  // 6. デバイス拡張を取得し、VK_KHR_swapchain が無ければ足す。
  std::vector<std::string> device_ext_names =
      QueryExtensionNames(xr.vulkanDeviceExtensionsProc(), xr, "xrGetVulkanDeviceExtensionsKHR");
  bool has_swapchain = false;
  for (const std::string& name : device_ext_names) {
    if (name == VK_KHR_SWAPCHAIN_EXTENSION_NAME) {
      has_swapchain = true;
      break;
    }
  }
  if (!has_swapchain) {
    Warn("ランタイムが %s を返さなかったため追加する", VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    device_ext_names.emplace_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  }
  std::vector<const char*> device_exts;
  for (const std::string& name : device_ext_names) {
    device_exts.push_back(name.c_str());
  }
  Info("VkDevice 拡張: %u 件", static_cast<unsigned>(device_ext_names.size()));

  // 7. グラフィックス対応の queue family を選び、空の feature で VkDevice を作る。
  uint32_t family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &family_count, nullptr);
  std::vector<VkQueueFamilyProperties> families(family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &family_count, families.data());
  uint32_t graphics_family = UINT32_MAX;
  for (uint32_t i = 0; i < family_count; ++i) {
    if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
      graphics_family = i;
      break;
    }
  }
  if (graphics_family == UINT32_MAX) {
    throw std::runtime_error("graphics 対応の queue family がありません");
  }
  queue_family_index_ = graphics_family;

  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info{};
  queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queue_info.queueFamilyIndex = graphics_family;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;
  const VkPhysicalDeviceFeatures features{};
  VkDeviceCreateInfo device_info{};
  device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  device_info.enabledExtensionCount = static_cast<uint32_t>(device_exts.size());
  device_info.ppEnabledExtensionNames = device_exts.data();
  device_info.pEnabledFeatures = &features;
  CheckVk(vkCreateDevice(physical_device_, &device_info, nullptr, &device_), "vkCreateDevice");
  volkLoadDevice(device_);
  vkGetDeviceQueue(device_, graphics_family, 0, &queue_);

  // 8. GPU 名を保持する。
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(physical_device_, &props);
  gpu_name_ = props.deviceName;
  Info("GPU: %s (queue family %u)", gpu_name_.c_str(), static_cast<unsigned>(graphics_family));
}

VulkanContext::~VulkanContext() {
  // 生成の逆順に破棄する。
  if (device_ != VK_NULL_HANDLE) {
    vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
  }
  if (instance_ != VK_NULL_HANDLE) {
    vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
  }
}

}  // namespace xrift::gfx
