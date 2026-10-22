#include "gfx/blit_pass.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "common/log.h"
#include "common/xr_result.h"
#include "gfx/swapchains.h"
#include "gfx/vulkan_context.h"

namespace xrift::gfx {
namespace {

// vk::Result を返す呼び出しの検査。成功以外は vk::to_string 付きの例外にする。
void CheckVk(vk::Result result, const char* what) {
  if (result == vk::Result::eSuccess) {
    return;
  }
  throw std::runtime_error(std::string(what) + " に失敗 (VkResult=" + vk::to_string(result) + ")");
}

uint64_t NowUs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// host-visible な 1 バッファの確保。マーカー画素 readback（1 画素）専用である。
// 画像全体の staging には使わない。
void AllocateHostBuffer(const VulkanContext& vk, vk::raii::Buffer* buf,
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

// 期待色の範囲検査。各成分が min <= value <= max のとき真である。
bool MatchesMarker(uint8_t b, uint8_t g, uint8_t r, const MarkerColor& expected) {
  return expected.minR <= r && r <= expected.maxR && expected.minG <= g && g <= expected.maxG &&
         expected.minB <= b && b <= expected.maxB;
}

}  // namespace

BlitPass::BlitPass(const VulkanContext& vk, const Swapchains& swapchains,
                   const MarkerLayout& markers)
    : device_(&vk.device()), queue_(&vk.queue()), markers_(markers) {
  vk::CommandPoolCreateInfo pool_info{};
  pool_info.sType = vk::StructureType::eCommandPoolCreateInfo;
  pool_info.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
  pool_info.queueFamilyIndex = vk.queueFamilyIndex();
  pool_ = vk::raii::CommandPool(*device_, pool_info);
  vk::CommandBufferAllocateInfo alloc{};
  alloc.sType = vk::StructureType::eCommandBufferAllocateInfo;
  alloc.commandPool = *pool_;
  alloc.level = vk::CommandBufferLevel::ePrimary;
  alloc.commandBufferCount = 2;
  vk::raii::CommandBuffers bufs = device_->allocateCommandBuffers(alloc);
  cmds_.push_back(std::move(bufs[0]));
  cmds_.push_back(std::move(bufs[1]));
  vk::FenceCreateInfo fence_info{};
  fence_info.sType = vk::StructureType::eFenceCreateInfo;
  fence_info.flags = vk::FenceCreateFlagBits::eSignaled;
  fence_ = vk::raii::Fence(*device_, fence_info);
  AllocateHostBuffer(vk, &stage_, &stageMem_, kStageSize);
  for (uint32_t e = 0; e < swapchains.eyeCount(); ++e) {
    EyeRes res;
    res.swapchain = swapchains.handle(e);
    res.extent = swapchains.extent(e);
    for (uint32_t i = 0; i < swapchains.imageCount(e); ++i) {
      res.images.push_back(swapchains.image(e, i));
    }
    eyes_.push_back(std::move(res));
  }
}

BlitPass::Result BlitPass::Blit(uint32_t eye, vk::Image own, uint32_t ownW, uint32_t ownH,
                                uint32_t srcHalf) {
  Result out{};
  const uint64_t t0 = NowUs();
  if (eye >= eyes_.size()) {
    throw std::out_of_range("眼番号が範囲外です");
  }
  EyeRes& res = eyes_[eye];

  XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  uint32_t image_index = 0;
  CheckXr(xrAcquireSwapchainImage(res.swapchain, &acquire, &image_index), "xrAcquireSwapchainImage");
  XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait_info.timeout = XR_INFINITE_DURATION;
  CheckXr(xrWaitSwapchainImage(res.swapchain, &wait_info), "xrWaitSwapchainImage");
  if (image_index >= res.images.size()) {
    throw std::runtime_error("swapchain 画像番号が範囲外です");
  }
  const vk::Image dst = res.images[image_index];

  CheckVk(device_->waitForFences(*fence_, vk::True, UINT64_MAX), "vkWaitForFences(blit)");
  device_->resetFences(*fence_);
  vk::raii::CommandBuffer& cmd = cmds_[eye & 1u];
  cmd.reset();
  vk::CommandBufferBeginInfo begin{};
  begin.sType = vk::StructureType::eCommandBufferBeginInfo;
  begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
  cmd.begin(begin);

  vk::ImageSubresourceRange range{};
  range.aspectMask = vk::ImageAspectFlagBits::eColor;
  range.levelCount = 1;
  range.layerCount = 1;

  vk::ImageMemoryBarrier src_to{};
  src_to.sType = vk::StructureType::eImageMemoryBarrier;
  src_to.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
  src_to.dstAccessMask = vk::AccessFlagBits::eTransferRead;
  src_to.oldLayout = vk::ImageLayout::eGeneral;
  src_to.newLayout = vk::ImageLayout::eTransferSrcOptimal;
  src_to.image = own;
  src_to.subresourceRange = range;
  vk::ImageMemoryBarrier dst_to{};
  dst_to.sType = vk::StructureType::eImageMemoryBarrier;
  dst_to.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
  dst_to.oldLayout = vk::ImageLayout::eUndefined;
  dst_to.newLayout = vk::ImageLayout::eTransferDstOptimal;
  dst_to.image = dst;
  dst_to.subresourceRange = range;
  std::array<vk::ImageMemoryBarrier, 2> to_blit{src_to, dst_to};
  cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                      {}, nullptr, nullptr, to_blit);

  // 半分の境界は「来た値を正」とする。自前イメージ実寸の半分であり、要求値は使わない。
  // 奇数幅では左半分を floor に寄せる（右に 1 列余る。左右のずれは 1 画素以内）。
  const uint32_t half = ownW / 2;
  const uint32_t x0 = (srcHalf == 0) ? 0 : half;
  const uint32_t x1 = (srcHalf == 0) ? half : ownW;
  const uint32_t halfW = (x1 > x0) ? (x1 - x0) : 1;
  vk::ImageBlit blit{};
  blit.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
  blit.srcSubresource.layerCount = 1;
  blit.srcOffsets[0] = vk::Offset3D{static_cast<int32_t>(x0), 0, 0};
  blit.srcOffsets[1] = vk::Offset3D{static_cast<int32_t>(x1), static_cast<int32_t>(ownH), 1};
  blit.dstSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
  blit.dstSubresource.layerCount = 1;
  blit.dstOffsets[0] = vk::Offset3D{0, 0, 0};
  blit.dstOffsets[1] = vk::Offset3D{static_cast<int32_t>(res.extent.width),
                                    static_cast<int32_t>(res.extent.height), 1};
  // 線形フィルタで拡大する。縮小方向には使わない（renderScale ≤ 1 かつ swapchain は
  // 推奨サイズのため、通常は拡大になる）。
  cmd.blitImage(own, vk::ImageLayout::eTransferSrcOptimal, dst,
                vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);

  vk::ImageMemoryBarrier src_park = src_to;
  src_park.srcAccessMask = vk::AccessFlagBits::eTransferRead;
  src_park.dstAccessMask = vk::AccessFlagBits::eShaderRead;
  src_park.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
  src_park.newLayout = vk::ImageLayout::eGeneral;
  cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                      vk::PipelineStageFlagBits::eFragmentShader, {}, nullptr, nullptr, src_park);

  // マーカー画素の dst 座標。半分内での相対位置を保って拡大する。
  // 左眼マーカー中心の半分内相対 x は 176、右眼も右半分起点で 176 である。
  const MarkerSpec& spec = (srcHalf == 0) ? markers_.left : markers_.right;
  const uint32_t mx = spec.x + spec.size / 2;
  const uint32_t my = spec.y + spec.size / 2;
  uint32_t sx = mx * res.extent.width / halfW;
  uint32_t sy = my * res.extent.height / (ownH > 0 ? ownH : 1);
  if (sx >= res.extent.width) {
    sx = res.extent.width - 1;
  }
  if (sy >= res.extent.height) {
    sy = res.extent.height - 1;
  }
  {
    // 提出画像から 1 画素だけ読む。コンポジタ渡しのレイアウトに戻してから終える。
    vk::ImageMemoryBarrier to_src = dst_to;
    to_src.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    to_src.dstAccessMask = vk::AccessFlagBits::eTransferRead;
    to_src.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    to_src.newLayout = vk::ImageLayout::eTransferSrcOptimal;
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                        vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr, to_src);
    vk::BufferImageCopy region{};
    region.bufferOffset = 0;
    region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = vk::Offset3D{static_cast<int32_t>(sx), static_cast<int32_t>(sy), 0};
    region.imageExtent = vk::Extent3D{1, 1, 1};
    cmd.copyImageToBuffer(dst, vk::ImageLayout::eTransferSrcOptimal, *stage_, region);
    vk::BufferMemoryBarrier host{};
    host.sType = vk::StructureType::eBufferMemoryBarrier;
    host.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    host.dstAccessMask = vk::AccessFlagBits::eHostRead;
    host.buffer = *stage_;
    host.offset = 0;
    host.size = kStageSize;
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
                        {}, nullptr, host, nullptr);
    vk::ImageMemoryBarrier to_present = to_src;
    to_present.srcAccessMask = vk::AccessFlagBits::eTransferRead;
    to_present.dstAccessMask = {};
    to_present.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
    to_present.newLayout = vk::ImageLayout::eColorAttachmentOptimal;
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                        vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, nullptr, nullptr,
                        to_present);
  }

  cmd.end();
  vk::CommandBuffer raw = *cmd;
  vk::SubmitInfo submit{};
  submit.sType = vk::StructureType::eSubmitInfo;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &raw;
  queue_->submit(submit, *fence_);
  CheckVk(device_->waitForFences(*fence_, vk::True, 2000000000u), "vkWaitForFences(blit 完了)");

  {
    void* mapped = nullptr;
    mapped = stageMem_.mapMemory(0, kStageSize);
    const uint8_t* p = static_cast<const uint8_t*>(mapped);
    // 左眼にマゼンタ、右眼にシアンが来ていること。逆なら失敗として数える。
    // 読み出し画素は p[0]=B, p[1]=G, p[2]=R である。
    const MarkerColor& expected = ((eye == 0) ? markers_.left : markers_.right).color;
    out.markerOk = MatchesMarker(p[0], p[1], p[2], expected);
    if (!out.markerOk) {
      Warn("左右分離検証: 眼 %u のマーカー不一致 (B=%u G=%u R=%u)", eye, p[0], p[1], p[2]);
    }
    stageMem_.unmapMemory();
  }

  XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  CheckXr(xrReleaseSwapchainImage(res.swapchain, &release), "xrReleaseSwapchainImage");
  out.blitUs = NowUs() - t0;
  return out;
}

}  // namespace xrift::gfx
