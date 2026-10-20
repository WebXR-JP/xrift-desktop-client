#pragma once

#include <cstdint>
#include <vector>

#include "common/xr_vulkan.h"

// 両眼ぶんの XrSwapchain（Vulkan イメージ）とフォーマット選択を担う。
// 眼ごとに arraySize=1 の swapchain を 1 本ずつ作る。
// フォーマットは vk::Format で保持し、int64_t との変換は XR 境界でのみ行う。

namespace xrift::gfx {

// 優先順に並べた swapchain フォーマット候補。
std::vector<vk::Format> PreferredSwapchainFormats();
// フォーマット値の表示名。未知の値は "不明なフォーマット" を返す。
// vk::to_string(Format) は存在するが "R8G8B8A8Srgb" 式の表記になるため、
// --probe の出力内容を変えないよう従来の "VK_FORMAT_..." 表記を保つ。
const char* SwapchainFormatName(vk::Format format);

class Swapchains {
 public:
  Swapchains(XrInstance instance, XrSession session,
             const std::vector<XrViewConfigurationView>& views);
  ~Swapchains();

  Swapchains(const Swapchains&) = delete;
  Swapchains& operator=(const Swapchains&) = delete;

  uint32_t eyeCount() const { return static_cast<uint32_t>(eyes_.size()); }
  XrSwapchain handle(uint32_t eye) const { return eyes_[eye].swapchain; }
  vk::Extent2D extent(uint32_t eye) const { return {eyes_[eye].width, eyes_[eye].height}; }
  uint32_t imageCount(uint32_t eye) const { return static_cast<uint32_t>(eyes_[eye].images.size()); }
  // XR 所有のイメージを非所有の vk::Image として返す。破棄してはならない。
  vk::Image image(uint32_t eye, uint32_t index) const {
    return vk::Image(eyes_[eye].images[index].image);
  }
  vk::Format format() const { return format_; }
  // 明示破棄。多重呼び出しは無視する。
  void Destroy();

 private:
  struct Eye {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    // XR 所有のイメージ一覧。vk::Image は都度生成する非所有ビューのため保持しない。
    std::vector<XrSwapchainImageVulkanKHR> images;
  };

  XrInstance instance_ = XR_NULL_HANDLE;
  std::vector<Eye> eyes_;
  vk::Format format_ = vk::Format::eUndefined;
};

}  // namespace xrift::gfx
