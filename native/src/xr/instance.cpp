#include "xr/instance.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include "common/log.h"
#include "common/xr_result.h"

namespace xrift::xr {
namespace {

// 必須のグラフィックス有効化拡張。
constexpr const char* kVulkanEnableExtension = XR_KHR_VULKAN_ENABLE_EXTENSION_NAME;
// M4 の local-floor 参照空間（XR_EXT_local_floor）の検出・有効化対象。
constexpr const char* kLocalFloorExtension = XR_EXT_LOCAL_FLOOR_EXTENSION_NAME;

bool HasExtension(const XrExtensionProperties* list, uint32_t count, const char* name) {
  for (uint32_t i = 0; i < count; ++i) {
    if (std::strcmp(list[i].extensionName, name) == 0) {
      return true;
    }
  }
  return false;
}

// xrGetInstanceProcAddr で拡張関数を 1 本取得する。取得失敗は例外にする。
PFN_xrVoidFunction LoadProc(XrInstance instance, const char* name) {
  PFN_xrVoidFunction fn = nullptr;
  CheckXr(xrGetInstanceProcAddr(instance, name, &fn), name);
  if (fn == nullptr) {
    throw std::runtime_error(std::string(name) + " の関数ポインタが null です");
  }
  return fn;
}

}  // namespace

Instance::Instance() {
  // コンストラクタが例外で抜けるとデストラクタは呼ばれないため、生成済みインスタンスは
  // ここで破棄する。Destroy() は冪等であり、部分構築状態でも安全に呼べる。
  try {
    // 1. インスタンス拡張を列挙し、XR_KHR_vulkan_enable の有無を確認する。
    uint32_t ext_count = 0;
    CheckXr(xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr),
            "xrEnumerateInstanceExtensionProperties(件数)");
    std::vector<XrExtensionProperties> extensions(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
    CheckXr(xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, extensions.data()),
            "xrEnumerateInstanceExtensionProperties(列挙)");
    if (!HasExtension(extensions.data(), ext_count, kVulkanEnableExtension)) {
      throw std::runtime_error(
          "XR_KHR_vulkan_enable がランタイムに存在しません。対応ランタイム(例: SteamVR)を導入してください");
    }
    has_local_floor_ = HasExtension(extensions.data(), ext_count, kLocalFloorExtension);

    // 2. インスタンスを生成する。XR_EXT_local_floor があれば有効化する
    // （M4 の local-floor 参照空間用。無ければ付けず、従来どおりの 1 拡張で作る）。
    XrApplicationInfo app_info{};
    std::snprintf(app_info.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "%s", "xrift_new");
    app_info.applicationVersion = 1;
    app_info.apiVersion = XR_API_VERSION_1_0;
    const char* enabled_all[] = {kVulkanEnableExtension, kLocalFloorExtension};
    const char* enabled_vulkan_only[] = {kVulkanEnableExtension};
    XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
    create_info.applicationInfo = app_info;
    if (has_local_floor_) {
      create_info.enabledExtensionCount = 2;
      create_info.enabledExtensionNames = enabled_all;
    } else {
      create_info.enabledExtensionCount = 1;
      create_info.enabledExtensionNames = enabled_vulkan_only;
    }
    CheckXr(xrCreateInstance(&create_info, &instance_), "xrCreateInstance");
    local_floor_enabled_ = has_local_floor_;
    if (local_floor_enabled_) {
      Info("XR_EXT_local_floor を有効化した（local-floor 参照空間が利用可）");
    }

    CheckXr(xrGetInstanceProperties(instance_, &instance_props_), "xrGetInstanceProperties");

    // 3. HMD の system を解決する。見つからない場合は環境要因が分かる文面にする。
    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    const XrResult system_result = xrGetSystem(instance_, &system_info, &system_);
    if (system_result == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
      throw std::runtime_error(
          std::string("HMD が見つかりません。SteamVR などが起動しているか確認してください (xrGetSystem: ") +
          XrResultToString(system_result) + ")");
    }
    CheckXr(system_result, "xrGetSystem");
    CheckXr(xrGetSystemProperties(instance_, system_, &system_props_), "xrGetSystemProperties");

    // 4. XR_KHR_vulkan_enable の拡張関数を取得する。
    pfn_requirements_ = reinterpret_cast<PFN_xrGetVulkanGraphicsRequirementsKHR>(
        LoadProc(instance_, "xrGetVulkanGraphicsRequirementsKHR"));
    pfn_instance_extensions_ = reinterpret_cast<PFN_xrGetVulkanInstanceExtensionsKHR>(
        LoadProc(instance_, "xrGetVulkanInstanceExtensionsKHR"));
    pfn_device_extensions_ = reinterpret_cast<PFN_xrGetVulkanDeviceExtensionsKHR>(
        LoadProc(instance_, "xrGetVulkanDeviceExtensionsKHR"));
    pfn_graphics_device_ = reinterpret_cast<PFN_xrGetVulkanGraphicsDeviceKHR>(
        LoadProc(instance_, "xrGetVulkanGraphicsDeviceKHR"));

    Info("runtime: %s, system: %s", instance_props_.runtimeName, system_props_.systemName);
  } catch (...) {
    Destroy();
    throw;
  }
}

Instance::~Instance() { Destroy(); }

void Instance::Destroy() {
  if (instance_ != XR_NULL_HANDLE) {
    const XrResult result = xrDestroyInstance(instance_);
    if (result != XR_SUCCESS) {
      Warn("xrDestroyInstance に失敗: %s", XrResultToString(result));
    }
    instance_ = XR_NULL_HANDLE;
  }
}

std::vector<XrViewConfigurationView> Instance::QueryStereoViews() const {
  uint32_t count = 0;
  CheckXr(xrEnumerateViewConfigurationViews(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                            0, &count, nullptr),
          "xrEnumerateViewConfigurationViews(件数)");
  std::vector<XrViewConfigurationView> views(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
  CheckXr(xrEnumerateViewConfigurationViews(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                            count, &count, views.data()),
          "xrEnumerateViewConfigurationViews(列挙)");
  views.resize(count);
  if (views.size() != 2) {
    throw std::runtime_error("PRIMARY_STEREO の view 数が 2 ではありません");
  }
  return views;
}

}  // namespace xrift::xr
