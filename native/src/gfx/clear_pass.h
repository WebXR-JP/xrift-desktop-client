#pragma once

#include <cstdint>
#include <vector>

#include "common/xr_vulkan.h"
#include "gfx/swapchains.h"

// 両眼の単色クリアを担う。acquire/wait/clear/release の一連を 1 眼単位で行う。
// コマンドバッファとフェンスは swapchain 画像ぶん事前に確保する。
// VkDevice/VkQueue は VulkanContext が所有し、ここでは借用する。

namespace xrift::gfx {

class ClearPass {
 public:
  ClearPass(const vk::raii::Device& device, uint32_t queueFamilyIndex,
            const vk::raii::Queue& queue, const Swapchains& swapchains);
  ~ClearPass();

  ClearPass(const ClearPass&) = delete;
  ClearPass& operator=(const ClearPass&) = delete;

  // 1 眼ぶんの acquire → clear → release を行う。
  void Draw(uint32_t eye);

 private:
  struct EyeResources {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    vk::Extent2D extent{0, 0};
    // XR 所有イメージの非所有ビュー。破棄しない。
    std::vector<vk::Image> images;
    std::vector<vk::raii::CommandBuffer> commands;
    std::vector<vk::raii::Fence> fences;
  };

  // 眼ごとのクリア色（線形 float）。左=暗い赤、右=暗い青にし、両眼の出方を目視で判別する。
  // 値は見た目の識別用であり、色域の正確さを問わない。
  static constexpr float kEyeClearColors[2][4] = {
      {0.25f, 0.02f, 0.02f, 1.0f},
      {0.02f, 0.02f, 0.35f, 1.0f},
  };

  // 破棄順の担保: 借用 (device_/queue_) より子 (pool_/eyes_) を先に破棄する。
  // メンバは宣言順に構築・逆順に破棄されるため、pool_ と commands/fences が
  // device_ より後に破棄されることは型で防止される。
  // なお device_/queue_ 自体の寿命は VulkanContext が持ち、ClearPass より長生きすることが前提である。
  const vk::raii::Device* device_ = nullptr;
  const vk::raii::Queue* queue_ = nullptr;
  vk::raii::CommandPool pool_{nullptr};
  std::vector<EyeResources> eyes_;
};

}  // namespace xrift::gfx
