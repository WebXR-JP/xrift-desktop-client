#include "transfer/control_block_reader.h"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include "common/xr_result.h"
#include "gfx/vulkan_context.h"
#include "ipc/control_blocks.h"

namespace xrift::transfer {
namespace {

// vk::Result を返す呼び出しの検査。成功以外は vk::to_string 付きの例外にする。
void CheckVk(vk::Result result, const char* what) {
  if (result == vk::Result::eSuccess) {
    return;
  }
  throw std::runtime_error(std::string(what) + " に失敗 (VkResult=" + vk::to_string(result) + ")");
}

// host-visible な 1 バッファの確保。制御ブロック readback（数 KB 以内）専用である。
// 画像全体の staging には使わない。
void AllocateHostBuffer(const gfx::VulkanContext& vk, vk::raii::Buffer* buf,
                        vk::raii::DeviceMemory* mem, vk::DeviceSize size) {
  const vk::raii::Device& device = vk.device();
  vk::BufferCreateInfo buf_info{};
  buf_info.sType = vk::StructureType::eBufferCreateInfo;
  buf_info.size = size;
  buf_info.usage = vk::BufferUsageFlagBits::eTransferDst;
  buf_info.sharingMode = vk::SharingMode::eExclusive;
  *buf = vk::raii::Buffer(device, buf_info);
  const vk::MemoryRequirements req = buf->getMemoryRequirements();
  const vk::PhysicalDeviceMemoryProperties props = vk.physicalDevice().getMemoryProperties();
  int32_t index = -1;
  for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    const vk::MemoryPropertyFlags flags =
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    if ((req.memoryTypeBits & (1u << i)) != 0u &&
        (props.memoryTypes[i].propertyFlags & flags) == flags) {
      index = static_cast<int32_t>(i);
      break;
    }
  }
  if (index < 0) {
    throw std::runtime_error("制御ブロック用ステージングの host-visible メモリタイプが無い");
  }
  vk::MemoryAllocateInfo mem_info{};
  mem_info.sType = vk::StructureType::eMemoryAllocateInfo;
  mem_info.allocationSize = req.size;
  mem_info.memoryTypeIndex = static_cast<uint32_t>(index);
  *mem = vk::raii::DeviceMemory(device, mem_info);
  buf->bindMemory(**mem, 0);
}

}  // namespace

// 制御ブロックの readback。自前イメージからブロック A/B（各 32x32）だけを読み、
// renderId 下位 16bit と pose タグを復号する。画像全体は読まない
// （docs/architecture.md「経路」の制御メタデータの例外。XR ループを readback 待ちで止めないよう最小限）。
ControlBlockReader::ControlBlockReader(const gfx::VulkanContext& vk)
    : device_(&vk.device()), queue_(&vk.queue()), qfi_(vk.queueFamilyIndex()) {
  vk::CommandPoolCreateInfo pool_info{};
  pool_info.sType = vk::StructureType::eCommandPoolCreateInfo;
  pool_info.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
  pool_info.queueFamilyIndex = qfi_;
  pool_ = vk::raii::CommandPool(*device_, pool_info);
  vk::CommandBufferAllocateInfo alloc{};
  alloc.sType = vk::StructureType::eCommandBufferAllocateInfo;
  alloc.commandPool = *pool_;
  alloc.level = vk::CommandBufferLevel::ePrimary;
  alloc.commandBufferCount = 1;
  vk::raii::CommandBuffers bufs = device_->allocateCommandBuffers(alloc);
  cmd_ = std::move(bufs[0]);
  vk::FenceCreateInfo fence_info{};
  fence_info.sType = vk::StructureType::eFenceCreateInfo;
  fence_ = vk::raii::Fence(*device_, fence_info);
  AllocateHostBuffer(vk, &stage_, &stageMem_, kStageSize);
}

void ControlBlockReader::Decode(vk::Image own, uint16_t* renderId16, uint16_t* poseTag) {
  device_->resetFences(*fence_);
  cmd_.reset();
  vk::CommandBufferBeginInfo begin{};
  begin.sType = vk::StructureType::eCommandBufferBeginInfo;
  begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
  cmd_.begin(begin);

  vk::ImageSubresourceRange range{};
  range.aspectMask = vk::ImageAspectFlagBits::eColor;
  range.levelCount = 1;
  range.layerCount = 1;
  vk::ImageMemoryBarrier to_src{};
  to_src.sType = vk::StructureType::eImageMemoryBarrier;
  to_src.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
  to_src.dstAccessMask = vk::AccessFlagBits::eTransferRead;
  to_src.oldLayout = vk::ImageLayout::eGeneral;
  to_src.newLayout = vk::ImageLayout::eTransferSrcOptimal;
  to_src.image = own;
  to_src.subresourceRange = range;
  cmd_.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                       vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr, to_src);
  const uint32_t org_x[2] = {kControlBlockOrgAX, kControlBlockOrgBX};
  for (int i = 0; i < 2; ++i) {
    vk::BufferImageCopy region{};
    region.bufferOffset = static_cast<vk::DeviceSize>(i * kControlBlockBytes);
    region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = vk::Offset3D{static_cast<int32_t>(org_x[i]),
                                     static_cast<int32_t>(kControlBlockOrgY), 0};
    region.imageExtent = vk::Extent3D{kControlBlockPixels, kControlBlockPixels, 1};
    cmd_.copyImageToBuffer(own, vk::ImageLayout::eTransferSrcOptimal, *stage_, region);
  }
  vk::BufferMemoryBarrier host{};
  host.sType = vk::StructureType::eBufferMemoryBarrier;
  host.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
  host.dstAccessMask = vk::AccessFlagBits::eHostRead;
  host.buffer = *stage_;
  host.offset = 0;
  host.size = kStageSize;
  cmd_.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
                       {}, nullptr, host, nullptr);
  vk::ImageMemoryBarrier park = to_src;
  park.srcAccessMask = vk::AccessFlagBits::eTransferRead;
  park.dstAccessMask = vk::AccessFlagBits::eShaderRead;
  park.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
  park.newLayout = vk::ImageLayout::eGeneral;
  cmd_.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                       vk::PipelineStageFlagBits::eFragmentShader, {}, nullptr, nullptr, park);

  cmd_.end();
  vk::CommandBuffer raw = *cmd_;
  vk::SubmitInfo submit{};
  submit.sType = vk::StructureType::eSubmitInfo;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &raw;
  queue_->submit(submit, *fence_);
  CheckVk(device_->waitForFences(*fence_, vk::True, 2000000000u), "vkWaitForFences(ctrl)");

  void* mapped = nullptr;
  mapped = stageMem_.mapMemory(0, kStageSize);
  const uint8_t* bytes = static_cast<const uint8_t*>(mapped);
  *renderId16 = DecodeBlock16(bytes);
  *poseTag = DecodeBlock16(bytes + kControlBlockBytes);
  stageMem_.unmapMemory();
}

}  // namespace xrift::transfer
