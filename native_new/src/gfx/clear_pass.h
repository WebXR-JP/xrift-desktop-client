#pragma once

#include <cstdint>
#include <vector>

#include "common/xr_vulkan.h"
#include "gfx/swapchains.h"

// 両眼の単色クリアを担う。acquire/wait/clear/release の一連を 1 眼単位で行う。
// コマンドバッファとフェンスは swapchain 画像ぶん事前に確保する。

namespace xrift::gfx {

class ClearPass {
 public:
  ClearPass(VkDevice device, uint32_t queueFamilyIndex, VkQueue queue, const Swapchains& swapchains);
  ~ClearPass();

  ClearPass(const ClearPass&) = delete;
  ClearPass& operator=(const ClearPass&) = delete;

  // 1 眼ぶんの acquire → clear → release を行う。
  void Draw(uint32_t eye);

 private:
  struct EyeResources {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    VkExtent2D extent{0, 0};
    std::vector<VkImage> images;
    std::vector<VkCommandBuffer> commands;
    std::vector<VkFence> fences;
  };

  // 眼ごとのクリア色（線形 float）。左=暗い赤、右=暗い青にし、両眼の出方を目視で判別する。
  // 値は見た目の識別用であり、色域の正確さを問わない。
  static constexpr float kEyeClearColors[2][4] = {
      {0.25f, 0.02f, 0.02f, 1.0f},
      {0.02f, 0.02f, 0.35f, 1.0f},
  };

  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  std::vector<EyeResources> eyes_;
};

}  // namespace xrift::gfx
