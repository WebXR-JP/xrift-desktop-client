#pragma once

#include <cstddef>
#include <cstdint>

#include "common/xr_vulkan.h"
#include "ipc/control_blocks.h"

// 制御ブロックの readback を担う層（transfer 側の照合前段）。
// 自前イメージからブロック A/B（各 32x32）だけを読み、画像全体は読まない。

namespace xrift::gfx {

class VulkanContext;

}  // namespace xrift::gfx

namespace xrift::transfer {

class ControlBlockReader {
 public:
  explicit ControlBlockReader(const gfx::VulkanContext& vk);

  // 自前イメージ（GENERAL 駐め）から 2 ブロックを読む。読み後に GENERAL へ戻す。
  // 自前イメージの不変条件（GENERAL 駐め）を壊さないことが呼び出し条件である。
  void Decode(vk::Image own, uint16_t* renderId16, uint16_t* poseTag);

 private:
  static constexpr size_t kStageSize = 2 * kControlBlockBytes;

  const vk::raii::Device* device_;
  const vk::raii::Queue* queue_;
  uint32_t qfi_ = 0;
  vk::raii::CommandPool pool_{nullptr};
  vk::raii::CommandBuffer cmd_{nullptr};
  vk::raii::Fence fence_{nullptr};
  vk::raii::Buffer stage_{nullptr};
  vk::raii::DeviceMemory stageMem_{nullptr};
};

}  // namespace xrift::transfer
