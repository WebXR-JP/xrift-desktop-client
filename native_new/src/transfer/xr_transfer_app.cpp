#include "transfer/xr_transfer_app.h"

#ifdef _WIN32

#include <windows.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "common/log.h"
#include "common/xr_result.h"
#include "gfx/swapchains.h"
#include "transfer/mem_log.h"
#include "transfer/input_state.h"
#include "transfer/m2_blocks.h"
#include "transfer/pipe_client.h"
#include "transfer/protocol.h"
#include "transfer/render_record.h"
#include "transfer/shared_texture.h"
#include "xr/input.h"
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
// --pace-records の応答停滞の上限（500ms は意図的な値であり、仮置きではない）。
// 未処理の画像がある間は新しいレコードを発行せず待ち、500ms 応答が無ければ
// 同じレコードの再送ではなく新しいレコードを発行して詰まりを解く。
// 古いレコードは TTL と掃除で失効させる。500ms は OSR の実測遅延（350〜500ms）の
// 上端に合わせ、通常の paint 間隔（約 11ms・低速時約 135ms）より十分長い。
constexpr uint64_t kPaceStallTimeoutUs = 500000u;
// Offer 途絶で producer 終了とみなす無受信の上限の既定（5000ms は意図的な値）。
// harness は計測終了で Offer を止めてから読取側を開いたままにし（socket.end の半閉鎖）、
// 8 秒の猶予後に kill する。exe が受信待ちを続けると統計を出す前に殺されるため、
// 通常時（約 90Hz で Offer が来る）では起きない 5 秒の途絶で新規受付を止めて終了する。
// 初回 Offer 前は適用しない（起動直後の誤終了を避ける）。
// 実行値は options.producerIdleTimeoutMs で上書きし、0 は無効（パイプ切断でのみ終了）。

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

// 検証用の提出画像ダンプ（PPM P6 保存）。blit 後の swapchain 全体を host-visible バッファ
// から P6（RGB）に変換して保存する。4 バイト画素の並びは swapchain 形式に従う
// （B8G8R8A8 系は B,G,R,A 順、R8G8B8A8 系は R,G,B,A 順）。提出の正しさには触らない
// （dump-submit が空なら呼ばない。製品経路の挙動は変えない）。
bool WriteEyePpm(const std::string& path, const uint8_t* pixels, uint32_t w, uint32_t h,
                 vk::Format format) {
  std::error_code ec;
  const std::filesystem::path out(path);
  if (out.has_parent_path()) {
    std::filesystem::create_directories(out.parent_path(), ec);
  }
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    Warn("dump-submit: %s を開けない", path.c_str());
    return false;
  }
  file << "P6\n" << w << " " << h << "\n255\n";
  if (!file) {
    Warn("dump-submit: %s のヘッダ書き込みに失敗", path.c_str());
    return false;
  }
  const bool bgr = (format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb);
  std::string row;
  row.resize(static_cast<size_t>(w) * 3);
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* src = pixels + static_cast<size_t>(y) * w * 4;
    for (uint32_t x = 0; x < w; ++x) {
      const uint8_t c0 = src[x * 4 + 0];
      const uint8_t c1 = src[x * 4 + 1];
      const uint8_t c2 = src[x * 4 + 2];
      // BGR 系は R=c2、RGB 系は R=c0 である。
      row[x * 3 + 0] = static_cast<char>(bgr ? c2 : c0);
      row[x * 3 + 1] = static_cast<char>(c1);
      row[x * 3 + 2] = static_cast<char>(bgr ? c0 : c2);
    }
    file.write(row.data(), static_cast<std::streamsize>(row.size()));
    if (!file) {
      Warn("dump-submit: %s の画素書き込みに失敗（行 %u）", path.c_str(), y);
      return false;
    }
  }
  file.close();
  return true;
}

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

// 最後に受理したアトラスの exe 所有の複製。新規受理が無いフレームでも同じ画像を
// 出し直すために保つ（黒フレーム防止）。CPU へは戻さない。
// 自前イメージは次の Offer で上書き・棄却画像で汚染されるため、受理確定時に
// GPU コピーで複製する。再提出はこの複製から swapchain へ blit し、
// pose/FOV は受理レコードのものをそのまま使う（付け替えはしない）。
// 寸法変化時は作り直す。世代進行（接続・空間）では呼び出し側が Invalidate する。
class HoldImage {
 public:
  explicit HoldImage(const gfx::VulkanContext& vk)
      : device_(&vk.device()),
        physical_(&vk.physicalDevice()),
        queue_(&vk.queue()),
        qfi_(vk.queueFamilyIndex()) {
    vk::CommandPoolCreateInfo pool_info{};
    pool_info.sType = vk::StructureType::eCommandPoolCreateInfo;
    pool_info.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    pool_info.queueFamilyIndex = qfi_;
    InvokeVk("vkCreateCommandPool(hold)", [&] { pool_ = vk::raii::CommandPool(*device_, pool_info); });
    vk::CommandBufferAllocateInfo alloc{};
    alloc.sType = vk::StructureType::eCommandBufferAllocateInfo;
    alloc.commandPool = *pool_;
    alloc.level = vk::CommandBufferLevel::ePrimary;
    alloc.commandBufferCount = 1;
    InvokeVk("vkAllocateCommandBuffers(hold)", [&] {
      vk::raii::CommandBuffers bufs = device_->allocateCommandBuffers(alloc);
      cmd_ = std::move(bufs[0]);
    });
    vk::FenceCreateInfo fence_info{};
    fence_info.sType = vk::StructureType::eFenceCreateInfo;
    InvokeVk("vkCreateFence(hold)", [&] { fence_ = vk::raii::Fence(*device_, fence_info); });
  }

  ~HoldImage() {
    if (imageBytes_ > 0) {
      TrackVkImageDestroy(imageBytes_);
      imageBytes_ = 0;
    }
    if (memoryBytes_ > 0) {
      TrackVkMemoryFree(memoryBytes_);
      memoryBytes_ = 0;
    }
  }

  HoldImage(const HoldImage&) = delete;
  HoldImage& operator=(const HoldImage&) = delete;

  bool Valid() const {
    return valid_ && static_cast<VkImage>(*image_) != VK_NULL_HANDLE && width_ > 0 &&
           height_ > 0;
  }
  void Invalidate() { valid_ = false; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  vk::Image image() const { return *image_; }

  // 自前イメージ（GENERAL 駐め）全体を複製する。両方とも GENERAL で返す。
  // 自前イメージの不変条件（GENERAL 駐め）を壊さないことが呼び出し条件である。
  // 失敗時は例外にし、valid は偽にする（半端な複製の再提出はしない）。
  void SaveFrom(vk::Image own, uint32_t w, uint32_t h) {
    if (static_cast<VkImage>(own) == VK_NULL_HANDLE || w == 0 || h == 0) {
      throw std::runtime_error("hold 複製の元画像・寸法が無効です");
    }
    Ensure(w, h);
    InvokeVk("vkResetFences(hold)", [&] { device_->resetFences(*fence_); });
    InvokeVk("vkResetCommandBuffer(hold)", [&] { cmd_.reset(); });
    vk::CommandBufferBeginInfo begin{};
    begin.sType = vk::StructureType::eCommandBufferBeginInfo;
    begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    InvokeVk("vkBeginCommandBuffer(hold)", [&] { cmd_.begin(begin); });

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
    cmd_.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                         vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr, parks);

    InvokeVk("vkEndCommandBuffer(hold)", [&] { cmd_.end(); });
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

 private:
  void Ensure(uint32_t w, uint32_t h) {
    if (defined_ && w == width_ && h == height_) {
      return;
    }
    if (imageBytes_ > 0) {
      TrackVkImageDestroy(imageBytes_);
      imageBytes_ = 0;
    }
    if (memoryBytes_ > 0) {
      TrackVkMemoryFree(memoryBytes_);
      memoryBytes_ = 0;
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
    InvokeVk("vkCreateImage(hold)", [&] { image_ = vk::raii::Image(*device_, info); });
    const vk::MemoryRequirements req = image_.getMemoryRequirements();
    imageBytes_ = req.size;
    TrackVkImageCreate(req.size);
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
    InvokeVk("vkAllocateMemory(hold)", [&] { memory_ = vk::raii::DeviceMemory(*device_, alloc); });
    memoryBytes_ = alloc.allocationSize;
    TrackVkMemoryAlloc(alloc.allocationSize);
    InvokeVk("vkBindImageMemory(hold)", [&] { image_.bindMemory(*memory_, 0); });
    width_ = w;
    height_ = h;
  }

  const vk::raii::Device* device_;
  const vk::raii::PhysicalDevice* physical_;
  const vk::raii::Queue* queue_;
  uint32_t qfi_ = 0;
  vk::raii::CommandPool pool_{nullptr};
  vk::raii::CommandBuffer cmd_{nullptr};
  vk::raii::Fence fence_{nullptr};
  vk::raii::Image image_{nullptr};
  vk::raii::DeviceMemory memory_{nullptr};
  uint64_t imageBytes_ = 0;
  uint64_t memoryBytes_ = 0;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  bool defined_ = false;
  bool valid_ = false;
};

// 受理画像の提出パス。acquire → blit → マーカー確認 → release を 1 眼単位で行う。
class BlitPass {
 public:
  BlitPass(const gfx::VulkanContext& vk, const gfx::Swapchains& swapchains,
           const std::string& dumpDir = "")
      : device_(&vk.device()), queue_(&vk.queue()), dumpDir_(dumpDir) {
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
    // 検証用の提出画像ダンプ（--dump-submit が空なら確保しない。通常経路の挙動は変えない）。
    // 各眼の実寸ぶんの host-visible バッファを一時確保し、終了時（デストラクタ）に解放する。
    if (!dumpDir_.empty()) {
      dumpStage_.reserve(eyes_.size());
      dumpMem_.reserve(eyes_.size());
      dumpSize_.assign(eyes_.size(), 0);
      for (size_t e = 0; e < eyes_.size(); ++e) {
        const uint64_t pixels = static_cast<uint64_t>(eyes_[e].extent.width) *
                                static_cast<uint64_t>(eyes_[e].extent.height);
        const vk::DeviceSize bytes = static_cast<vk::DeviceSize>(pixels * 4);
        if (bytes == 0) {
          continue;
        }
        try {
          vk::raii::Buffer buf{nullptr};
          vk::raii::DeviceMemory mem{nullptr};
          AllocateHostBuffer(vk, &buf, &mem, bytes);
          dumpStage_.push_back(std::move(buf));
          dumpMem_.push_back(std::move(mem));
          dumpSize_[e] = bytes;
        } catch (const std::exception& ex) {
          Warn("dump-submit: 眼 %u の確保に失敗したためダンプを無効化: %s",
               static_cast<unsigned>(e), ex.what());
          dumpDir_.clear();
          break;
        }
      }
      if (!dumpDir_.empty()) {
        Info("dump-submit: ダンプ先 %s（各眼の実寸ぶんを一時確保した）", dumpDir_.c_str());
      }
    }
  }

  // ダンプが有効か（確保まで成功したか）。確保失敗時は false になる。
  bool dumpEnabled() const {
    return !dumpDir_.empty() && dumpStage_.size() == eyes_.size();
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
  // dump が真のときは release 前に swapchain 全体を host-visible バッファへ写し、
  // COLOR_ATTACHMENT_OPTIMAL に戻してから PPM 保存する（提出の正しさを壊さない）。
  // 失敗時は例外にする（呼び出し側は layerCount=0 で終える）。
  Result Blit(uint32_t eye, vk::Image own, uint32_t ownW, uint32_t ownH, uint32_t srcHalf,
              bool checkMarker, bool dump = false) {
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

    // 検証用の提出画像ダンプ。blit 後・release 前に swapchain 全体を写し、
    // 提出前に COLOR_ATTACHMENT_OPTIMAL へ戻す（提出の正しさを壊さない）。
    // dump-submit が空の通常経路では記録しない。
    const bool do_dump =
        dump && dumpEnabled() && eye < dumpStage_.size() && dumpSize_[eye] != 0 &&
        static_cast<VkBuffer>(*dumpStage_[eye]) != VK_NULL_HANDLE;
    if (do_dump) {
      vk::ImageMemoryBarrier to_dump_src{};
      to_dump_src.sType = vk::StructureType::eImageMemoryBarrier;
      to_dump_src.dstAccessMask = vk::AccessFlagBits::eTransferRead;
      to_dump_src.oldLayout = vk::ImageLayout::eColorAttachmentOptimal;
      to_dump_src.newLayout = vk::ImageLayout::eTransferSrcOptimal;
      to_dump_src.image = dst;
      to_dump_src.subresourceRange = range;
      cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                          vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr, to_dump_src);
      vk::BufferImageCopy full{};
      full.bufferOffset = 0;
      full.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
      full.imageSubresource.layerCount = 1;
      full.imageOffset = vk::Offset3D{0, 0, 0};
      full.imageExtent =
          vk::Extent3D{res.extent.width, res.extent.height, 1};
      cmd.copyImageToBuffer(dst, vk::ImageLayout::eTransferSrcOptimal, *dumpStage_[eye], full);
      vk::BufferMemoryBarrier dump_host{};
      dump_host.sType = vk::StructureType::eBufferMemoryBarrier;
      dump_host.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
      dump_host.dstAccessMask = vk::AccessFlagBits::eHostRead;
      dump_host.buffer = *dumpStage_[eye];
      dump_host.offset = 0;
      dump_host.size = dumpSize_[eye];
      cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
                          {}, nullptr, dump_host, nullptr);
      vk::ImageMemoryBarrier back = to_dump_src;
      back.srcAccessMask = vk::AccessFlagBits::eTransferRead;
      back.dstAccessMask = {};
      back.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
      back.newLayout = vk::ImageLayout::eColorAttachmentOptimal;
      cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                          vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, nullptr, nullptr,
                          back);
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

    if (do_dump) {
      // release 前に写した全体を PPM 保存する。失敗しても提出は続ける。
      void* dmapped = nullptr;
      bool mapped = false;
      try {
        InvokeVk("vkMapMemory(blit-dump)", [&] {
          dmapped = dumpMem_[eye].mapMemory(0, dumpSize_[eye]);
        });
        mapped = true;
        const std::string name = (eye == 0) ? "eye0.ppm" : "eye1.ppm";
        const std::string path = (std::filesystem::path(dumpDir_) / name).string();
        const uint8_t* dpixels = static_cast<const uint8_t*>(dmapped);
        if (WriteEyePpm(path, dpixels, res.extent.width, res.extent.height, format_)) {
          Info("dump-submit: 眼 %u を %s に保存（%ux%u）", eye, path.c_str(),
               static_cast<unsigned>(res.extent.width),
               static_cast<unsigned>(res.extent.height));
        }
      } catch (const std::exception& ex) {
        Warn("dump-submit: 眼 %u の保存に失敗: %s", eye, ex.what());
      }
      if (mapped) {
        dumpMem_[eye].unmapMemory();
      }
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
  // 検証用の提出画像ダンプ先と各眼の host-visible バッファ（空なら無効）。
  // blit と同じ submit 内で写し、release 前に PPM 保存する。終了時に解放する。
  std::string dumpDir_;
  std::vector<vk::raii::Buffer> dumpStage_;
  std::vector<vk::raii::DeviceMemory> dumpMem_;
  std::vector<vk::DeviceSize> dumpSize_;
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
  uint64_t resubmits = 0;  // 新規受理が無いフレームで hold から出し直した提出数。
  uint64_t pacedWaits = 0;  // --pace-records で未処理待ちのため発行を見送ったフレーム数。
  uint64_t verifyOk = 0;
  uint64_t verifyNg = 0;
  uint64_t firstUs = 0;
  uint64_t lastUs = 0;
  // M4: 入力・haptics・空間・再生成の統計。
  uint64_t inputSent = 0;       // main へ送った InputState の件数。
  uint64_t hapticReceived = 0;  // main から受けた HapticCommand の件数。
  uint64_t spaceChanges = 0;    // spaceEpoch を進めた回数。
  uint64_t restarts = 0;        // セッション再生成の成功回数。
  uint64_t restartAttempts = 0;  // セッション再生成の試行回数。
  uint32_t connEpoch = 1;       // 現在の接続世代。

  void Print(const RenderLedger::Stats& ledger, const xr::InputStats& input) const {
    const double spanS =
        firstUs != 0 && lastUs > firstUs ? static_cast<double>(lastUs - firstUs) / 1e6 : 0.0;
    const double fps = spanS > 0.0 ? static_cast<double>(frames) / spanS : 0.0;
    Info("XR転送統計: フレーム=%llu 描画=%llu 要求=%llu 受信=%llu Done=%llu",
         static_cast<unsigned long long>(frames), static_cast<unsigned long long>(rendered),
         static_cast<unsigned long long>(requests), static_cast<unsigned long long>(offers),
         static_cast<unsigned long long>(doneOk));
    Info("XR転送統計: fps=%.2f（XR フレーム間隔からの算出）", fps);
    Info("XR転送統計: 発行レコード=%llu 追い出し=%llu pace待機フレーム=%llu",
         static_cast<unsigned long long>(ledger.issued),
         static_cast<unsigned long long>(ledger.evicted),
         static_cast<unsigned long long>(pacedWaits));
    Info("XR転送統計: 受理=%llu 不明ID=%llu pose不一致=%llu 世代不一致=%llu 失効=%llu",
         static_cast<unsigned long long>(ledger.accepted),
         static_cast<unsigned long long>(ledger.unknownId),
         static_cast<unsigned long long>(ledger.poseMismatch),
         static_cast<unsigned long long>(ledger.spaceMismatch),
         static_cast<unsigned long long>(ledger.expired));
    const double denom = blits > 0 ? static_cast<double>(blits) : 1.0;
    Info("XR転送統計: blit=%llu 平均=%lluus 最大=%lluus 層あり=%llu 再提出=%llu 空=%llu",
         static_cast<unsigned long long>(blits),
         static_cast<unsigned long long>(blitUsTotal / denom),
         static_cast<unsigned long long>(blitUsMax),
         static_cast<unsigned long long>(submitLayers),
         static_cast<unsigned long long>(resubmits),
         static_cast<unsigned long long>(emptyFrames));
    Info("XR転送統計: 左右分離検証 ok=%llu ng=%llu",
         static_cast<unsigned long long>(verifyOk), static_cast<unsigned long long>(verifyNg));
    Info("M4統計: connEpoch=%u InputState送信=%llu haptic受信=%llu space変更=%llu",
         connEpoch, static_cast<unsigned long long>(inputSent),
         static_cast<unsigned long long>(hapticReceived),
         static_cast<unsigned long long>(spaceChanges));
    Info("M4統計: セッション再生成 成功=%llu 試行=%llu",
         static_cast<unsigned long long>(restarts),
         static_cast<unsigned long long>(restartAttempts));
    Info("M4統計: 入力 sync=%llu 失敗=%llu snapshot=%llu profile変化=%llu",
         static_cast<unsigned long long>(input.syncs),
         static_cast<unsigned long long>(input.syncFailed),
         static_cast<unsigned long long>(input.snapshots),
         static_cast<unsigned long long>(input.profileChanges));
    Info("M4統計: haptics 成功=%llu 未送信(inactive等)=%llu 失敗=%llu",
         static_cast<unsigned long long>(input.hapticOk),
         static_cast<unsigned long long>(input.hapticInactive),
         static_cast<unsigned long long>(input.hapticFailed));
  }
};

}  // namespace

int RunXrTransfer(const XrTransferOptions& options) {
  Info("XR転送開始: pipe=%s render-scale=%.2f record-ttl=%llums submit-verify=%s space=%s pace=%s",
       options.pipeName.c_str(), options.renderScale,
       static_cast<unsigned long long>(options.recordTtlMs),
       options.submitVerify ? "on" : "off", options.space.c_str(),
       options.paceRecords ? "on" : "off");
  if (options.resubmit) {
    if (options.resubmitMaxAgeMs == 0) {
      Info("再提出 on（新規受理が無いフレームは最終受理を同じ pose/FOV で出す。年齢無制限）");
    } else {
      Info("再提出 on（新規受理が無いフレームは最終受理を同じ pose/FOV で出す。上限 %llums）",
           static_cast<unsigned long long>(options.resubmitMaxAgeMs));
    }
  } else {
    Info("再提出 off（--no-resubmit。旧挙動の空フレーム比較用）");
  }
  // ADR: Offer 途絶の終了判定は実行値で決める。0 は app 経由の通常運用（静止ページで
  // paint が止まっても終了しない）のため無効とし、既定 5000 はハーネス向けに保つ。
  const uint64_t idleTimeoutUs = options.producerIdleTimeoutMs <= 0
                                     ? 0u
                                     : static_cast<uint64_t>(options.producerIdleTimeoutMs) * 1000u;
  if (idleTimeoutUs == 0u) {
    Info("producer idle タイムアウト無効（--producer-idle-timeout-ms=0。パイプ切断でのみ終了）");
  } else {
    Info("producer idle タイムアウト %dms（途絶で producer 終了とみなす）",
         options.producerIdleTimeoutMs);
  }
  if (options.injectStaleRecord > 0) {
    Info("検証フック有効: --inject-stale-record=%d（そのフレームの要求に旧 pose 混入の合図）",
         options.injectStaleRecord);
  }
  if (options.injectSessionRestartMs > 0) {
    Info("検証フック有効: --inject-session-restart=%d（起動からその ms 後に意図的にセッションを"
         "破棄・再生成する）",
         options.injectSessionRestartMs);
  }
  if (options.injectSpaceChangeMs > 0) {
    Info("検証フック有効: --inject-space-change=%d（起動からその ms 後に模擬の space 変更を"
         "1 回適用する。実イベントではない）",
         options.injectSpaceChangeMs);
  }
  if (!options.dumpSubmitDir.empty()) {
    Info("検証フック有効: --dump-submit=%s --dump-frame=%d（その回目の提出時に両眼を PPM 保存）",
         options.dumpSubmitDir.c_str(), options.dumpFrame);
  }
  // 終了段階ログ（--dump-exit-stage / XRIFT_EXIT_STAGE）。破棄順の切り分け用。
  // 既定 off。通常経路の出力は変えない。Info は行ごとに flush するため、
  // 異常終了時も最後に出た段階まで残る。
  // 環境変数の有無は GetEnvironmentVariableA で見る（getenv の警告を避ける）。
  bool stage_log = options.dumpExitStage;
  if (!stage_log) {
    stage_log = ::GetEnvironmentVariableA("XRIFT_EXIT_STAGE", nullptr, 0) != 0;
  }
  auto stage = [&](const char* what) {
    if (stage_log) {
      Info("終了段階: %s", what);
    }
  };
  if (stage_log) {
    Info("終了段階ログ有効（--dump-exit-stage / XRIFT_EXIT_STAGE）");
  }
  // ADR: 終了時の非決定的クラッシュ（実測: 統計出力後の残り破棄で 0xC0000005・
  // 0xC0000409）は、明示 teardown 後に残る RAII・静的デストラクタのいずれかで
  // 起きる。必要な後始末（セッション終了・パイプ・統計 flush）は統計出力時点で
  // 済んでいるため、以後は破棄を走らせず即時終了する（既定 on）。
  // Info/Error は行ごとに flush するが、_Exit 前に念のため両方を flush する。
  // --no-quick-exit では従来の RAII 破棄を残す（切り分け用）。
  auto quick_exit = [&](int code) {
    Info("終了: 残りの破棄を省略して終了する（--no-quick-exit で旧経路）");
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(code);
  };

  // 転送セッションを先に作る。同一 VkDevice を XR セッションと共有し、
  // 自前イメージから swapchain への blit を同一デバイス内で行う。
  // 別デバイスにするとデバイス間コピーが要るため、この順序を変えないこと。
  TransferSession::Options transfer_opts{};
  transfer_opts.verify = false;    // M1 の部分検証は行わない。M2 の照合で行う。
  transfer_opts.gpuCheck = false;  // M1b の全画面検査は行わない。制御メタのみ読む。
  TransferSession transfer(transfer_opts);
  xr::SpacePreference space_pref = xr::SpacePreference::Auto;
  try {
    space_pref = xr::ParseSpacePreference(options.space);
  } catch (const std::exception& e) {
    Error("%s", e.what());
    return 1;
  }
  if (options.injectSessionRestartMs < 0) {
    Error("--inject-session-restart は 0 以上であること");
    return 1;
  }
  if (options.injectSpaceChangeMs < 0) {
    Error("--inject-space-change は 0 以上であること");
    return 1;
  }

  // セッション束（セッション・参照空間・swapchain・blit・入力空間）。
  // LOSS_PENDING や --inject-session-restart ではこの束だけを破棄・再生成する。
  // TransferSession（instance・Vulkan device）・M2BlockReader・PipeClient は保つ。
  // 再生成のたびに connEpoch を進め、旧世代のレコードを失効させる（DESIGN §6・§10）。
  std::unique_ptr<xr::Session> session;
  std::unique_ptr<gfx::Swapchains> swapchains;
  std::unique_ptr<BlitPass> blit;
  xr::InputManager input;
  uint32_t conn_epoch = 1;
  RenderLedger ledger(conn_epoch, 1, options.recordTtlMs * 1000u);
  XrStats stats{};
  stats.connEpoch = conn_epoch;
  try {
    session = std::make_unique<xr::Session>(transfer.XrInstance(), transfer.Vulkan(), space_pref);
  } catch (const std::exception& e) {
    Error("XR セッションの生成に失敗: %s", e.what());
    // 生成失敗時も統計（空）とメッセージは残す。以後の RAII 破棄は
    // quick-exit で飛ばす（終了時クラッシュの回避。正常経路と同一の出口）。
    stats.Print(ledger.stats(), input.stats());
    if (options.quickExit) {
      quick_exit(1);
    }
    return 1;
  }
  try {
    input.Create(transfer.XrInstance().handle());
  } catch (const std::exception& e) {
    Error("入力アクションの生成に失敗: %s", e.what());
    stats.Print(ledger.stats(), input.stats());
    if (options.quickExit) {
      quick_exit(1);
    }
    return 1;
  }
  if (!input.AttachSpaces(session->handle())) {
    Warn("入力アクションの attach に失敗したため入力は inactive のまま続行する");
  }
  try {
    swapchains = std::make_unique<gfx::Swapchains>(transfer.XrInstance().handle(),
                                                   session->handle(), session->views(),
                                                   gfx::M2PreferredSwapchainFormats());
  } catch (const std::exception& e) {
    Error("swapchain の生成に失敗: %s", e.what());
    stats.Print(ledger.stats(), input.stats());
    if (options.quickExit) {
      quick_exit(1);
    }
    return 1;
  }
  if (swapchains->format() != vk::Format::eB8G8R8A8Unorm) {
    Warn("swapchain が %s（受理画像と不一致。二重符号化の疑いが残る。DESIGN §9 の未決）",
         gfx::SwapchainFormatName(swapchains->format()));
  } else {
    Info("swapchain が B8G8R8A8_UNORM（受理画像と一致。blit は複写・拡大のみ）");
  }
  blit = std::make_unique<BlitPass>(transfer.Vulkan(), *swapchains, options.dumpSubmitDir);
  M2BlockReader reader(transfer.Vulkan());
  HoldImage hold(transfer.Vulkan());
  // 接続失敗は例外で返る。統計とメッセージを残してから投げ直す
  // （呼び出し側は 1 に丸めて通常終了させる）。
  std::unique_ptr<PipeClient> pipe_owned;
  try {
    pipe_owned = std::make_unique<PipeClient>(options.pipeName);
  } catch (const std::exception& e) {
    Error("パイプ接続に失敗: %s", e.what());
    stats.Print(ledger.stats(), input.stats());
    throw;
  }
  // 以後は参照で使う（呼び出し側の記述は変えない。破棄順序も変わらない）。
  PipeClient& pipe = *pipe_owned;
  Info("パイプ接続: main プロセスと接続した");

  XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerProjectionView proj_views[2] = {
      {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
      {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
  };
  layer.space = session->space();
  layer.viewCount = 2;
  layer.views = proj_views;
  std::vector<XrView> views(2, {XR_TYPE_VIEW});

  // 受理済みの最新画像。受理確定時に hold へ GPU 複製し、以後は hold から出し直す。
  // 自前イメージは次の Offer で上書き・棄却画像で汚染されるため、提出の複製元には使わない。
  // 再提出は同じ record の pose/FOV のまま行い、最新 pose への付け替えはしない。
  bool hasFresh = false;
  RenderRecord freshRecord{};
  uint32_t freshW = 0;
  uint32_t freshH = 0;
  // 再提出用の保持。hold イメージと、その画像に対応する受理レコードの複写である。
  // 台帳の掃除・TTL 失効では消さない（黒より古い絵を優先する）。世代進行では消す。
  RenderRecord holdRecord{};
  bool holdHasRecord = false;
  // pose 不一致診断の出力済み件数。先頭 5 件だけ詳報し、以後は集計のみにする。
  uint64_t mismatchLogCount = 0;
  // 再提出ログの前回出力時刻。毎フレーム出すと膨大になるため 1 秒に抑える。
  uint64_t lastResubmitLogUs = 0;
  // 検証用の提出画像ダンプの状態。N 回目の提出（両眼 blit 成功で 1）に 1 回だけ保存する。
  // セッション再生成で blit が作り直されても提出回数は保つ（回数は提出基準である）。
  uint64_t submits = 0;
  bool dumpDone = false;

  bool session_running = false;
  bool quit = false;
  // M4: セッション再生成の状態。LOSS_PENDING で束を破棄し、1 秒周期で再生成を試みる。
  // 試行上限（30 回≒30 秒）を超えたらランタイム未復帰として安全に終了する（統計出力）。
  // 上限 30・間隔 1 秒はいずれも意図的な値であり、仮置きではない。
  bool needs_rebuild = false;
  int rebuild_attempts_left = 0;
  uint64_t last_rebuild_try_us = 0;
  // REFERENCE_SPACE_CHANGE_PENDING の受付。適用はフレーム側で行う。
  bool pending_space_change = false;
  // --inject-space-change の発火済み合図（1 回だけ送る）と模擬の区別。
  // 模擬もランタイム由来と同じ適用経路（空間作り直し・spaceEpoch 更新・旧レコード失効）を
  // 通すが、実イベントではないことをログに明記し、統計の space変更には計上する。
  bool space_change_injected = false;
  bool pending_space_injected = false;
  // locate 確認に使う予測表示時刻（直近の xrWaitFrame の値）。
  // 時刻 0 を渡すと受け付けないランタイムがある（実測: SteamVR は
  // XR_ERROR_TIME_INVALID）ため、running 中のフレーム時刻を使う。
  XrTime last_display_time = 0;
  // --inject-session-restart の発火済み合図（1 回だけ送る）。
  bool restart_injected = false;
  const uint64_t start_us = NowUs();
  // M4 の毎秒入力ログの前回出力時刻。0 は未出力である。
  uint64_t last_input_log_us = 0;
  // M4計測: 押下エッジ用の前フレームと 1 秒ラッチ（左右のボタン bit）。
  // ラッチは 1 秒サマリで 0 へ戻す。製品経路の buttons 欄は変えない。
  xr::InputSnapshot prev_snapshot{};
  bool have_prev_snapshot = false;
  uint8_t input_latch[2] = {0, 0};
  // パイプの生死と終了コード。切断（hard close）は新規受付の停止と統計付き終了の合図
  // であり（DESIGN §6③）、未完了の借用へ Done は送らない。プロトコルに終了合図が無い
  // ため、少なくともハングせず統計を残すことを優先し、正常な切断は 0、異常は非ゼロで返す。
  bool pipeAlive = true;
  int exitCode = 0;
  // 送信失敗の扱い。相手先の破棄に伴う切断（BROKEN_PIPE 等）は正常な終了手順の
  // 合図であり、受信側の切断検知と同じく統計付きの正常終了にする（exit は 0 のまま）。
  // アプリ側は未返却の借用を隔離して非ゼロ化するため、失敗の見落としにはならない。
  // それ以外の送信失敗は異常として非ゼロ終了にする。
  // 終了直前の競合（サーバ破棄と次フレーム送信の入れ違い）は切断側に倒す。
  auto note_send_failure = [&](const char* what, const std::exception& e) {
    const PipeError* pipe_error = dynamic_cast<const PipeError*>(&e);
    // 相手先の破棄に伴う切断コード（いずれも「パイプは閉じられた」の変種）。
    // GetOverlappedResult/WriteFile の失敗理由として実測した 232 を含む。
    const bool graceful = pipe_error != nullptr &&
                          (pipe_error->code == ERROR_BROKEN_PIPE ||
                           pipe_error->code == ERROR_NO_DATA ||
                           pipe_error->code == ERROR_PIPE_NOT_CONNECTED);
    if (graceful) {
      Info("%s送信先の切断を検知（Win32=%lu）のため新規受付を止めて終了する", what,
           static_cast<unsigned long>(pipe_error->code));
    } else {
      Error("%s送信に失敗: %s", what, e.what());
      exitCode = 1;
    }
    pipeAlive = false;
    quit = true;
  };
  // 最後に Offer（有効・無効を問わず何らかの受信）があった時刻。初回受信前は idle 判定を
  // 行わない（起動直後の誤終了を避ける）。
  bool haveOfferEver = false;
  uint64_t lastOfferUs = NowUs();
  uint64_t frame_no = 0;
  uint64_t droppedIntermediate = 0;
  // --pace-records の未処理レコードの有無と発行時刻。on のとき前回発行分の
  // Offer 処理が済むまで新規発行を止め、500ms 停滞したら新しい分で解く。
  bool paceOutstanding = false;
  uint64_t paceIssuedUs = 0;
  const uint64_t deadlineUs =
      options.seconds > 0 ? NowUs() + static_cast<uint64_t>(options.seconds) * 1000000u : 0;

  // セッション束の破棄・再生成。LOSS_PENDING と --inject-session-restart で使う。
  // 成功時は新しい connEpoch で ledger を切り替え、旧世代のレコードを失効させる。
  // 入力空間・swapchain・blit は作り直し、instance・device・reader・pipe は保つ。
  // 失敗時は false を返す（呼び出し側は再試行か安全な終了を選ぶ）。
  // 再試行の上限 30 回・間隔 1 秒はいずれも意図的な値であり、仮置きではない。
  static constexpr int kRebuildMaxAttempts = 30;
  static constexpr uint64_t kRebuildRetryIntervalUs = 1000000u;
  auto rebuild_session = [&](const char* why) -> bool {
    ++stats.restartAttempts;
    Info("セッション再生成を開始（%s）。connEpoch=%u→%u", why, conn_epoch, conn_epoch + 1);
    input.DetachSpaces();
    blit.reset();
    swapchains.reset();
    session.reset();
    session_running = false;
    hasFresh = false;
    hold.Invalidate();
    holdHasRecord = false;
    ++conn_epoch;
    ledger.AdvanceConnEpoch(conn_epoch);
    stats.connEpoch = conn_epoch;
    try {
      session =
          std::make_unique<xr::Session>(transfer.XrInstance(), transfer.Vulkan(), space_pref);
    } catch (const std::exception& e) {
      Warn("セッション再生成に失敗（%s）: %s", why, e.what());
      return false;
    }
    if (input.hasActionSet()) {
      if (!input.AttachSpaces(session->handle())) {
        Warn("再生成後の attach に失敗したため入力は inactive のまま続行する");
      }
    }
    try {
      swapchains = std::make_unique<gfx::Swapchains>(transfer.XrInstance().handle(),
                                                     session->handle(), session->views(),
                                                     gfx::M2PreferredSwapchainFormats());
      blit = std::make_unique<BlitPass>(transfer.Vulkan(), *swapchains, options.dumpSubmitDir);
    } catch (const std::exception& e) {
      Warn("再生成後の swapchain 生成に失敗（%s）: %s", why, e.what());
      session.reset();
      return false;
    }
    layer.space = session->space();
    ++stats.restarts;
    Info("セッション再生成に成功（%s）。connEpoch=%u space=%s", why, conn_epoch,
         xr::SpaceTypeName(session->spaceType()));
    return true;
  };

  // REFERENCE_SPACE_CHANGE_PENDING の適用。新しい原点で locate できることを確認してから
  // spaceEpoch を進め、旧世代のレコードを失効させる（DESIGN §10）。
  // --inject-space-change の模擬もこの経路を通す（別経路は作らない）。
  auto apply_pending_space = [&]() {
    if (!pending_space_change || !session) {
      return;
    }
    if (session->RecreateSpaceForPendingChange(last_display_time)) {
      ledger.AdvanceSpaceEpoch(ledger.spaceEpoch() + 1);
      hasFresh = false;
      hold.Invalidate();
      holdHasRecord = false;
      ++stats.spaceChanges;
      if (pending_space_injected) {
        Info("模擬 space 変更を適用（実イベントではない。--inject-space-change。"
             "spaceEpoch=%u。旧レコードを失効）",
             ledger.spaceEpoch());
      } else {
        Info("reference space 変更を適用: spaceEpoch=%u（旧レコードを失効）", ledger.spaceEpoch());
      }
    } else {
      Warn("reference space 変更の適用に失敗したため旧空間・旧世代を保つ");
    }
    pending_space_change = false;
    pending_space_injected = false;
  };

  auto poll_events = [&]() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(transfer.XrInstance().handle(), &event) == XR_SUCCESS) {
      switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
          const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
          if (!session || changed->session != session->handle()) {
            break;
          }
          session->setState(changed->state);
          Info("session 状態: %d (connEpoch=%u)", static_cast<int>(changed->state), conn_epoch);
          switch (changed->state) {
            case XR_SESSION_STATE_READY:
              if (!session_running) {
                session->Begin();
                session_running = true;
                Info("セッション開始");
              }
              break;
            case XR_SESSION_STATE_STOPPING:
              // STOPPING → xrEndSession。脱着は loss とは限らないため束は保つ
              // （focus/visible/shouldRender に従って提出だけ止める）。
              if (session_running) {
                session->End();
                session_running = false;
                Info("セッション終了（STOPPING。束は保持し READY で再開する）");
              }
              break;
            case XR_SESSION_STATE_EXITING:
              // EXITING はランタイムの終了要求であり、再生成せず安全に終わる。
              quit = true;
              break;
            case XR_SESSION_STATE_LOSS_PENDING:
              // loss 時は束を破棄し、新しい connEpoch で再生成する。
              // ランタイムが復帰しなければ再試行の末に安全に終了する。
              Info("セッション loss を検知したため束を破棄し再生成する");
              input.DetachSpaces();
              blit.reset();
              swapchains.reset();
              session.reset();
              session_running = false;
              hasFresh = false;
              hold.Invalidate();
              holdHasRecord = false;
              needs_rebuild = true;
              rebuild_attempts_left = kRebuildMaxAttempts;
              last_rebuild_try_us = 0;
              break;
            default:
              break;
          }
          break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
          quit = true;
          break;
        case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
          // 実 profile の読みは毎フレームの Snapshot 側で行い、変化を記録する。
          Info("interaction profile 変更イベントを受信（次フレームの取得で確認する）");
          break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
          // 適用時刻に従い、新しい原点で locate できるタイミングで世代を進める。
          // ここでは受付だけ行い、適用はフレーム側で行う。
          pending_space_change = true;
          Info("reference space 変更予定を受信（次フレームで適用を試みる）");
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
  // M4: 同じパイプで main → exe の HapticCommand も受ける。Offer とは別扱いにし、
  // producer 生存の証拠（haveOfferEver）には数えない。
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
      // M4: main → exe の HapticCommand は Offer とは別扱いにする。
      // producer 生存の証拠には数えず、借用台帳にも触れない。
      // セッションが無い・実行中でない場合は inactive として記録する（例外にしない）。
      if (msg.magic == kMagic && msg.version == kVersion && msg.type == kTypeHapticCommand) {
        if (!IsValidHapticCommand(msg)) {
          Warn("不正な HapticCommand を破棄（hand/amplitude/duration の不一致）");
          continue;
        }
        ++stats.hapticReceived;
        const uint32_t hand = HapticHand(msg);
        const float amp = HapticAmplitude(msg);
        const uint32_t dur = HapticDurationMs(msg);
        // FOCUSED でなければ送らず inactive に数える（入力の sync と同じ基準）。
        const bool haptic_focused =
            session && session->state() == XR_SESSION_STATE_FOCUSED;
        bool sent = false;
        if (session) {
          sent = input.ApplyHaptic(session->handle(), hand, amp, dur, haptic_focused);
        } else {
          input.ApplyHaptic(XR_NULL_HANDLE, hand, amp, dur, false);
        }
        // 送信結果は XrResult で残す（VkResult ではない。実機で「成功だが振動不明」の
        // 切り分け用）。inactive（未呼出し）と呼出し失敗は区別して出す。
        if (sent) {
          Info("haptics 送信: %s 振幅=%.2f 持続=%ums（XrResult=%s）", hand == 0 ? "左" : "右",
               amp, dur, XrResultToString(input.lastHapticResult()));
        } else if (input.lastHapticCalled()) {
          Info("haptics 未送信: %s 振幅=%.2f 持続=%ums（呼出し失敗 XrResult=%s。統計に残る）",
               hand == 0 ? "左" : "右", amp, dur, XrResultToString(input.lastHapticResult()));
        } else {
          Info("haptics 未送信: %s 振幅=%.2f 持続=%ums（inactive のため送らず。統計に残る）",
               hand == 0 ? "左" : "右", amp, dur);
        }
        continue;
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
        note_send_failure("ACK", e);
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
      // pose 不一致の切り分け用に先頭 5 件だけ詳報する。復号値と、同じ id16 を持つ
      // 台帳レコードの期待値を並べ、shim 側の付番・pipe 経由の取り違え・画像破損を区別する。
      // 毎回は出さない（72Hz でのログ肥大の防止）。
      if (mismatchLogCount < 5 &&
          (reason == RejectReason::PoseMismatch || reason == RejectReason::UnknownId)) {
        const RenderRecord* cand = nullptr;
        uint16_t want = 0;
        if (ledger.LookupById16(id16, &cand, &want) && cand != nullptr) {
          Info("M2 棄却診断[%llu]: %s decoded id16=0x%04x tag=0x%04x / 台帳 renderId=%llu"
               " wantTag=0x%04x conn=%u space=%u（台帳現世代 conn=%u space=%u）/ ledger最新=%llu",
               static_cast<unsigned long long>(mismatchLogCount),
               RejectReasonName(reason), id16, tag,
               static_cast<unsigned long long>(cand->renderId), want, cand->connEpoch,
               cand->spaceEpoch, ledger.connEpoch(), ledger.spaceEpoch(),
               static_cast<unsigned long long>(ledger.latestRenderId()));
        } else {
          Info("M2 棄却診断[%llu]: %s decoded id16=0x%04x tag=0x%04x / 台帳にid16一致なし"
               "（現世代 conn=%u space=%u）/ ledger最新=%llu",
               static_cast<unsigned long long>(mismatchLogCount),
               RejectReasonName(reason), id16, tag, ledger.connEpoch(), ledger.spaceEpoch(),
               static_cast<unsigned long long>(ledger.latestRenderId()));
        }
        ++mismatchLogCount;
      }
      Info("M2 棄却: %s（id16=0x%04x tag=0x%04x）", RejectReasonName(reason), id16, tag);
      return;
    }
    freshRecord = *hit;
    freshW = transfer.OwnWidth();
    freshH = transfer.OwnHeight();
    hasFresh = true;
  };

  // XRループの例外（xrWaitFrame 等の CheckXr 失敗）は統計付きで終了する。
  // 統計欠落と abort（未捕捉例外の即死）を避けるための境界である。
  // なお通常の終了（秒数・切断・途絶）は break で抜け、ここには来ない。
  try {
  while (!quit) {
    if (deadlineUs != 0 && NowUs() >= deadlineUs) {
      Info("指定秒数を経過したため終了する");
      break;
    }
    poll_events();
    if (quit) {
      break;
    }
    // M4: --inject-session-restart の発火（1 回だけ。復帰経路の検証用）。
    // ランタイムが受け付ける範囲で束を破棄・再生成し、成功/失敗を統計に残す。
    if (!restart_injected && options.injectSessionRestartMs > 0 && session &&
        NowUs() - start_us >= static_cast<uint64_t>(options.injectSessionRestartMs) * 1000u) {
      restart_injected = true;
      if (!rebuild_session("--inject-session-restart")) {
        needs_rebuild = true;
        rebuild_attempts_left = kRebuildMaxAttempts;
        last_rebuild_try_us = NowUs();
      }
    }
    // M4計測: --inject-space-change の発火（1 回だけ。適用は apply_pending_space の
    // 実イベントと同じ経路を通す。実イベントではないことは受付・適用の両方で明記する）。
    if (!space_change_injected && options.injectSpaceChangeMs > 0 && session &&
        NowUs() - start_us >= static_cast<uint64_t>(options.injectSpaceChangeMs) * 1000u) {
      space_change_injected = true;
      pending_space_change = true;
      pending_space_injected = true;
      Info("検証フック --inject-space-change: 模擬の space 変更要求（実イベントではない。"
           "次フレームで適用を試みる）");
    }
    // M4: loss 後の再生成の再試行（1 秒周期）。上限を超えたらランタイム未復帰として
    // 安全に終了する（統計出力。非ゼロにはしない。こちらの破損ではないため）。
    if (needs_rebuild && !session && rebuild_attempts_left > 0 &&
        NowUs() - last_rebuild_try_us >= kRebuildRetryIntervalUs) {
      last_rebuild_try_us = NowUs();
      --rebuild_attempts_left;
      if (rebuild_session("loss 後の再試行")) {
        needs_rebuild = false;
      } else if (rebuild_attempts_left == 0) {
        Info("ランタイムが復帰しないため安全に終了する（再試行の上限に到達）");
        break;
      }
    }
    // M4: 参照空間変更の適用は running 中のフレーム時刻が要るため、
    // xrWaitFrame の後（running 側）で行う。実行中でない間は受付だけ保ち、
    // 適用は開始後の最初のフレームまで待つ。
    if (!session_running || !session) {
      // 実行中でない間も Offer の受付・ACK と切断検知は進める。受信した Offer は
      // drain_offers が Done を返すため、消費して返せなくなることはない。
      // 以前はここで切断を見ず --seconds か kill 待ちでぶら下がったため、
      // harness 先死で統計を出さずに殺されていた（DESIGN §6③の違反）。
      const uint64_t offersBeforeIdle = stats.offers;
      drain_offers();
      if (stats.offers != offersBeforeIdle) {
        paceOutstanding = false;
      }
      ledger.SweepExpired(NowUs());
      if (quit) {
        break;
      }
      if (haveOfferEver && idleTimeoutUs != 0u && NowUs() - lastOfferUs > idleTimeoutUs) {
        Info("Offer が %dms 途絶えたため producer 終了とみなして統計を出して終了する",
             options.producerIdleTimeoutMs);
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
    CheckXr(xrWaitFrame(session->handle(), &wait_info, &frame_state), "xrWaitFrame");
    last_display_time = frame_state.predictedDisplayTime;
    // M4: 参照空間変更の適用（新しい原点で locate できる確認が取れてから世代を進める）。
    // 当フレームの予測表示時刻を使う（時刻 0 を渡すと受け付けないランタイムがある）。
    apply_pending_space();
    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    CheckXr(xrBeginFrame(session->handle(), &begin_info), "xrBeginFrame");

    bool rendered = false;
    if (frame_state.shouldRender == XR_TRUE) {
      XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
      locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
      locate_info.displayTime = frame_state.predictedDisplayTime;
      locate_info.space = session->space();
      XrViewState view_state{XR_TYPE_VIEW_STATE};
      uint32_t view_count = 0;
      CheckXr(xrLocateViews(session->handle(), &locate_info, &view_state,
                            static_cast<uint32_t>(views.size()), &view_count, views.data()),
              "xrLocateViews");
      const bool pose_valid =
          (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0 &&
          (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
      if (view_count == views.size() && pose_valid) {
        // 要求アトラス寸法は左右各眼の描画倍率から求める。swapchain 自体は
        // XR 推奨サイズのまま作る（VRAM 圧迫を避ける。既定 0.5）。
        const vk::Extent2D eye0 = swapchains->extent(0);
        const vk::Extent2D eye1 = swapchains->extent(1);
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
        // --pace-records: 未処理の画像がある間は新しいレコードを発行しない
        // （RenderRequest を送らない）。Offer 処理後の次のフレームで発行する。
        // 応答が無いまま 500ms 経過したら同じレコードの再送ではなく新しい
        // レコードを発行して詰まらないようにする（古い分は失効させる）。
        // off のときは従来どおり毎フレーム発行する。
        bool issue_record = true;
        if (options.paceRecords && paceOutstanding) {
          if (NowUs() - paceIssuedUs > kPaceStallTimeoutUs) {
            Info("pace: 応答が %llums 無いため新しいレコードを発行する（旧分は失効させる）",
                 static_cast<unsigned long long>(kPaceStallTimeoutUs / 1000u));
            paceOutstanding = false;
          } else {
            ++stats.pacedWaits;
            issue_record = false;
          }
        }
        if (issue_record) {
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
            note_send_failure("RenderRequest", e);
          }
          rendered = true;
          if (options.paceRecords) {
            paceOutstanding = true;
            paceIssuedUs = NowUs();
          }
        }
      }
    }

    // Offer の受信・ACK は shouldRender が false のフレームでも毎フレーム行う
    //（SYNCHRONIZED 中でも Offer を受けて ACK できること）。提出だけを
    // shouldRender（rendered）で止める。drain_offers 内の期限切れは返却許可ではなく、
    // ここでの期限は受信のポーリング間隔である。
    const uint64_t offersBefore = stats.offers;
    drain_offers();
    if (stats.offers != offersBefore) {
      // Offer を処理（受理/棄却/中間破棄）し終えたため、次のフレームで
      // 新しいレコードを発行できる（--pace-records の待ちの解除）。
      paceOutstanding = false;
    }
    ledger.SweepExpired(NowUs());
    // harness が Offer を止めたまま読取側を開き続けると（半閉鎖）、切断は返らず
    // 空フレームが続く。一定時間の途絶は producer 終了とみなして統計を出して終了し、
    // kill による統計消失を避ける。切断検知時と同様、現在のフレームは安全に終える。
    if (pipeAlive && !quit && haveOfferEver && idleTimeoutUs != 0u &&
        NowUs() - lastOfferUs > idleTimeoutUs) {
      Info("Offer が %dms 途絶えたため producer 終了とみなして統計を出して終了する",
           options.producerIdleTimeoutMs);
      pipeAlive = false;
      quit = true;
    }
    // M4: 入力の取得と main への送信。FOCUSED 時に sync し、それ以外は inactive の
    // スナップショットとして出す（「0」と「未対応」を区別できる形。毎フレーム送信し、
    // ログは毎秒に抑える）。脱着時は focus を失うため提出だけ止め、受信・送信は続ける。
    if (!quit) {
      const bool focused = session->state() == XR_SESSION_STATE_FOCUSED;
      input.Sync(session->handle(), focused);
      const xr::InputSnapshot snapshot = input.Snapshot(
          session->handle(), session->space(), frame_state.predictedDisplayTime, focused, frame_no);
      // M4計測: 押下エッジの記録。1 秒周期のスナップショットでは短い押下を
      // 取りこぼすため、状態が変化したフレームで 1 行出す（変化が無ければ出さない）。
      if (have_prev_snapshot) {
        const std::string edge = xr::InputManager::DescribeEdges(prev_snapshot, snapshot);
        if (!edge.empty()) {
          Info("[M4エッジ] frame=%llu %s", static_cast<unsigned long long>(frame_no),
               edge.c_str());
        }
      }
      prev_snapshot = snapshot;
      have_prev_snapshot = true;
      input_latch[0] |= xr::InputManager::PressedBits(snapshot.hands[0]);
      input_latch[1] |= xr::InputManager::PressedBits(snapshot.hands[1]);
      try {
        pipe.Send(MakeInputStateMessage(snapshot, conn_epoch, ledger.spaceEpoch(), input_latch[0],
                                        input_latch[1]));
        ++stats.inputSent;
      } catch (const std::exception& e) {
        note_send_failure("InputState", e);
      }
      if (NowUs() - last_input_log_us >= 1000000u) {
        last_input_log_us = NowUs();
        Info("[M4入力] %s",
             xr::InputManager::SummarizeWithLatch(snapshot, input_latch[0], input_latch[1])
                 .c_str());
        input_latch[0] = input_latch[1] = 0;
      }
    }
    // 提出は受理画像のレコードの pose/FOV で行う。最新 pose への付け替えはしない。
    // 新規受理が無いフレームでも hold の複製を同じ pose/FOV で出し直す（黒防止）。
    // 最初の 1 枚が来る前だけ空フレームにする。受理から record-ttl 超の新規受理は
    // 失効として棄却するが、hold の再提出は別枠（既定では無制限）で行う。
    XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
    end_info.displayTime = frame_state.predictedDisplayTime;
    end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
    bool submit_layer = false;
    bool is_resubmit = false;
    // src 画像（GENERAL 駐めのアトラス）の左右半分を各眼へ blit し、
    // record の pose/FOV を提出層に載せる。新規受理と再提出で共用する。
    // 失敗時は例外にする（呼び出し側は空フレームで終える）。
    auto submit_image = [&](vk::Image src, uint32_t srcW, uint32_t srcH,
                            const RenderRecord& rec, bool allow_dump) {
      // 検証用の提出画像ダンプ。N 回目の提出時に blit 後・release 前の両眼を写す。
      // 1 回だけ。提出の正しさは変えない（ダンプ後に COLOR_ATTACHMENT に戻す）。
      // 再提出では保存しない（回数は新規受理の提出基準に保つ）。
      const bool do_dump = allow_dump && !options.dumpSubmitDir.empty() && !dumpDone && blit &&
                           blit->dumpEnabled() &&
                           submits + 1 == static_cast<uint64_t>(options.dumpFrame);
      const BlitPass::Result r0 =
          blit->Blit(0, src, srcW, srcH, 0, options.submitVerify, do_dump);
      const BlitPass::Result r1 =
          blit->Blit(1, src, srcW, srcH, 1, options.submitVerify, do_dump);
      if (do_dump) {
        dumpDone = true;
        Info("dump-submit: %llu 回目の提出を保存した", static_cast<unsigned long long>(submits + 1));
      }
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
        const EyePose& p = (eye == 0) ? rec.left : rec.right;
        const EyeFov& f = (eye == 0) ? rec.leftFov : rec.rightFov;
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
        proj_views[eye].subImage.swapchain = swapchains->handle(eye);
        proj_views[eye].subImage.imageRect.offset = {0, 0};
        proj_views[eye].subImage.imageRect.extent.width =
            static_cast<int32_t>(swapchains->extent(eye).width);
        proj_views[eye].subImage.imageRect.extent.height =
            static_cast<int32_t>(swapchains->extent(eye).height);
        proj_views[eye].subImage.imageArrayIndex = 0;
      }
    };
    // 新規受理があれば hold へ GPU 複製してから提出する。TTL 超の受理でも複製は残す
    // （再提出は黒より古い絵を優先するため。上限は --resubmit-max-age-ms で別に付ける）。
    // --no-resubmit では複製自体を行わない（比較用の旧挙動に余計な GPU 負荷を載せない）。
    if (hasFresh && options.resubmit) {
      try {
        hold.SaveFrom(transfer.OwnImage(), freshW, freshH);
        holdRecord = freshRecord;
        holdHasRecord = true;
      } catch (const std::exception& e) {
        Warn("hold 複製に失敗したため再提出はできない: %s", e.what());
        hold.Invalidate();
        holdHasRecord = false;
      }
    }
    bool fresh_failed = false;
    if (rendered && hasFresh) {
      const uint64_t age = NowUs() - freshRecord.issuedUs;
      if (age <= ledger.maxAgeUs()) {
        try {
          submit_image(transfer.OwnImage(), freshW, freshH, freshRecord, true);
          Info("M2 提出: renderId=%llu 年齢=%lluus（画像対応レコードの pose/FOV）",
               static_cast<unsigned long long>(freshRecord.renderId),
               static_cast<unsigned long long>(age));
          submit_layer = true;
          ++submits;
          hasFresh = false;
        } catch (const std::exception& e) {
          Warn("blit に失敗したため空フレームで終える: %s", e.what());
          fresh_failed = true;
        }
      } else {
        hasFresh = false;
      }
    }
    // 再提出。新規受理が無い・使えないフレームでも最後の受理を同じ pose/FOV で出し直す。
    // 新規 blit の失敗直後は出さない（同じ原因で落ちる重複を避ける）。
    // shouldRender が偽のフレームでも層を出す。最初の 1 枚が来る前だけ空にする。
    if (!submit_layer && !fresh_failed && options.resubmit && blit && hold.Valid() &&
        holdHasRecord) {
      const uint64_t age = NowUs() - holdRecord.issuedUs;
      const bool too_old =
          options.resubmitMaxAgeMs > 0 && age > options.resubmitMaxAgeMs * 1000u;
      if (!too_old) {
        try {
          submit_image(hold.image(), hold.width(), hold.height(), holdRecord, false);
          ++stats.resubmits;
          const uint64_t now = NowUs();
          if (stats.resubmits <= 3 || now - lastResubmitLogUs >= 1000000u) {
            lastResubmitLogUs = now;
            Info("M2 再提出: renderId=%llu 年齢=%lluus（最終受理の pose/FOV のまま）",
                 static_cast<unsigned long long>(holdRecord.renderId),
                 static_cast<unsigned long long>(age));
          }
          submit_layer = true;
          is_resubmit = true;
        } catch (const std::exception& e) {
          Warn("再提出の blit に失敗したため空フレームで終える: %s", e.what());
        }
      }
    }
    if (submit_layer) {
      end_info.layerCount = 1;
      end_info.layers = layers;
      ++stats.rendered;
      if (!is_resubmit) {
        ++stats.submitLayers;
      }
    } else {
      end_info.layerCount = 0;
      ++stats.emptyFrames;
    }
    CheckXr(xrEndFrame(session->handle(), &end_info), "xrEndFrame");
  }
  } catch (const std::exception& e) {
    Error("XRループの異常終了: %s（統計を出して終了する）", e.what());
    pipeAlive = false;
    exitCode = 1;
  }

  stats.Print(ledger.stats(), input.stats());
  Info("M2 中間破棄: %llu（最新優先のため Done のみ返した件数）",
       static_cast<unsigned long long>(droppedIntermediate));
  if (stats.submitLayers == 0 && stats.resubmits == 0) {
    Warn("提出層 0 のまま終了した（受理画像が無い。harness 側の描画を確認すること）");
  }
  if (!options.dumpSubmitDir.empty() && !dumpDone) {
    Warn("dump-submit: %d 回目の提出に届かず保存しなかった（提出 %llu 回）",
         options.dumpFrame, static_cast<unsigned long long>(submits));
  }
  if (!pipeAlive) {
    Info("パイプ終了に伴う終了（exit=%d）", exitCode);
  }
  // セッション束は明示の順序で破棄する（blit → swapchain → session）。
  // 宣言順の逆順 RAII でも同じ順序になるが、段階ログと合わせて順序を固定する。
  // なお実行中のまま xrDestroySession しても SteamVR は受け付けることを確認済み
  // （終了時 exit 0）。STOPPING 未満での xrEndSession は NOT_STOPPING で失敗する
  // ため、ここであえて End は呼ばない（イベント経路の End に任せる）。
  stage("セッション束の破棄を開始する");
  stage("blit を破棄する");
  blit.reset();
  stage("swapchain を破棄する");
  swapchains.reset();
  stage("セッションを破棄する");
  session.reset();
  stage("セッション束の破棄完了（残りは RAII 破棄）");
  // 残りの RAII（TransferSession・reader・pipe 等）と静的デストラクタは
  // 走らせずに終了する（既定 on。終了時クラッシュの回避）。
  // --no-quick-exit では旧来どおり return し、RAII 破棄を行う（切り分け用）。
  if (options.quickExit) {
    quick_exit(exitCode);
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
