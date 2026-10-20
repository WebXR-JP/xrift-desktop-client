#pragma once

#include <cstdint>
#include <string>

#include "common/xr_vulkan.h"
#include "xr/instance.h"

// OpenXR ランタイムの要件に従って VkInstance/VkPhysicalDevice/VkDevice を作る。
// XR_KHR_vulkan_enable (v1) の手順専用。破棄は生成の逆順で行う。

namespace xrift::gfx {

class VulkanContext {
 public:
  explicit VulkanContext(const xr::Instance& xr);
  ~VulkanContext();

  VulkanContext(const VulkanContext&) = delete;
  VulkanContext& operator=(const VulkanContext&) = delete;

  VkInstance instance() const { return instance_; }
  VkPhysicalDevice physicalDevice() const { return physical_device_; }
  VkDevice device() const { return device_; }
  VkQueue queue() const { return queue_; }
  uint32_t queueFamilyIndex() const { return queue_family_index_; }
  const std::string& gpuName() const { return gpu_name_; }

 private:
  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queue_family_index_ = 0;
  std::string gpu_name_;
};

}  // namespace xrift::gfx
