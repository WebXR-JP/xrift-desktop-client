#include "gfx/vulkan_context.h"

// 動的ディスパッチャの既定実体の定義。全 TU で 1 回のみ。
// 本 exe は vk::raii::Context 起点のディスパッチャだけを使い、既定ディスパッチャ経由の呼び出しは行わない。
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

#include <stdexcept>
#include <string>
#include <vector>

#include "common/log.h"
#include "common/xr_result.h"

namespace xrift::gfx {
namespace {

// vk::Result を返す呼び出しの検査。成功以外は vk::to_string 付きの例外にする。
void CheckVk(vk::Result result, const char* what) {
  if (result == vk::Result::eSuccess) {
    return;
  }
  throw std::runtime_error(std::string(what) + " に失敗 (VkResult=" + vk::to_string(result) + ")");
}

// 例外 (vk::SystemError) を投げる raii 呼び出しを、従来の CheckVk と同じ粒度に揃える。
// 型は std::runtime_error、文面は "<what> に失敗 (VkResult=<名前>)" とする。
template <typename F>
void InvokeVk(const char* what, F&& f) {
  try {
    f();
  } catch (const vk::SystemError& e) {
    const vk::Result result = static_cast<vk::Result>(e.code().value());
    throw std::runtime_error(std::string(what) + " に失敗 (VkResult=" + vk::to_string(result) +
                             ")");
  }
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

VulkanContext::VulkanContext(const xr::Instance& xr,
                             const std::vector<std::string>& extraDeviceExtensions) {
  // 1. vk::raii::Context の生成時に読み込みが行われる (vulkan-1.dll の動的読み込みを含む)。
  //    明示の初期化呼び出しは不要である。

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

  // 4. VkInstance を作る。
  vk::ApplicationInfo app_info{};
  app_info.pApplicationName = "xrift_new";
  app_info.applicationVersion = 1;
  app_info.pEngineName = "xrift";
  app_info.engineVersion = 1;
  // 要件の最小版を使う。XrVersion と Vk apiVersion は符号化が異なるため組み立て直す。
  // 0 が返る異常系では 1.0 に退避する。
  app_info.apiVersion = requirements.minApiVersionSupported != 0
                            ? VK_MAKE_API_VERSION(0, XR_VERSION_MAJOR(requirements.minApiVersionSupported),
                                                  XR_VERSION_MINOR(requirements.minApiVersionSupported),
                                                  XR_VERSION_PATCH(requirements.minApiVersionSupported))
                            : VK_API_VERSION_1_0;
  vk::InstanceCreateInfo instance_info{};
  instance_info.sType = vk::StructureType::eInstanceCreateInfo;
  instance_info.pApplicationInfo = &app_info;
  instance_info.enabledExtensionCount = static_cast<uint32_t>(instance_exts.size());
  instance_info.ppEnabledExtensionNames = instance_exts.data();
  InvokeVk("vkCreateInstance", [&] { instance_ = vk::raii::Instance(context_, instance_info); });

  // 5. ランタイムに物理デバイスを選ばせる。境界は生ハンドルで行い、所有は raii に巻き直す。
  VkPhysicalDevice chosen = VK_NULL_HANDLE;
  CheckXr(xr.vulkanGraphicsDeviceProc()(xr.handle(), xr.systemId(),
                                        static_cast<VkInstance>(*instance_), &chosen),
          "xrGetVulkanGraphicsDeviceKHR");
  physical_device_ = vk::raii::PhysicalDevice(instance_, chosen);

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
  // 呼び出し側の追加分（例: 転送用の外部メモリ系）を重複なく足す。
  // 有無の検査は呼び出し側で行う。ここでは有効化だけを担う。
  for (const std::string& extra : extraDeviceExtensions) {
    bool known = false;
    for (const std::string& name : device_ext_names) {
      if (name == extra) {
        known = true;
        break;
      }
    }
    if (!known) {
      device_ext_names.push_back(extra);
    }
  }
  // 追加分が物理デバイスに実在することを vkCreateDevice の前に確認する。
  // 存在しない名前を渡すと ErrorExtensionNotPresent になるだけであり、
  // どれが無いのか分からなくなるため、ここで名指しして落とす。
  if (!extraDeviceExtensions.empty()) {
    const std::vector<vk::ExtensionProperties> available =
        physical_device_.enumerateDeviceExtensionProperties();
    Info("デバイス拡張の列挙: %u 件", static_cast<unsigned>(available.size()));
    std::string missing;
    for (const std::string& extra : extraDeviceExtensions) {
      bool found = false;
      for (const vk::ExtensionProperties& prop : available) {
        if (std::string(prop.extensionName.data()) == extra) {
          found = true;
          break;
        }
      }
      if (!found) {
        if (!missing.empty()) {
          missing += ", ";
        }
        missing += extra;
      }
    }
    if (!missing.empty()) {
      throw std::runtime_error("要求されたデバイス拡張が物理デバイスにありません: " + missing);
    }
  }
  std::vector<const char*> device_exts;
  for (const std::string& name : device_ext_names) {
    device_exts.push_back(name.c_str());
  }
  Info("VkDevice 拡張: %u 件", static_cast<unsigned>(device_ext_names.size()));

  // 7. グラフィックス対応の queue family を選び、空の feature で VkDevice を作る。
  const std::vector<vk::QueueFamilyProperties> families =
      physical_device_.getQueueFamilyProperties();
  uint32_t graphics_family = UINT32_MAX;
  for (uint32_t i = 0; i < families.size(); ++i) {
    if ((families[i].queueFlags & vk::QueueFlagBits::eGraphics) != vk::QueueFlags{}) {
      graphics_family = i;
      break;
    }
  }
  if (graphics_family == UINT32_MAX) {
    throw std::runtime_error("graphics 対応の queue family がありません");
  }
  queue_family_index_ = graphics_family;

  const float priority = 1.0f;
  vk::DeviceQueueCreateInfo queue_info{};
  queue_info.sType = vk::StructureType::eDeviceQueueCreateInfo;
  queue_info.queueFamilyIndex = graphics_family;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;
  const vk::PhysicalDeviceFeatures features{};
  vk::DeviceCreateInfo device_info{};
  device_info.sType = vk::StructureType::eDeviceCreateInfo;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  device_info.enabledExtensionCount = static_cast<uint32_t>(device_exts.size());
  device_info.ppEnabledExtensionNames = device_exts.data();
  device_info.pEnabledFeatures = &features;
  InvokeVk("vkCreateDevice", [&] { device_ = vk::raii::Device(physical_device_, device_info); });
  InvokeVk("vkGetDeviceQueue", [&] { queue_ = device_.getQueue(graphics_family, 0); });

  // 8. GPU 名を保持する。
  const vk::PhysicalDeviceProperties props = physical_device_.getProperties();
  gpu_name_ = props.deviceName.data();
  Info("GPU: %s (queue family %u)", gpu_name_.c_str(), static_cast<unsigned>(graphics_family));
}

VulkanContext::~VulkanContext() {
  // 破棄はメンバの逆順 (queue_ → device_ → physical_device_ → instance_ → context_) に自動で行われる。
  // queue_ と physical_device_ は非所有のため実処理はなく、device_ → instance_ の順が保たれる。
}

}  // namespace xrift::gfx
