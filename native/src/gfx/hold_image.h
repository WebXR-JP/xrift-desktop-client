#pragma once

#include <cstdint>

#include "common/xr_vulkan.h"

// 最後に受理したアトラスの exe 所有の複製を保つ層（gpu 側の保持専用）。
// CPU へは戻さない。提出の複製元として transfer 層から借用される。

namespace xrift::gfx {

class VulkanContext;

class HoldImage {
 public:
  explicit HoldImage(const VulkanContext& vk);

  ~HoldImage();

  HoldImage(const HoldImage&) = delete;
  HoldImage& operator=(const HoldImage&) = delete;

  bool Valid() const {
    return valid_ && static_cast<VkImage>(*image_) != VK_NULL_HANDLE && width_ > 0 &&
           height_ > 0;
  }
  void Invalidate() { valid_ = false; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  vk::Image image() const { return *image_; }

  // 自前イメージ（GENERAL 駐め）全体を複製する。両方とも GENERAL で返す。
  // 自前イメージの不変条件（GENERAL 駐め）を壊さないことが呼び出し条件である。
  // 失敗時は例外にし、valid は偽にする（半端な複製の再提出はしない）。
  void SaveFrom(vk::Image own, uint32_t w, uint32_t h);

 private:
  void Ensure(uint32_t w, uint32_t h);

  const vk::raii::Device* device_;
  const vk::raii::PhysicalDevice* physical_;
  const vk::raii::Queue* queue_;
  uint32_t qfi_ = 0;
  vk::raii::CommandPool pool_{nullptr};
  vk::raii::CommandBuffer cmd_{nullptr};
  vk::raii::Fence fence_{nullptr};
  vk::raii::Image image_{nullptr};
  vk::raii::DeviceMemory memory_{nullptr};
  uint64_t imageBytes_ = 0;
  uint64_t memoryBytes_ = 0;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  bool defined_ = false;
  bool valid_ = false;
};

}  // namespace xrift::gfx
