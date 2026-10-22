#include "gfx/clear_pass.h"

#include <stdexcept>
#include <string>

#include "common/log.h"
#include "common/xr_result.h"

namespace xrift::gfx {
namespace {

// vk::Result を返す呼び出しの検査。成功以外は vk::to_string 付きの例外にする。
void CheckVk(vk::Result result, const char* what) {
  if (result == vk::Result::eSuccess) {
    return;
  }
  throw std::runtime_error(std::string(what) + " に失敗 (VkResult=" + vk::to_string(result) + ")");
}

}  // namespace

ClearPass::ClearPass(const vk::raii::Device& device, uint32_t queueFamilyIndex,
                     const vk::raii::Queue& queue, const Swapchains& swapchains)
    : device_(&device), queue_(&queue) {
  vk::CommandPoolCreateInfo pool_info{};
  pool_info.sType = vk::StructureType::eCommandPoolCreateInfo;
  pool_info.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
  pool_info.queueFamilyIndex = queueFamilyIndex;
  pool_ = vk::raii::CommandPool(*device_, pool_info);

  const uint32_t eyes = swapchains.eyeCount();
  eyes_.reserve(eyes);
  for (uint32_t e = 0; e < eyes; ++e) {
    EyeResources res;
    res.swapchain = swapchains.handle(e);
    res.extent = swapchains.extent(e);
    const uint32_t images = swapchains.imageCount(e);
    res.images.reserve(images);
    for (uint32_t i = 0; i < images; ++i) {
      res.images.push_back(swapchains.image(e, i));
    }
    vk::CommandBufferAllocateInfo alloc{};
    alloc.sType = vk::StructureType::eCommandBufferAllocateInfo;
    alloc.commandPool = *pool_;
    alloc.level = vk::CommandBufferLevel::ePrimary;
    alloc.commandBufferCount = images;
    res.commands.reserve(images);
    vk::raii::CommandBuffers allocated = device_->allocateCommandBuffers(alloc);
    for (vk::raii::CommandBuffer& cmd : allocated) {
      res.commands.push_back(std::move(cmd));
    }
    vk::FenceCreateInfo fence_info{};
    fence_info.sType = vk::StructureType::eFenceCreateInfo;
    // 初回の待機を素通りさせるため、生成時は signaled にする。
    fence_info.flags = vk::FenceCreateFlagBits::eSignaled;
    res.fences.reserve(images);
    for (uint32_t i = 0; i < images; ++i) {
      vk::raii::Fence fence{nullptr};
      fence = vk::raii::Fence(*device_, fence_info);
      res.fences.push_back(std::move(fence));
    }
    eyes_.push_back(std::move(res));
  }
}

ClearPass::~ClearPass() {
  if (static_cast<VkCommandPool>(*pool_) == VK_NULL_HANDLE) {
    return;
  }
  // 仕掛かりの提出が残っていればここで吸収する。
  try {
    device_->waitIdle();
  } catch (const vk::SystemError& e) {
    Warn("vkDeviceWaitIdle に失敗 (VkResult=%s)",
         vk::to_string(static_cast<vk::Result>(e.code().value())).c_str());
  }
  // fences と pool_ (および commands) は raii の破棄で解放される。破棄順は宣言の逆順である。
}

void ClearPass::Draw(uint32_t eye) {
  if (eye >= eyes_.size()) {
    throw std::out_of_range("眼番号が範囲外です");
  }
  EyeResources& res = eyes_[eye];

  XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  uint32_t image_index = 0;
  CheckXr(xrAcquireSwapchainImage(res.swapchain, &acquire, &image_index),
          "xrAcquireSwapchainImage");
  XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait_info.timeout = XR_INFINITE_DURATION;
  CheckXr(xrWaitSwapchainImage(res.swapchain, &wait_info), "xrWaitSwapchainImage");
  if (image_index >= res.images.size()) {
    throw std::runtime_error("swapchain 画像番号が範囲外です");
  }

  vk::raii::Fence& fence = res.fences[image_index];
  vk::raii::CommandBuffer& cmd = res.commands[image_index];
  CheckVk(device_->waitForFences(*fence, vk::True, UINT64_MAX), "vkWaitForFences");
  device_->resetFences(*fence);
  cmd.reset();

  vk::CommandBufferBeginInfo begin{};
  begin.sType = vk::StructureType::eCommandBufferBeginInfo;
  begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
  cmd.begin(begin);

  vk::ImageSubresourceRange range{};
  range.aspectMask = vk::ImageAspectFlagBits::eColor;
  range.baseMipLevel = 0;
  range.levelCount = 1;
  range.baseArrayLayer = 0;
  range.layerCount = 1;
  vk::ImageMemoryBarrier to_dst{};
  to_dst.sType = vk::StructureType::eImageMemoryBarrier;
  to_dst.srcAccessMask = {};
  to_dst.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
  to_dst.oldLayout = vk::ImageLayout::eUndefined;
  to_dst.newLayout = vk::ImageLayout::eTransferDstOptimal;
  to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_dst.image = res.images[image_index];
  to_dst.subresourceRange = range;
  cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer,
                      {}, nullptr, nullptr, to_dst);

  vk::ClearColorValue color{};
  color.float32[0] = kEyeClearColors[eye][0];
  color.float32[1] = kEyeClearColors[eye][1];
  color.float32[2] = kEyeClearColors[eye][2];
  color.float32[3] = kEyeClearColors[eye][3];
  cmd.clearColorImage(res.images[image_index], vk::ImageLayout::eTransferDstOptimal, color, range);

  // コンポジタ渡し用に COLOR_ATTACHMENT_OPTIMAL で終える。以降の書き手は存在しないため、
  // 可視性の起点は TRANSFER_WRITE の公開、順序は TRANSFER → COLOR_ATTACHMENT_OUTPUT で保つ。
  vk::ImageMemoryBarrier to_present = to_dst;
  to_present.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
  to_present.dstAccessMask = {};
  to_present.oldLayout = vk::ImageLayout::eTransferDstOptimal;
  to_present.newLayout = vk::ImageLayout::eColorAttachmentOptimal;
  cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                      vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, nullptr, nullptr,
                      to_present);

  cmd.end();
  vk::CommandBuffer raw_cmd = *cmd;
  vk::SubmitInfo submit{};
  submit.sType = vk::StructureType::eSubmitInfo;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &raw_cmd;
  queue_->submit(submit, *fence);
  CheckVk(device_->waitForFences(*fence, vk::True, UINT64_MAX), "vkWaitForFences(描画完了)");

  XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  CheckXr(xrReleaseSwapchainImage(res.swapchain, &release), "xrReleaseSwapchainImage");
}

}  // namespace xrift::gfx
