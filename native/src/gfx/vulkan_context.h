#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/xr_vulkan.h"
#include "xr/instance.h"

// OpenXR ランタイムの要件に従って VkInstance/VkPhysicalDevice/VkDevice を作る。
// XR_KHR_vulkan_enable (v1) の手順専用。破棄は生成の逆順で行う。
// 生成したハンドルは vk::raii で所有する。OpenXR との境界では生ハンドルに変換して渡す。

namespace xrift::gfx {

class VulkanContext {
 public:
  // extraDeviceExtensions はランタイム要求に足す分だけを指定する。
  // 空なら従来どおりランタイム要求（+ 不足時の swapchain）のみで作る。
  // 既存の呼び出しは引数なしのまま動きが変わらない。
  explicit VulkanContext(const xr::Instance& xr,
                         const std::vector<std::string>& extraDeviceExtensions = {});
  ~VulkanContext();

  VulkanContext(const VulkanContext&) = delete;
  VulkanContext& operator=(const VulkanContext&) = delete;

  const vk::raii::Context& context() const { return context_; }
  const vk::raii::Instance& instance() const { return instance_; }
  const vk::raii::PhysicalDevice& physicalDevice() const { return physical_device_; }
  const vk::raii::Device& device() const { return device_; }
  const vk::raii::Queue& queue() const { return queue_; }
  uint32_t queueFamilyIndex() const { return queue_family_index_; }
  const std::string& gpuName() const { return gpu_name_; }

  // OpenXR セッション生成用の Vk バインディングを組み立てる。
  // ハンドルの所有はこの VulkanContext に残り、返却は値（ローカルコピー）である。
  XrGraphicsBindingVulkanKHR xrGraphicsBinding() const;

 private:
  // 破棄順の担保: メンバは宣言順に構築され、逆順に破棄される。
  // 生成順 (context → instance → device) に対し、破棄は (device → instance → context) となる。
  // 子 (device) が親 (instance) より後に破棄されることは型で防止される。
  // context_ は読み込みライブラリ (vulkan-1.dll) の寿命を持つため、最初に宣言し最後に破棄する。
  // queue_ と physical_device_ は非所有 (device/instance に属する) のため破棄時の実処理はない。
  vk::raii::Context context_;
  vk::raii::Instance instance_{nullptr};
  vk::raii::PhysicalDevice physical_device_{nullptr};
  vk::raii::Device device_{nullptr};
  vk::raii::Queue queue_{nullptr};
  uint32_t queue_family_index_ = 0;
  std::string gpu_name_;
};

}  // namespace xrift::gfx
