#pragma once

#include <vector>

#include "common/xr_vulkan.h"

// XrInstance の生成と HMD system の解決を担う。
// Vulkan デバイス側が必要とする XR_KHR_vulkan_enable の関数ポインタもここで束ねる。

namespace xrift::xr {

class Instance {
 public:
  Instance();
  ~Instance();

  Instance(const Instance&) = delete;
  Instance& operator=(const Instance&) = delete;

  XrInstance handle() const { return instance_; }
  XrSystemId systemId() const { return system_; }
  const XrInstanceProperties& instanceProperties() const { return instance_props_; }
  const XrSystemProperties& systemProperties() const { return system_props_; }
  // XR_EXT_local_floor が利用可か。今回は使わないが将来の参照用に保持する。
  bool hasLocalFloor() const { return has_local_floor_; }

  // PRIMARY_STEREO の view 構成を返す。session を必要としない。
  std::vector<XrViewConfigurationView> QueryStereoViews() const;

  PFN_xrGetVulkanGraphicsRequirementsKHR vulkanRequirementsProc() const { return pfn_requirements_; }
  PFN_xrGetVulkanInstanceExtensionsKHR vulkanInstanceExtensionsProc() const {
    return pfn_instance_extensions_;
  }
  PFN_xrGetVulkanDeviceExtensionsKHR vulkanDeviceExtensionsProc() const {
    return pfn_device_extensions_;
  }
  PFN_xrGetVulkanGraphicsDeviceKHR vulkanGraphicsDeviceProc() const { return pfn_graphics_device_; }

 private:
  XrInstance instance_ = XR_NULL_HANDLE;
  XrSystemId system_ = XR_NULL_SYSTEM_ID;
  XrInstanceProperties instance_props_{XR_TYPE_INSTANCE_PROPERTIES};
  XrSystemProperties system_props_{XR_TYPE_SYSTEM_PROPERTIES};
  bool has_local_floor_ = false;

  PFN_xrGetVulkanGraphicsRequirementsKHR pfn_requirements_ = nullptr;
  PFN_xrGetVulkanInstanceExtensionsKHR pfn_instance_extensions_ = nullptr;
  PFN_xrGetVulkanDeviceExtensionsKHR pfn_device_extensions_ = nullptr;
  PFN_xrGetVulkanGraphicsDeviceKHR pfn_graphics_device_ = nullptr;
};

}  // namespace xrift::xr
