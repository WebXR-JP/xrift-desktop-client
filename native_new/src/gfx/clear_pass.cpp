#include "gfx/clear_pass.h"

#include <stdexcept>
#include <string>

#include "common/log.h"
#include "common/xr_result.h"

namespace xrift::gfx {
namespace {

void CheckVk(VkResult result, const char* what) {
  if (result == VK_SUCCESS) {
    return;
  }
  throw std::runtime_error(std::string(what) + " に失敗 (VkResult=" +
                           std::to_string(static_cast<int>(result)) + ")");
}

}  // namespace

ClearPass::ClearPass(VkDevice device, uint32_t queueFamilyIndex, VkQueue queue,
                     const Swapchains& swapchains)
    : device_(device), queue_(queue) {
  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = queueFamilyIndex;
  CheckVk(vkCreateCommandPool(device_, &pool_info, nullptr, &pool_), "vkCreateCommandPool");

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
    res.commands.resize(images);
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = images;
    CheckVk(vkAllocateCommandBuffers(device_, &alloc, res.commands.data()),
            "vkAllocateCommandBuffers");
    res.fences.resize(images);
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    // 初回の待機を素通りさせるため、生成時は signaled にする。
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < images; ++i) {
      CheckVk(vkCreateFence(device_, &fence_info, nullptr, &res.fences[i]), "vkCreateFence");
    }
    eyes_.push_back(std::move(res));
  }
}

ClearPass::~ClearPass() {
  if (pool_ == VK_NULL_HANDLE) {
    return;
  }
  // 仕掛かりの提出が残っていればここで吸収する。
  const VkResult idle = vkDeviceWaitIdle(device_);
  if (idle != VK_SUCCESS) {
    Warn("vkDeviceWaitIdle に失敗 (VkResult=%d)", static_cast<int>(idle));
  }
  for (EyeResources& res : eyes_) {
    for (VkFence fence : res.fences) {
      vkDestroyFence(device_, fence, nullptr);
    }
    res.fences.clear();
  }
  // プールの破棄でコマンドバッファもまとめて解放される。
  vkDestroyCommandPool(device_, pool_, nullptr);
  pool_ = VK_NULL_HANDLE;
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

  VkFence fence = res.fences[image_index];
  VkCommandBuffer cmd = res.commands[image_index];
  CheckVk(vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
  CheckVk(vkResetFences(device_, 1, &fence), "vkResetFences");
  CheckVk(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  CheckVk(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");

  const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageMemoryBarrier to_dst{};
  to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  to_dst.srcAccessMask = 0;
  to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_dst.image = res.images[image_index];
  to_dst.subresourceRange = range;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &to_dst);

  VkClearColorValue color{};
  color.float32[0] = kEyeClearColors[eye][0];
  color.float32[1] = kEyeClearColors[eye][1];
  color.float32[2] = kEyeClearColors[eye][2];
  color.float32[3] = kEyeClearColors[eye][3];
  vkCmdClearColorImage(cmd, res.images[image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color,
                       1, &range);

  // コンポジタ渡し用に COLOR_ATTACHMENT_OPTIMAL で終える。以降の書き手は存在しないため、
  // 可視性の起点は TRANSFER_WRITE の公開、順序は TRANSFER → COLOR_ATTACHMENT_OUTPUT で保つ。
  VkImageMemoryBarrier to_present = to_dst;
  to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_present.dstAccessMask = 0;
  to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_present.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr,
                       1, &to_present);

  CheckVk(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  CheckVk(vkQueueSubmit(queue_, 1, &submit, fence), "vkQueueSubmit");
  CheckVk(vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences(描画完了)");

  XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  CheckXr(xrReleaseSwapchainImage(res.swapchain, &release), "xrReleaseSwapchainImage");
}

}  // namespace xrift::gfx
