#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/xr_vulkan.h"

// 両眼ぶんの XrSwapchain（Vulkan イメージ）とフォーマット選択を担う。
// 眼ごとに arraySize=1 の swapchain を 1 本ずつ作る。

namespace xrift::gfx {

// 優先順に並べた swapchain フォーマット候補。
std::vector<int64_t> PreferredSwapchainFormats();
// フォーマット値の表示名。未知の値は "不明なフォーマット" を返す。
const char* SwapchainFormatName(int64_t format);

class Swapchains {
 public:
  Swapchains(XrInstance instance, XrSession session,
             const std::vector<XrViewConfigurationView>& views);
  ~Swapchains();

  Swapchains(const Swapchains&) = delete;
  Swapchains& operator=(const Swapchains&) = delete;

  uint32_t eyeCount() const { return static_cast<uint32_t>(eyes_.size()); }
  XrSwapchain handle(uint32_t eye) const { return eyes_[eye].swapchain; }
  VkExtent2D extent(uint32_t eye) const { return {eyes_[eye].width, eyes_[eye].height}; }
  uint32_t imageCount(uint32_t eye) const { return static_cast<uint32_t>(eyes_[eye].images.size()); }
  VkImage image(uint32_t eye, uint32_t index) const { return eyes_[eye].images[index].image; }
  int64_t format() const { return format_; }
  // 明示破棄。多重呼び出しは無視する。
  void Destroy();

 private:
  struct Eye {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<XrSwapchainImageVulkanKHR> images;
  };

  XrInstance instance_ = XR_NULL_HANDLE;
  std::vector<Eye> eyes_;
  int64_t format_ = 0;
};

}  // namespace xrift::gfx
