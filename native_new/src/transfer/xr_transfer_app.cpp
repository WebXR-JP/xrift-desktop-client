#include "transfer/xr_transfer_app.h"

#ifdef _WIN32

#include <windows.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "common/log.h"
#include "common/xr_result.h"
#include "gfx/swapchains.h"
#include "transfer/m2_blocks.h"
#include "transfer/pipe_client.h"
#include "transfer/protocol.h"
#include "transfer/render_record.h"
#include "transfer/shared_texture.h"
#include "xr/session.h"

namespace xrift::transfer {
namespace {

// M2 画像配置（harness/electron/README.md の「M2 画像配置」と一致させること）。
// アトラスは左右横並び（左半分=左眼、右半分=右眼）。来た値を正とするため、
// 半分の境界は自前イメージの実寸 (ownW/2) で決める。奇数幅では左を floor に寄せる。
constexpr uint32_t kBlockCell = 4;      // 1 セルの一辺。
constexpr uint32_t kBlockOrgAX = 64;    // ブロック A（renderId 下位 16bit）の左上 x。
constexpr uint32_t kBlockOrgBX = 128;   // ブロック B（pose タグ 16bit）の左上 x。
constexpr uint32_t kBlockOrgY = 64;     // 両ブロックの左上 y。
constexpr uint32_t kBlockSize = 32;     // ブロックの一辺。
constexpr uint32_t kMarkerSize = 32;    // 眼マーカーの一辺。
constexpr uint32_t kMarkerLeftX = 160;  // 左マーカー（マゼンタ）の左上 x（アトラス座標）。
constexpr uint32_t kMarkerRightDx = 160;  // 右マーカー（シアン）の左上 x（右半分起点から）。
constexpr uint32_t kMarkerY = 8;          // 両マーカーの左上 y（予約ブロックと重ねない）。
// RenderRequest の予約先頭バイトに載せる旧 pose 混入の合図。
// harness 側フック用であり、通常の要求では 0 である。
constexpr uint8_t kStaleMarker = 0x5Au;
// Offer 受信ポーリングの待ち上限（1ms は意図的な値であり、仮置きではない）。
// XR ループを止めないよう 1 フレームの待ちが 1ms 程度に収まることを保証する。
// ReceiveWithDeadline の期限切れは「その回の受信を打ち切る」意味であり、
// borrow 資源の返却許可にはならない。ここでの期限は受信のポーリング間隔である。
// なお deadlineUs=0 は無期限待ちの意味であるため、ノンブロッキングのつもりで
// 現在時刻や 0 をそのまま期限に渡さないこと。
constexpr uint64_t kOfferPollWaitUs = 1000u;
// Offer が途絶えた場合に producer 終了とみなす無受信の上限（5 秒は意図的な値）。
// harness は計測終了で Offer を止めてから読取側を開いたままにし（socket.end の半閉鎖）、
// 8 秒の猶予後に kill する。exe が受信待ちを続けると統計を出す前に殺されるため、
// 通常時（約 90Hz で Offer が来る）では起きない 5 秒の途絶で新規受付を止めて終了する。
// 初回 Offer 前は適用しない（起動直後の誤終了を避ける）。
constexpr uint64_t kProducerIdleTimeoutUs = 5000000u;

uint64_t NowUs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void CheckVk(vk::Result result, const char* what) {
  if (result == vk::Result::eSuccess) {
    return;
  }
  throw std::runtime_error(std::string(what) + " に失敗 (VkResult=" + vk::to_string(result) + ")");
}

template <typename F>
void InvokeVk(const char* what, F&& f) {
  try {
    f();
  } catch (const vk::SystemError& e) {
    const vk::Result result = static_cast<vk::Result>(e.code().value());
    throw std::runtime_error(std::string(what) + " に失敗 (VkResult=" + vk::to_string(result) +
                             ")");
  }
}

bool IsMagenta(uint8_t b, uint8_t g, uint8_t r) { return r > 200 && b > 200 && g < 80; }
bool IsCyan(uint8_t b, uint8_t g, uint8_t r) { return g > 200 && b > 200 && r < 80; }

EyePose ToEyePose(const XrPosef& p) {
  EyePose e{};
  e.position[0] = p.position.x;
  e.position[1] = p.position.y;
  e.position[2] = p.position.z;
  e.orientation[0] = p.orientation.x;
  e.orientation[1] = p.orientation.y;
  e.orientation[2] = p.orientation.z;
  e.orientation[3] = p.orientation.w;
  return e;
}

EyeFov ToEyeFov(const XrFovf& f) {
  EyeFov e{};
  e.left = f.angleLeft;
  e.right = f.angleRight;
  e.up = f.angleUp;
  e.down = f.angleDown;
  return e;
}

// host-visible な 1 バッファの確保。M2 の制御メタ readback（数 KB 以内）専用である。
// 画像全体の staging には使わない。
void AllocateHostBuffer(const gfx::VulkanContext& vk, vk::raii::Buffer* buf,
                        vk::raii::DeviceMemory* mem, vk::DeviceSize size) {
  const vk::raii::Device& device = vk.device();
  vk::BufferCreateInfo buf_info{};
  buf_info.sType = vk::StructureType::eBufferCreateInfo;
  buf_info.size = size;
  buf_info.usage = vk::BufferUsageFlagBits::eTransferDst;
  buf_info.sharingMode = vk::SharingMode::eExclusive;
  InvokeVk("vkCreateBuffer(M2)", [&] { *buf = vk::raii::Buffer(device, buf_info); });
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
    throw std::runtime_error("M2 ステージングの host-visible メモリタイプが無い");
  }
  vk::MemoryAllocateInfo mem_info{};
  mem_info.sType = vk::StructureType::eMemoryAllocateInfo;
  mem_info.allocationSize = req.size;
  mem_info.memoryTypeIndex = static_cast<uint32_t>(index);
  InvokeVk("vkAllocateMemory(M2)", [&] { *mem = vk::raii::DeviceMemory(device, mem_info); });
  InvokeVk("vkBindBufferMemory(M2)", [&] { buf->bindMemory(**mem, 0); });
}

// M2 制御メタデータの readback。自前イメージからブロック A/B（各 32x32）だけを読み、
// renderId 下位 16bit と pose タグを復号する。画像全体は読まない
// （DESIGN §1 の制御メタデータの例外。XR ループを readback 待ちで止めないよう最小限）。
class M2BlockReader {
 public:
  explicit M2BlockReader(const gfx::VulkanContext& vk)
      : device_(&vk.device()), queue_(&vk.queue()), qfi_(vk.queueFamilyIndex()) {
    vk::CommandPoolCreateInfo pool_info{};
    pool_info.sType = vk::StructureType::eCommandPoolCreateInfo;
    pool_info.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    pool_info.queueFamilyIndex = qfi_;
    InvokeVk("vkCreateCommandPool(M2)", [&] { pool_ = vk::raii::CommandPool(*device_, pool_info); });
    vk::CommandBufferAllocateInfo alloc{};
    alloc.sType = vk::StructureType::eCommandBufferAllocateInfo;
    alloc.commandPool = *pool_;
    alloc.level = vk::CommandBufferLevel::ePrimary;
    alloc.commandBufferCount = 1;
    InvokeVk("vkAllocateCommandBuffers(M2)", [&] {
      vk::raii::CommandBuffers bufs = device_->allocateCommandBuffers(alloc);
      cmd_ = std::move(bufs[0]);
    });
    vk::FenceCreateInfo fence_info{};
    fence_info.sType = vk::StructureType::eFenceCreateInfo;
    InvokeVk("vkCreateFence(M2)", [&] { fence_ = vk::raii::Fence(*device_, fence_info); });
    AllocateHostBuffer(vk, &stage_, &stageMem_, kStageSize);
  }

  // 自前イメージ（GENERAL 駐め）から 2 ブロックを読む。読み後に GENERAL へ戻す。
  // 自前イメージの不変条件（GENERAL 駐め）を壊さないことが呼び出し条件である。
  void Decode(vk::Image own, uint16_t* renderId16, uint16_t* poseTag) {
    InvokeVk("vkResetFences(M2)", [&] { device_->resetFences(*fence_); });
    InvokeVk("vkResetCommandBuffer(M2)", [&] { cmd_.reset(); });
    vk::CommandBufferBeginInfo begin{};
    begin.sType = vk::StructureType::eCommandBufferBeginInfo;
    begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    InvokeVk("vkBeginCommandBuffer(M2)", [&] { cmd_.begin(begin); });

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
    const uint32_t org_x[2] = {kBlockOrgAX, kBlockOrgBX};
    for (int i = 0; i < 2; ++i) {
      vk::BufferImageCopy region{};
      region.bufferOffset = static_cast<vk::DeviceSize>(i * kBlockBytes);
      region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
      region.imageSubresource.layerCount = 1;
      region.imageOffset = vk::Offset3D{static_cast<int32_t>(org_x[i]),
                                       static_cast<int32_t>(kBlockOrgY), 0};
      region.imageExtent = vk::Extent3D{kBlockSize, kBlockSize, 1};
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

    InvokeVk("vkEndCommandBuffer(M2)", [&] { cmd_.end(); });
    vk::CommandBuffer raw = *cmd_;
    vk::SubmitInfo submit{};
    submit.sType = vk::StructureType::eSubmitInfo;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &raw;
    InvokeVk("vkQueueSubmit(M2)", [&] { queue_->submit(submit, *fence_); });
    CheckVk(device_->waitForFences(*fence_, vk::True, 2000000000u), "vkWaitForFences(M2)");

    void* mapped = nullptr;
    InvokeVk("vkMapMemory(M2)", [&] { mapped = stageMem_.mapMemory(0, kStageSize); });
    const uint8_t* bytes = static_cast<const uint8_t*>(mapped);
    *renderId16 = DecodeBlock16(bytes);
    *poseTag = DecodeBlock16(bytes + kBlockBytes);
    stageMem_.unmapMemory();
  }

 private:
  static constexpr size_t kBlockBytes = kM2BlockPixels * kM2BlockPixels * 4;
  static constexpr size_t kStageSize = 2 * kBlockBytes;


  const vk::raii::Device* device_;
  const vk::raii::Queue* queue_;
  uint32_t qfi_ = 0;
  vk::raii::CommandPool pool_{nullptr};
  vk::raii::CommandBuffer cmd_{nullptr};
  vk::raii::Fence fence_{nullptr};
  vk::raii::Buffer stage_{nullptr};
  vk::raii::DeviceMemory stageMem_{nullptr};
};

// 受理画像の提出パス。acquire → blit → マーカー確認 → release を 1 眼単位で行う。
class BlitPass {
 public:
  BlitPass(const gfx::VulkanContext& vk, const gfx::Swapchains& swapchains)
      : device_(&vk.device()), queue_(&vk.queue()) {
    vk::CommandPoolCreateInfo pool_info{};
    pool_info.sType = vk::StructureType::eCommandPoolCreateInfo;
    pool_info.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    pool_info.queueFamilyIndex = vk.queueFamilyIndex();
    InvokeVk("vkCreateCommandPool(blit)", [&] { pool_ = vk::raii::CommandPool(*device_, pool_info); });
    vk::CommandBufferAllocateInfo alloc{};
    alloc.sType = vk::StructureType::eCommandBufferAllocateInfo;
    alloc.commandPool = *pool_;
    alloc.level = vk::CommandBufferLevel::ePrimary;
    alloc.commandBufferCount = 2;
    InvokeVk("vkAllocateCommandBuffers(blit)", [&] {
      vk::raii::CommandBuffers bufs = device_->allocateCommandBuffers(alloc);
      cmds_.push_back(std::move(bufs[0]));
      cmds_.push_back(std::move(bufs[1]));
    });
    vk::FenceCreateInfo fence_info{};
    fence_info.sType = vk::StructureType::eFenceCreateInfo;
    fence_info.flags = vk::FenceCreateFlagBits::eSignaled;
    InvokeVk("vkCreateFence(blit)", [&] { fence_ = vk::raii::Fence(*device_, fence_info); });
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
    format_ = swapchains.format();
  }

  struct Result {
    uint64_t blitUs = 0;
    bool markerOk = false;  // 左眼=マゼンタ、右眼=シアンの確認結果。
  };

  // 受理画像の片半分を各眼へ blit する。srcHalf: 0=左半分、1=右半分。
  // 色の扱い: 同一形式間の blit は複写・拡大であり、色空間の変換は行わない。
  // swapchain が sRGB 形式の場合、二重符号化の疑いが残る（DESIGN §9 の未決事項。
  // M2 では左右分離と pose 対応を優先し、色の最終確認は M5 まで持ち越す）。
  // blit 後に提出画像からマーカー 1 画素だけを読む（画像全体は読まない）。
  // 失敗時は例外にする（呼び出し側は layerCount=0 で終える）。
  Result Blit(uint32_t eye, vk::Image own, uint32_t ownW, uint32_t ownH, uint32_t srcHalf,
              bool checkMarker) {
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
    InvokeVk("vkResetFences(blit)", [&] { device_->resetFences(*fence_); });
    vk::raii::CommandBuffer& cmd = cmds_[eye & 1u];
    InvokeVk("vkResetCommandBuffer(blit)", [&] { cmd.reset(); });
    vk::CommandBufferBeginInfo begin{};
    begin.sType = vk::StructureType::eCommandBufferBeginInfo;
    begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    InvokeVk("vkBeginCommandBuffer(blit)", [&] { cmd.begin(begin); });

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
    const uint32_t mx = (srcHalf == 0) ? (kMarkerLeftX + kMarkerSize / 2)
                                       : (kMarkerRightDx + kMarkerSize / 2);
    const uint32_t my = kMarkerY + kMarkerSize / 2;
    uint32_t sx = mx * res.extent.width / halfW;
    uint32_t sy = my * res.extent.height / (ownH > 0 ? ownH : 1);
    if (sx >= res.extent.width) {
      sx = res.extent.width - 1;
    }
    if (sy >= res.extent.height) {
      sy = res.extent.height - 1;
    }
    if (checkMarker) {
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
    } else {
      // コンポジタ渡し用に COLOR_ATTACHMENT_OPTIMAL で終える（ClearPass と同じ）。
      vk::ImageMemoryBarrier to_present = dst_to;
      to_present.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
      to_present.dstAccessMask = {};
      to_present.oldLayout = vk::ImageLayout::eTransferDstOptimal;
      to_present.newLayout = vk::ImageLayout::eColorAttachmentOptimal;
      cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                          vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, nullptr, nullptr,
                          to_present);
    }

    InvokeVk("vkEndCommandBuffer(blit)", [&] { cmd.end(); });
    vk::CommandBuffer raw = *cmd;
    vk::SubmitInfo submit{};
    submit.sType = vk::StructureType::eSubmitInfo;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &raw;
    InvokeVk("vkQueueSubmit(blit)", [&] { queue_->submit(submit, *fence_); });
    CheckVk(device_->waitForFences(*fence_, vk::True, 2000000000u), "vkWaitForFences(blit 完了)");

    if (checkMarker) {
      void* mapped = nullptr;
      InvokeVk("vkMapMemory(blit)", [&] { mapped = stageMem_.mapMemory(0, kStageSize); });
      const uint8_t* p = static_cast<const uint8_t*>(mapped);
      // 左眼にマゼンタ、右眼にシアンが来ていること。逆なら失敗として数える。
      out.markerOk = (eye == 0) ? IsMagenta(p[0], p[1], p[2]) : IsCyan(p[0], p[1], p[2]);
      if (!out.markerOk) {
        Warn("左右分離検証: 眼 %u のマーカー不一致 (B=%u G=%u R=%u)", eye, p[0], p[1], p[2]);
      }
      stageMem_.unmapMemory();
    } else {
      out.markerOk = true;
    }

    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    CheckXr(xrReleaseSwapchainImage(res.swapchain, &release), "xrReleaseSwapchainImage");
    out.blitUs = NowUs() - t0;
    return out;
  }

  vk::Format format() const { return format_; }

 private:
  struct EyeRes {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    vk::Extent2D extent{0, 0};
    std::vector<vk::Image> images;  // XR 所有の非所有ビュー。破棄しない。
  };

  static constexpr vk::DeviceSize kStageSize = 4;  // マーカー 1 画素。

  const vk::raii::Device* device_;
  const vk::raii::Queue* queue_;
  vk::raii::CommandPool pool_{nullptr};
  std::vector<vk::raii::CommandBuffer> cmds_;
  vk::raii::Fence fence_{nullptr};
  vk::raii::Buffer stage_{nullptr};
  vk::raii::DeviceMemory stageMem_{nullptr};
  std::vector<EyeRes> eyes_;
  vk::Format format_ = vk::Format::eUndefined;
};

struct XrStats {
  uint64_t frames = 0;
  uint64_t rendered = 0;
  uint64_t requests = 0;
  uint64_t offers = 0;
  uint64_t doneOk = 0;
  uint64_t blits = 0;
  uint64_t blitUsTotal = 0;
  uint64_t blitUsMax = 0;
  uint64_t submitLayers = 0;
  uint64_t emptyFrames = 0;
  uint64_t verifyOk = 0;
  uint64_t verifyNg = 0;
  uint64_t firstUs = 0;
  uint64_t lastUs = 0;

  void Print(const RenderLedger::Stats& ledger) const {
    const double spanS =
        firstUs != 0 && lastUs > firstUs ? static_cast<double>(lastUs - firstUs) / 1e6 : 0.0;
    const double fps = spanS > 0.0 ? static_cast<double>(frames) / spanS : 0.0;
    Info("XR転送統計: フレーム=%llu 描画=%llu 要求=%llu 受信=%llu Done=%llu",
         static_cast<unsigned long long>(frames), static_cast<unsigned long long>(rendered),
         static_cast<unsigned long long>(requests), static_cast<unsigned long long>(offers),
         static_cast<unsigned long long>(doneOk));
    Info("XR転送統計: fps=%.2f（XR フレーム間隔からの算出）", fps);
    Info("XR転送統計: 受理=%llu 不明ID=%llu pose不一致=%llu 世代不一致=%llu 失効=%llu",
         static_cast<unsigned long long>(ledger.accepted),
         static_cast<unsigned long long>(ledger.unknownId),
         static_cast<unsigned long long>(ledger.poseMismatch),
         static_cast<unsigned long long>(ledger.spaceMismatch),
         static_cast<unsigned long long>(ledger.expired));
    const double denom = blits > 0 ? static_cast<double>(blits) : 1.0;
    Info("XR転送統計: blit=%llu 平均=%lluus 最大=%lluus 層あり=%llu 空=%llu",
         static_cast<unsigned long long>(blits),
         static_cast<unsigned long long>(blitUsTotal / denom),
         static_cast<unsigned long long>(blitUsMax),
         static_cast<unsigned long long>(submitLayers),
         static_cast<unsigned long long>(emptyFrames));
    Info("XR転送統計: 左右分離検証 ok=%llu ng=%llu",
         static_cast<unsigned long long>(verifyOk), static_cast<unsigned long long>(verifyNg));
  }
};

}  // namespace

int RunXrTransfer(const XrTransferOptions& options) {
  Info("XR転送開始: pipe=%s render-scale=%.2f submit-verify=%s", options.pipeName.c_str(),
       options.renderScale, options.submitVerify ? "on" : "off");
  if (options.injectStaleRecord > 0) {
    Info("検証フック有効: --inject-stale-record=%d（そのフレームの要求に旧 pose 混入の合図）",
         options.injectStaleRecord);
  }

  // 転送セッションを先に作る。同一 VkDevice を XR セッションと共有し、
  // 自前イメージから swapchain への blit を同一デバイス内で行う。
  // 別デバイスにするとデバイス間コピーが要るため、この順序を変えないこと。
  TransferSession::Options transfer_opts{};
  transfer_opts.verify = false;    // M1 の部分検証は行わない。M2 の照合で行う。
  transfer_opts.gpuCheck = false;  // M1b の全画面検査は行わない。制御メタのみ読む。
  TransferSession transfer(transfer_opts);
  xr::Session session(transfer.XrInstance(), transfer.Vulkan());
  gfx::Swapchains swapchains(transfer.XrInstance().handle(), session.handle(), session.views(),
                             gfx::M2PreferredSwapchainFormats());
  if (swapchains.format() != vk::Format::eB8G8R8A8Unorm) {
    Warn("swapchain が %s（受理画像と不一致。二重符号化の疑いが残る。DESIGN §9 の未決）",
         gfx::SwapchainFormatName(swapchains.format()));
  } else {
    Info("swapchain が B8G8R8A8_UNORM（受理画像と一致。blit は複写・拡大のみ）");
  }
  BlitPass blit(transfer.Vulkan(), swapchains);
  M2BlockReader reader(transfer.Vulkan());
  PipeClient pipe(options.pipeName);
  Info("パイプ接続: main プロセスと接続した");

  RenderLedger ledger(1, 1);
  XrStats stats{};
  XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerProjectionView proj_views[2] = {
      {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
      {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
  };
  layer.space = session.space();
  layer.viewCount = 2;
  layer.views = proj_views;
  std::vector<XrView> views(2, {XR_TYPE_VIEW});

  // 受理済みの最新画像。自前イメージが上書きされていない間のみ再利用できる。
  // 新規 Offer の到着で自前イメージは変わるため、その時点で無効化する。
  // 提出した画像は消費済みとし、古い画像の無限再利用は行わない。
  bool hasFresh = false;
  RenderRecord freshRecord{};
  uint32_t freshW = 0;
  uint32_t freshH = 0;

  bool session_running = false;
  bool quit = false;
  // パイプの生死と終了コード。切断（hard close）は新規受付の停止と統計付き終了の合図
  // であり（DESIGN §6③）、未完了の借用へ Done は送らない。プロトコルに終了合図が無い
  // ため、少なくともハングせず統計を残すことを優先し、正常な切断は 0、異常は非ゼロで返す。
  bool pipeAlive = true;
  int exitCode = 0;
  // 最後に Offer（有効・無効を問わず何らかの受信）があった時刻。初回受信前は idle 判定を
  // 行わない（起動直後の誤終了を避ける）。
  bool haveOfferEver = false;
  uint64_t lastOfferUs = NowUs();
  uint64_t frame_no = 0;
  uint64_t droppedIntermediate = 0;
  const uint64_t deadlineUs =
      options.seconds > 0 ? NowUs() + static_cast<uint64_t>(options.seconds) * 1000000u : 0;

  auto poll_events = [&]() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(transfer.XrInstance().handle(), &event) == XR_SUCCESS) {
      switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
          const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
          if (changed->session != session.handle()) {
            break;
          }
          session.setState(changed->state);
          Info("session 状態: %d", static_cast<int>(changed->state));
          switch (changed->state) {
            case XR_SESSION_STATE_READY:
              if (!session_running) {
                session.Begin();
                session_running = true;
                Info("セッション開始");
              }
              break;
            case XR_SESSION_STATE_STOPPING:
              if (session_running) {
                session.End();
                session_running = false;
                Info("セッション終了");
              }
              break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
              quit = true;
              break;
            default:
              break;
          }
          break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
          quit = true;
          break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
          // 適用時刻の追跡は行わず、受信時点で世代を進める。
          // 旧世代のレコードは即座に失効する（DESIGN §10）。
          ledger.AdvanceSpaceEpoch(ledger.spaceEpoch() + 1);
          hasFresh = false;
          Info("reference space 変更: spaceEpoch=%u へ進行し旧レコードを失効させた",
               ledger.spaceEpoch());
          break;
        default:
          break;
      }
      event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
  };

  // Offer の受け皿。最新 1 件だけ照合し、中間は Done だけ返す。
  // 中間画像の受理は行わない（最新優先の方針。破棄数は統計に出る）。
  // shouldRender が false のフレーム（SYNCHRONIZED 等）でも毎フレーム呼ぶこと。
  // 受信・ACK は描画可否と無関係に進め、止めるのは提出だけである。
  auto drain_offers = [&]() {
    TransferMessage msg{};
    bool have_last = false;
    for (;;) {
      bool timed_out = false;
      // ポーリング用の期限は「今 + 猶予」とする。現在時刻そのままでは
      // ReceiveWithDeadline 冒頭の期限到達判定が即 true になり ReadFile を
      // 一度も試さずに返るため、現在時刻をそのまま期限に渡さないこと。
      // 期限切れは返却許可ではなく、その回の受信の打ち切りにすぎない。
      // ここでの期限は受信のポーリング間隔である。
      const uint64_t poll_deadline = NowUs() + kOfferPollWaitUs;
      bool received = false;
      try {
        received = pipe.ReceiveWithDeadline(&msg, poll_deadline, &timed_out);
      } catch (const std::exception& e) {
        Error("パイプ受信に失敗: %s", e.what());
        pipeAlive = false;
        exitCode = 1;
        quit = true;
        return;
      }
      if (!received) {
        // 空振り（期限切れ）は正常であり、borrow 資源の返却許可にはならない。
        // 切断時は新規受付を止め、現在の処理を安全に終わらせて統計を出して終了する。
        // 未完了の借用へ Done は送らない（隔離。DESIGN §6③）。
        if (!timed_out) {
          Info("パイプ切断を検知したため新規受付を止めて終了する");
          pipeAlive = false;
          quit = true;
          return;
        }
        break;
      }
      // 何らかの受信があれば producer は生存している（有効・無効を問わない）。
      haveOfferEver = true;
      lastOfferUs = NowUs();
      if (!IsValidOffer(msg)) {
        Warn("不正な Offer を破棄（magic/version/type/format の不一致）");
        continue;
      }
      ++stats.offers;
      TransferSession::Timings timings{};
      const uint32_t status =
          transfer.ProcessOffer(msg.handle, msg.width, msg.height, &timings, nullptr);
      if (status == kStatusNoAck) {
        Error("GPU 完了未確認のため受付を停止して終了する（borrow への ACK なし）");
        pipeAlive = false;
        exitCode = 2;
        quit = true;
        return;
      }
      if (status == kStatusOk) {
        ++stats.doneOk;
      }
      const TransferMessage done = MakeDone(msg.borrowId, status);
      try {
        pipe.Send(done);
      } catch (const std::exception& e) {
        Error("ACK 送信に失敗: %s", e.what());
        pipeAlive = false;
        exitCode = 1;
        quit = true;
        return;
      }
      if (have_last) {
        ++droppedIntermediate;
      }
      have_last = true;
    }
    if (!have_last) {
      return;
    }
    // 最新 Offer の自前イメージから制御メタだけを読み、台帳と照合する。
    // 画像全体は読まない（DESIGN §1 の例外）。
    hasFresh = false;
    const vk::Image own = transfer.OwnImage();
    if (static_cast<VkImage>(own) == VK_NULL_HANDLE) {
      return;
    }
    uint16_t id16 = 0;
    uint16_t tag = 0;
    try {
      reader.Decode(own, &id16, &tag);
    } catch (const std::exception& e) {
      Warn("M2 ブロック readback に失敗: %s", e.what());
      return;
    }
    const RenderRecord* hit = nullptr;
    const RejectReason reason = ledger.Accept(id16, tag, ledger.spaceEpoch(), NowUs(), &hit);
    if (reason != RejectReason::Accepted || hit == nullptr) {
      Info("M2 棄却: %s（id16=0x%04x tag=0x%04x）", RejectReasonName(reason), id16, tag);
      return;
    }
    freshRecord = *hit;
    freshW = transfer.OwnWidth();
    freshH = transfer.OwnHeight();
    hasFresh = true;
  };

  while (!quit) {
    if (deadlineUs != 0 && NowUs() >= deadlineUs) {
      Info("指定秒数を経過したため終了する");
      break;
    }
    poll_events();
    if (!session_running) {
      // 実行中でない間も Offer の受付・ACK と切断検知は進める。受信した Offer は
      // drain_offers が Done を返すため、消費して返せなくなることはない。
      // 以前はここで切断を見ず --seconds か kill 待ちでぶら下がったため、
      // harness 先死で統計を出さずに殺されていた（DESIGN §6③の違反）。
      drain_offers();
      ledger.SweepExpired(NowUs());
      if (quit) {
        break;
      }
      if (haveOfferEver && NowUs() - lastOfferUs > kProducerIdleTimeoutUs) {
        Info("Offer が 5 秒途絶えたため producer 終了とみなして統計を出して終了する");
        break;
      }
      // 実行中でない間は眠らせ、ビジーループにしない。
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    if (stats.frames == 0) {
      stats.firstUs = NowUs();
    }
    ++frame_no;
    ++stats.frames;
    stats.lastUs = NowUs();

    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    CheckXr(xrWaitFrame(session.handle(), &wait_info, &frame_state), "xrWaitFrame");
    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    CheckXr(xrBeginFrame(session.handle(), &begin_info), "xrBeginFrame");

    bool rendered = false;
    if (frame_state.shouldRender == XR_TRUE) {
      XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
      locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
      locate_info.displayTime = frame_state.predictedDisplayTime;
      locate_info.space = session.space();
      XrViewState view_state{XR_TYPE_VIEW_STATE};
      uint32_t view_count = 0;
      CheckXr(xrLocateViews(session.handle(), &locate_info, &view_state,
                            static_cast<uint32_t>(views.size()), &view_count, views.data()),
              "xrLocateViews");
      const bool pose_valid =
          (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0 &&
          (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
      if (view_count == views.size() && pose_valid) {
        // 要求アトラス寸法は左右各眼の描画倍率から求める。swapchain 自体は
        // XR 推奨サイズのまま作る（VRAM 圧迫を避ける。既定 0.5）。
        const vk::Extent2D eye0 = swapchains.extent(0);
        const vk::Extent2D eye1 = swapchains.extent(1);
        const auto scaled = [](uint32_t v, double s) {
          return static_cast<uint32_t>(static_cast<double>(v) * s + 0.5);
        };
        const uint32_t req_w = scaled(eye0.width, options.renderScale) +
                               scaled(eye1.width, options.renderScale);
        const uint32_t half_h0 = scaled(eye0.height, options.renderScale);
        const uint32_t half_h1 = scaled(eye1.height, options.renderScale);
        const uint32_t req_h = half_h0 > half_h1 ? half_h0 : half_h1;
        const EyePose left = ToEyePose(views[0].pose);
        const EyePose right = ToEyePose(views[1].pose);
        const EyeFov lf = ToEyeFov(views[0].fov);
        const EyeFov rf = ToEyeFov(views[1].fov);
        const uint64_t render_id = frame_no;  // 0 を避けるため 1 起点の通番を使う。
        const RenderRecord& rec =
            ledger.Issue(render_id, left, right, lf, rf, req_w, req_h, NowUs());
        TransferMessage req = MakeRenderRequest(rec.renderId, rec.connEpoch, rec.spaceEpoch,
                                                rec.left, rec.right, rec.leftFov, rec.rightFov,
                                                req_w, req_h);
        if (options.injectStaleRecord > 0 &&
            frame_no == static_cast<uint64_t>(options.injectStaleRecord)) {
          req.reserved[0] = kStaleMarker;
        }
        try {
          pipe.Send(req);
          ++stats.requests;
        } catch (const std::exception& e) {
          Error("RenderRequest 送信に失敗: %s", e.what());
          pipeAlive = false;
          exitCode = 1;
          quit = true;
        }
        rendered = true;
      }
    }

    // Offer の受信・ACK は shouldRender が false のフレームでも毎フレーム行う
    //（SYNCHRONIZED 中でも Offer を受けて ACK できること）。提出だけを
    // shouldRender（rendered）で止める。drain_offers 内の期限切れは返却許可ではなく、
    // ここでの期限は受信のポーリング間隔である。
    drain_offers();
    ledger.SweepExpired(NowUs());
    // harness が Offer を止めたまま読取側を開き続けると（半閉鎖）、切断は返らず
    // 空フレームが続く。一定時間の途絶は producer 終了とみなして統計を出して終了し、
    // kill による統計消失を避ける。切断検知時と同様、現在のフレームは安全に終える。
    if (pipeAlive && !quit && haveOfferEver && NowUs() - lastOfferUs > kProducerIdleTimeoutUs) {
      Info("Offer が 5 秒途絶えたため producer 終了とみなして統計を出して終了する");
      pipeAlive = false;
      quit = true;
    }
    // 提出は受理画像のレコードの pose/FOV で行う。最新 pose への付け替えはしない。
    // 新規受理が無ければ layerCount=0 で終える（対応不明の画像の再投影頼みは禁止。
    // DESIGN §7）。受理から 500ms 超のレコードは失効として棄却する。
    XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
    end_info.displayTime = frame_state.predictedDisplayTime;
    end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
    bool submit_layer = false;
    if (rendered && hasFresh) {
      const uint64_t age = NowUs() - freshRecord.issuedUs;
      if (age <= kRenderRecordMaxAgeUs) {
        try {
          const vk::Image own = transfer.OwnImage();
          const BlitPass::Result r0 = blit.Blit(0, own, freshW, freshH, 0, options.submitVerify);
          const BlitPass::Result r1 = blit.Blit(1, own, freshW, freshH, 1, options.submitVerify);
          const uint64_t blit_us = r0.blitUs + r1.blitUs;
          ++stats.blits;
          stats.blitUsTotal += blit_us;
          if (blit_us > stats.blitUsMax) {
            stats.blitUsMax = blit_us;
          }
          if (options.submitVerify) {
            if (r0.markerOk && r1.markerOk) {
              ++stats.verifyOk;
            } else {
              ++stats.verifyNg;
            }
          }
          for (uint32_t eye = 0; eye < 2; ++eye) {
            const EyePose& p = (eye == 0) ? freshRecord.left : freshRecord.right;
            const EyeFov& f = (eye == 0) ? freshRecord.leftFov : freshRecord.rightFov;
            proj_views[eye].pose.position.x = p.position[0];
            proj_views[eye].pose.position.y = p.position[1];
            proj_views[eye].pose.position.z = p.position[2];
            proj_views[eye].pose.orientation.x = p.orientation[0];
            proj_views[eye].pose.orientation.y = p.orientation[1];
            proj_views[eye].pose.orientation.z = p.orientation[2];
            proj_views[eye].pose.orientation.w = p.orientation[3];
            proj_views[eye].fov.angleLeft = f.left;
            proj_views[eye].fov.angleRight = f.right;
            proj_views[eye].fov.angleUp = f.up;
            proj_views[eye].fov.angleDown = f.down;
            proj_views[eye].subImage.swapchain = swapchains.handle(eye);
            proj_views[eye].subImage.imageRect.offset = {0, 0};
            proj_views[eye].subImage.imageRect.extent.width =
                static_cast<int32_t>(swapchains.extent(eye).width);
            proj_views[eye].subImage.imageRect.extent.height =
                static_cast<int32_t>(swapchains.extent(eye).height);
            proj_views[eye].subImage.imageArrayIndex = 0;
          }
          Info("M2 提出: renderId=%llu 年齢=%lluus（画像対応レコードの pose/FOV）",
               static_cast<unsigned long long>(freshRecord.renderId),
               static_cast<unsigned long long>(age));
          submit_layer = true;
          hasFresh = false;
        } catch (const std::exception& e) {
          Warn("blit に失敗したため空フレームで終える: %s", e.what());
        }
      } else {
        hasFresh = false;
      }
    }
    if (submit_layer) {
      end_info.layerCount = 1;
      end_info.layers = layers;
      ++stats.rendered;
      ++stats.submitLayers;
    } else {
      end_info.layerCount = 0;
      ++stats.emptyFrames;
    }
    CheckXr(xrEndFrame(session.handle(), &end_info), "xrEndFrame");
  }

  stats.Print(ledger.stats());
  Info("M2 中間破棄: %llu（最新優先のため Done のみ返した件数）",
       static_cast<unsigned long long>(droppedIntermediate));
  if (stats.submitLayers == 0) {
    Warn("提出層 0 のまま終了した（受理画像が無い。harness 側の描画を確認すること）");
  }
  if (!pipeAlive) {
    Info("パイプ終了に伴う終了（exit=%d）", exitCode);
  }
  return exitCode;
}

}  // namespace xrift::transfer

#else

#include <string>

#include "common/log.h"
#include "transfer/xr_transfer_app.h"

namespace xrift::transfer {

int RunXrTransfer(const XrTransferOptions&) {
  Error("--xr-transfer は Windows のみ（Linux は将来対応）");
  return 1;
}

}  // namespace xrift::transfer

#endif
