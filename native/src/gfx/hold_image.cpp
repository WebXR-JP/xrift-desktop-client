#include "gfx/hold_image.h"

#include <array>
#include <stdexcept>
#include <string>
#include <utility>

#include "gfx/vulkan_context.h"

namespace xrift::gfx {

// 最後に受理したアトラスの exe 所有の複製を保つ層（gpu 側の保持専用）。
// 新規受理が無いフレームでも同じ画像を出し直すため（黒フレーム防止）。
// 自前イメージは次の Offer で上書き・棄却画像で汚染されるため、受理確定時に
// GPU コピーで複製する。再提出はこの複製から swapchain へ blit し、
// pose/FOV は受理レコードのものをそのまま使う（付け替えはしない）。
// 寸法変化時は作り直す。世代進行（接続・空間）では呼び出し側が Invalidate する。
HoldImage::HoldImage(const VulkanContext& vk)
    : device_(&vk.device()),
      physical_(&vk.physicalDevice()),
      queue_(&vk.queue()),
      qfi_(vk.queueFamilyIndex()) {
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
}

HoldImage::~HoldImage() = default;

void HoldImage::SaveFrom(vk::Image own, uint32_t w, uint32_t h) {
  if (static_cast<VkImage>(own) == VK_NULL_HANDLE || w == 0 || h == 0) {
    throw std::runtime_error("hold 複製の元画像・寸法が無効です");
  }
  Ensure(w, h);
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
  vk::ImageMemoryBarrier own_to{};
  own_to.sType = vk::StructureType::eImageMemoryBarrier;
  own_to.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
  own_to.dstAccessMask = vk::AccessFlagBits::eTransferRead;
  own_to.oldLayout = vk::ImageLayout::eGeneral;
  own_to.newLayout = vk::ImageLayout::eTransferSrcOptimal;
  own_to.image = own;
  own_to.subresourceRange = range;
  vk::ImageMemoryBarrier hold_to{};
  hold_to.sType = vk::StructureType::eImageMemoryBarrier;
  hold_to.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
  hold_to.oldLayout =
      defined_ ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined;
  hold_to.newLayout = vk::ImageLayout::eTransferDstOptimal;
  hold_to.image = *image_;
  hold_to.subresourceRange = range;
  if (defined_) {
    hold_to.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    std::array<vk::ImageMemoryBarrier, 2> to_copy{own_to, hold_to};
    cmd_.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                         vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr, to_copy);
  } else {
    cmd_.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                         vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr, own_to);
    cmd_.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                         vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr, hold_to);
  }

  vk::ImageCopy region{};
  region.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
  region.srcSubresource.layerCount = 1;
  region.dstSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
  region.dstSubresource.layerCount = 1;
  region.extent = vk::Extent3D{w, h, 1};
  cmd_.copyImage(own, vk::ImageLayout::eTransferSrcOptimal, *image_,
                 vk::ImageLayout::eTransferDstOptimal, region);

  vk::ImageMemoryBarrier own_park = own_to;
  own_park.srcAccessMask = vk::AccessFlagBits::eTransferRead;
  own_park.dstAccessMask = vk::AccessFlagBits::eShaderRead;
  own_park.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
  own_park.newLayout = vk::ImageLayout::eGeneral;
  vk::ImageMemoryBarrier hold_park = hold_to;
  hold_park.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
  hold_park.dstAccessMask = vk::AccessFlagBits::eTransferRead;
  hold_park.oldLayout = vk::ImageLayout::eTransferDstOptimal;
  hold_park.newLayout = vk::ImageLayout::eGeneral;
  std::array<vk::ImageMemoryBarrier, 2> parks{own_park, hold_park};
  cmd_.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                       {}, nullptr, nullptr, parks);

  cmd_.end();
  vk::CommandBuffer raw = *cmd_;
  vk::SubmitInfo submit{};
  submit.sType = vk::StructureType::eSubmitInfo;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &raw;
  try {
    queue_->submit(submit, *fence_);
  } catch (const vk::SystemError& e) {
    valid_ = false;
    const vk::Result result = static_cast<vk::Result>(e.code().value());
    throw std::runtime_error(std::string("vkQueueSubmit(hold) に失敗 (VkResult=") +
                             vk::to_string(result) + ")");
  }
  const vk::Result fence_result = device_->waitForFences(*fence_, vk::True, 2000000000u);
  if (fence_result != vk::Result::eSuccess) {
    valid_ = false;
    throw std::runtime_error("hold 複製の完了待ちが 2 秒でタイムアウト");
  }
  defined_ = true;
  valid_ = true;
}

void HoldImage::Ensure(uint32_t w, uint32_t h) {
  if (defined_ && w == width_ && h == height_) {
    return;
  }
  memory_.clear();
  image_.clear();
  defined_ = false;
  valid_ = false;

  vk::ImageCreateInfo info{};
  info.sType = vk::StructureType::eImageCreateInfo;
  info.imageType = vk::ImageType::e2D;
  info.format = vk::Format::eB8G8R8A8Unorm;
  info.extent = vk::Extent3D{w, h, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = vk::SampleCountFlagBits::e1;
  info.tiling = vk::ImageTiling::eOptimal;
  info.usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
  info.sharingMode = vk::SharingMode::eExclusive;
  info.initialLayout = vk::ImageLayout::eUndefined;
  image_ = vk::raii::Image(*device_, info);
  const vk::MemoryRequirements req = image_.getMemoryRequirements();
  // メモリ種別の選択は自前イメージと同じ基準（deviceLocal 優先）である。
  const vk::PhysicalDeviceMemoryProperties props = physical_->getMemoryProperties();
  int32_t index = -1;
  for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    if ((req.memoryTypeBits & (1u << i)) != 0u &&
        (props.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal) ==
            vk::MemoryPropertyFlagBits::eDeviceLocal) {
      index = static_cast<int32_t>(i);
      break;
    }
  }
  if (index < 0) {
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
      if ((req.memoryTypeBits & (1u << i)) != 0u) {
        index = static_cast<int32_t>(i);
        break;
      }
    }
  }
  if (index < 0) {
    throw std::runtime_error("hold イメージのメモリタイプが無い");
  }
  vk::MemoryAllocateInfo alloc{};
  alloc.sType = vk::StructureType::eMemoryAllocateInfo;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = static_cast<uint32_t>(index);
  memory_ = vk::raii::DeviceMemory(*device_, alloc);
  image_.bindMemory(*memory_, 0);
  width_ = w;
  height_ = h;
}

}  // namespace xrift::gfx
