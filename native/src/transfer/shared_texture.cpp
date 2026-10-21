// shared_texture の実体。Windows 専用であるため、非 Windows では説明文だけにして
// ビルドを通す（Linux の輸送実装は将来の差し替え対象）。
// VK_USE_PLATFORM_WIN32_KHR は CMake が Windows ビルド全体に定義する。
#ifdef _WIN32

#include "transfer/shared_texture.h"

#include <windows.h>

#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/log.h"
#include "common/xr_result.h"
#include "transfer/protocol.h"

namespace xrift::transfer {

namespace {

// 転送に必須のデバイス拡張。存在しなければ明確なエラーで停止する。
// 追加は gfx::VulkanContext の extraDeviceExtensions 経由で行う。
// 名前は vulkan_core.h の正準マクロを使う（手書きの大文字綴りは不一致になる）。
constexpr const char* kRequiredDeviceExtensions[] = {
    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
};

// vk::Result を返す呼び出しの検査。成功以外は vk::to_string 付きの例外にする。
// gfx 側の CheckVk と同粒度にする（既存 TU と重複する小関数は局所化する方針に従う）。
void CheckVk(vk::Result result, const char* what) {
  if (result == vk::Result::eSuccess) {
    return;
  }
  throw std::runtime_error(std::string(what) + " に失敗 (VkResult=" + vk::to_string(result) + ")");
}

// 例外 (vk::SystemError) を投げる raii 呼び出しを従来の CheckVk と同じ粒度に揃える。
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

// GPU 完了を確認できない失敗（フェンス待ち失敗・device lost・waitIdle 失敗）の合図。
// timeout は返却許可ではないため、呼び出し側は Done を送らず新規受付を止める。
// submit 前の失敗（GPU は触っていない）や完了確認済みの失敗とは区別する。
class UnconfirmedCompletion : public std::runtime_error {
 public:
  explicit UnconfirmedCompletion(const std::string& message) : std::runtime_error(message) {}
};

// 受領した複製ハンドルの RAII。import 成否に関わらず閉じる。
// 元の Electron 側ハンドルではなく、複製だけを閉じる。
class DuplicatedHandle {
 public:
  explicit DuplicatedHandle(uint64_t value)
      : handle_(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value))) {}
  ~DuplicatedHandle() {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(handle_);
    }
  }
  DuplicatedHandle(const DuplicatedHandle&) = delete;
  DuplicatedHandle& operator=(const DuplicatedHandle&) = delete;
  HANDLE get() const { return handle_; }

 private:
  HANDLE handle_;
};

std::vector<std::string> RequiredExtensionList() {
  return std::vector<std::string>(std::begin(kRequiredDeviceExtensions),
                                  std::end(kRequiredDeviceExtensions));
}

vk::ImageSubresourceRange FullColorRange() {
  vk::ImageSubresourceRange range{};
  range.aspectMask = vk::ImageAspectFlagBits::eColor;
  range.baseMipLevel = 0;
  range.levelCount = 1;
  range.baseArrayLayer = 0;
  range.layerCount = 1;
  return range;
}

}  // namespace

TransferSession::TransferSession() : vulkan_(instance_, RequiredExtensionList()) {
  const vk::raii::PhysicalDevice& physical = vulkan_.physicalDevice();
  const vk::raii::Device& device = vulkan_.device();

  // 物理デバイスの API 版を確認する。外部形式の問い合わせは 1.1 以降の core 関数を使う。
  const uint32_t apiVersion = physical.getProperties().apiVersion;
  if (apiVersion < VK_API_VERSION_1_1) {
    throw std::runtime_error("転送には Vulkan 1.1 以降の物理デバイスが必要");
  }

  // 必須のデバイス拡張の有無は VulkanContext が vkCreateDevice の前に名指しで確認する。
  // ここでは物理デバイスの API 版だけを確認する。

  vk::CommandPoolCreateInfo pool_info{};
  pool_info.sType = vk::StructureType::eCommandPoolCreateInfo;
  pool_info.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
  pool_info.queueFamilyIndex = vulkan_.queueFamilyIndex();
  InvokeVk("vkCreateCommandPool", [&] { pool_ = vk::raii::CommandPool(device, pool_info); });

  vk::CommandBufferAllocateInfo alloc{};
  alloc.sType = vk::StructureType::eCommandBufferAllocateInfo;
  alloc.commandPool = *pool_;
  alloc.level = vk::CommandBufferLevel::ePrimary;
  alloc.commandBufferCount = 1;
  InvokeVk("vkAllocateCommandBuffers", [&] {
    vk::raii::CommandBuffers buffers = device.allocateCommandBuffers(alloc);
    copyCmd_ = std::move(buffers[0]);
  });

  vk::FenceCreateInfo fence_info{};
  fence_info.sType = vk::StructureType::eFenceCreateInfo;
  InvokeVk("vkCreateFence", [&] { fence_ = vk::raii::Fence(device, fence_info); });

  Info("転送セッション開始: GPU=%s", vulkan_.gpuName().c_str());
}

TransferSession::~TransferSession() {
  // 仕掛かりの GPU アクセスを吸収してから破棄する。
  // 完了未確認の保留資源がある場合は waitIdle の成否で扱いを分ける。成功時は通常破棄し、
  // 失敗時は破棄せず OS/ドライバ回収に委ねる（意図的リーク）。raii の自動破棄を抑えるため
  // unique_ptr を放棄する（所有権だけ放棄し、vkDestroy は呼ばない）。
  // デストラクタのため例外は出さない。
  bool idle_ok = false;
  try {
    vulkan_.device().waitIdle();
    idle_ok = true;
  } catch (...) {
  }
  if (!idle_ok && (unconfirmedImage_ || unconfirmedMemory_)) {
    unconfirmedImage_.release();
    unconfirmedMemory_.release();
    // プロセス終了時の OS/ドライバ回収に委ねる。
  }
  if (idle_ok) {
    // GPU が触っていないことが確定したため、残っていれば通常破棄する。
    ReleaseUnconfirmedCounted();
  }
  // 以降は raii の逆順破棄（自前 → fence → pool → device → XR instance）。
}

void TransferSession::ReleaseUnconfirmedCounted() {
  unconfirmedMemory_.reset();
  unconfirmedImage_.reset();
}

void TransferSession::EnsureOwnImage(uint32_t width, uint32_t height) {
  if (ownDefined_ && ownWidth_ == width && ownHeight_ == height) {
    return;
  }
  const vk::raii::Device& device = vulkan_.device();
  ownMemory_.clear();
  ownImage_.clear();
  ownDefined_ = false;

  vk::ImageCreateInfo info{};
  info.sType = vk::StructureType::eImageCreateInfo;
  info.imageType = vk::ImageType::e2D;
  info.format = vk::Format::eB8G8R8A8Unorm;
  info.extent = vk::Extent3D{width, height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = vk::SampleCountFlagBits::e1;
  info.tiling = vk::ImageTiling::eOptimal;
  info.usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst |
               vk::ImageUsageFlagBits::eSampled;
  info.sharingMode = vk::SharingMode::eExclusive;
  info.initialLayout = vk::ImageLayout::eUndefined;
  InvokeVk("vkCreateImage(自前)", [&] { ownImage_ = vk::raii::Image(device, info); });

  // 所要量の問い合わせは 1.0 系の vkGetImageMemoryRequirements を使う。
  // DeviceImageMemoryRequirements 経由（1.3 / maintenance4 相当）は使わない。
  const vk::MemoryRequirements req = ownImage_.getMemoryRequirements();
  const vk::PhysicalDeviceMemoryProperties mem_props =
      vulkan_.physicalDevice().getMemoryProperties();
  int32_t index = -1;
  for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
    if ((req.memoryTypeBits & (1u << i)) != 0u &&
        (mem_props.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal) ==
            vk::MemoryPropertyFlagBits::eDeviceLocal) {
      index = static_cast<int32_t>(i);
      break;
    }
  }
  if (index < 0) {
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
      if ((req.memoryTypeBits & (1u << i)) != 0u) {
        index = static_cast<int32_t>(i);
        break;
      }
    }
  }
  if (index < 0) {
    throw std::runtime_error("自前イメージのメモリタイプが無い");
  }
  vk::MemoryAllocateInfo alloc{};
  alloc.sType = vk::StructureType::eMemoryAllocateInfo;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = static_cast<uint32_t>(index);
  InvokeVk("vkAllocateMemory(自前)", [&] { ownMemory_ = vk::raii::DeviceMemory(device, alloc); });
  InvokeVk("vkBindImageMemory(自前)",
           [&] { ownImage_.bindMemory(*ownMemory_, 0); });
  ownWidth_ = width;
  ownHeight_ = height;
  ownDefined_ = false;
}

uint32_t TransferSession::ProcessOffer(uint64_t remoteHandle, uint32_t width, uint32_t height) {
  const vk::raii::Device& device = vulkan_.device();
  const vk::raii::Queue& queue = vulkan_.queue();

  try {
    // 受領した複製ハンドルは受領直後から単一の所有ガードで保持し、全経路で閉じる。
    // import 前の失敗（照合・生成の失敗）でも漏らさない。
    DuplicatedHandle owned(remoteHandle);

    // 前回の完了未確認資源が残っている場合の新規受付は誤りである。
    // 古い保留を上書き破棄するより保持を優先し、ACK せず受付停止に回す。
    if (unconfirmedImage_ || unconfirmedMemory_) {
      throw UnconfirmedCompletion("前回の完了未確認 import 資源が残っているため新規受付を停止");
    }

    // Offer の width/height/format を信じて import し、妥当性は Vulkan 側の
    // import・コピーで担保する。不正な寸法・形式は失敗側（kStatusCopyFailed）に回り、
    // 黙って成功にはならない。

    // 2. import 用イメージの生成。実 Desc（Offer の codedSize）に合わせ、決め打ちしない。
    vk::ExternalMemoryImageCreateInfo external{};
    external.sType = vk::StructureType::eExternalMemoryImageCreateInfo;
    external.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eD3D11Texture;
    vk::ImageCreateInfo image_info{};
    image_info.sType = vk::StructureType::eImageCreateInfo;
    image_info.pNext = &external;
    image_info.imageType = vk::ImageType::e2D;
    image_info.format = vk::Format::eB8G8R8A8Unorm;
    image_info.extent = vk::Extent3D{width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = vk::SampleCountFlagBits::e1;
    image_info.tiling = vk::ImageTiling::eOptimal;
    image_info.usage = vk::ImageUsageFlagBits::eTransferSrc |
                       vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
    image_info.sharingMode = vk::SharingMode::eExclusive;
    image_info.initialLayout = vk::ImageLayout::eUndefined;
    vk::raii::Image import_image{nullptr};
    InvokeVk("vkCreateImage(import)", [&] { import_image = vk::raii::Image(device, image_info); });

    // 3. 外部形式の対応確認とメモリ所要量の取得。
    vk::PhysicalDeviceExternalImageFormatInfo external_format{};
    external_format.sType = vk::StructureType::ePhysicalDeviceExternalImageFormatInfo;
    external_format.handleType = vk::ExternalMemoryHandleTypeFlagBits::eD3D11Texture;
    vk::PhysicalDeviceImageFormatInfo2 format_info{};
    format_info.sType = vk::StructureType::ePhysicalDeviceImageFormatInfo2;
    format_info.pNext = &external_format;
    format_info.format = vk::Format::eB8G8R8A8Unorm;
    format_info.type = vk::ImageType::e2D;
    format_info.tiling = vk::ImageTiling::eOptimal;
    format_info.usage = vk::ImageUsageFlagBits::eTransferSrc |
                        vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
    // 外部形式の取得は StructureChain で受ける。非対応時は例外になる。
    using FormatChain = vk::StructureChain<vk::ImageFormatProperties2,
                                           vk::ExternalImageFormatProperties>;
    FormatChain chain{};
    InvokeVk("vkGetPhysicalDeviceImageFormatProperties2", [&] {
      chain = vulkan_.physicalDevice()
                  .getImageFormatProperties2<vk::ImageFormatProperties2,
                                             vk::ExternalImageFormatProperties>(format_info);
    });
    const vk::ExternalMemoryFeatureFlags features =
        chain.get<vk::ExternalImageFormatProperties>().externalMemoryProperties.externalMemoryFeatures;
    const bool dedicated_only =
        (features & vk::ExternalMemoryFeatureFlagBits::eDedicatedOnly) != vk::ExternalMemoryFeatureFlags{};

    // 所要量の問い合わせは 1.0 系の vkGetImageMemoryRequirements を使う。
    // DeviceImageMemoryRequirements 経由（1.3 / maintenance4 相当）は使わない。
    const vk::MemoryRequirements req = import_image.getMemoryRequirements();
    vk::MemoryWin32HandlePropertiesKHR handle_props =
        device.getMemoryWin32HandlePropertiesKHR(
            vk::ExternalMemoryHandleTypeFlagBits::eD3D11Texture,
            reinterpret_cast<HANDLE>(static_cast<uintptr_t>(remoteHandle)));
    // 画像要件とハンドル由来の互換集合の積（AND）から選ぶ。空なら明示エラーにする。
    const uint32_t compatible = req.memoryTypeBits & handle_props.memoryTypeBits;
    const vk::PhysicalDeviceMemoryProperties mem_props =
        vulkan_.physicalDevice().getMemoryProperties();
    int32_t index = -1;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
      if ((compatible & (1u << i)) != 0u) {
        index = static_cast<int32_t>(i);
        break;
      }
    }
    if (index < 0) {
      throw std::runtime_error("import の互換メモリタイプが無い");
    }

    // 4. NT ハンドルの import。所有権は移らないため、guard が関数末尾で閉じる。
    // DEDICATED_ONLY の場合は専用 allocate 情報が必須である（実測事実）。
    vk::ImportMemoryWin32HandleInfoKHR import_info{};
    import_info.sType = vk::StructureType::eImportMemoryWin32HandleInfoKHR;
    import_info.handleType = vk::ExternalMemoryHandleTypeFlagBits::eD3D11Texture;
    import_info.handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(remoteHandle));
    vk::MemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = vk::StructureType::eMemoryDedicatedAllocateInfo;
    dedicated.image = *import_image;
    dedicated.pNext = &import_info;
    vk::MemoryAllocateInfo alloc{};
    alloc.sType = vk::StructureType::eMemoryAllocateInfo;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = static_cast<uint32_t>(index);
    alloc.pNext = dedicated_only ? static_cast<void*>(&dedicated) : static_cast<void*>(&import_info);
    vk::raii::DeviceMemory import_memory{nullptr};
    InvokeVk("vkAllocateMemory(import)",
             [&] { import_memory = vk::raii::DeviceMemory(device, alloc); });
    InvokeVk("vkBindImageMemory(import)",
             [&] { import_image.bindMemory(*import_memory, 0); });

    // import 資源は submit 以降の失敗（完了未確認）に備えて保留メンバへ移す。
    // 以降は非所有の表示ハンドル経由で参照し、破棄は「フェンス完了を確認できた」か
    // 「waitIdle が成功した」場合だけ行う。完了未確認時は保持し、プロセス終了時の
    // OS/ドライバ回収に委ねる（意図的リーク）。成功時・submit 前の失敗時は従来どおり
    // 即座に破棄する。
    unconfirmedImage_ = std::make_unique<vk::raii::Image>(std::move(import_image));
    unconfirmedMemory_ = std::make_unique<vk::raii::DeviceMemory>(std::move(import_memory));
    const vk::Image import_view = **unconfirmedImage_;

    // 5. 自前イメージの用意とコピー。完了はフェンスで待ち、完了後に ACK する。
    EnsureOwnImage(width, height);
    InvokeVk("vkResetFences", [&] { device.resetFences(*fence_); });
    InvokeVk("vkResetCommandBuffer", [&] { copyCmd_.reset(); });
    vk::CommandBufferBeginInfo begin{};
    begin.sType = vk::StructureType::eCommandBufferBeginInfo;
    begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    InvokeVk("vkBeginCommandBuffer", [&] { copyCmd_.begin(begin); });

    const vk::ImageSubresourceRange range = FullColorRange();
    const uint32_t qfi = vulkan_.queueFamilyIndex();
    // D3D11 共有テクスチャの implied layout は GENERAL である。
    // oldLayout=UNDEFINED での遷移は内容破棄を許すため使わない。
    // acquire: 外部（D3D11 側）から自デバイス queue family への所有権移転と
    // GENERAL → TRANSFER_SRC_OPTIMAL の遷移を兼ねる。外部 producer の書き込み完了は
    // このバリアでは保証されない（自プロセス内の順序だけを律し、producer 完了の証拠には
    // ならない。DESIGN §8 の未解決リスク）。source access は 0 にする。
    vk::ImageMemoryBarrier acquire{};
    acquire.sType = vk::StructureType::eImageMemoryBarrier;
    acquire.srcAccessMask = vk::AccessFlags{};
    acquire.dstAccessMask = vk::AccessFlagBits::eTransferRead;
    acquire.oldLayout = vk::ImageLayout::eGeneral;
    acquire.newLayout = vk::ImageLayout::eTransferSrcOptimal;
    acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    acquire.dstQueueFamilyIndex = qfi;
    acquire.image = import_view;
    acquire.subresourceRange = range;
    copyCmd_.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                             vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr,
                             acquire);

    vk::ImageMemoryBarrier own_to_dst{};
    own_to_dst.sType = vk::StructureType::eImageMemoryBarrier;
    own_to_dst.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
    own_to_dst.oldLayout =
        ownDefined_ ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined;
    own_to_dst.newLayout = vk::ImageLayout::eTransferDstOptimal;
    own_to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    own_to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    own_to_dst.image = *ownImage_;
    own_to_dst.subresourceRange = range;
    if (ownDefined_) {
      own_to_dst.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
      copyCmd_.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr,
                               own_to_dst);
    } else {
      copyCmd_.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                               vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr,
                               own_to_dst);
    }

    vk::ImageCopy region{};
    region.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    region.srcSubresource.layerCount = 1;
    region.dstSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    region.dstSubresource.layerCount = 1;
    region.extent = vk::Extent3D{width, height, 1};
    copyCmd_.copyImage(import_view, vk::ImageLayout::eTransferSrcOptimal, *ownImage_,
                       vk::ImageLayout::eTransferDstOptimal, region);

    // release: 自デバイス queue family から外部への所有権返却と
    // TRANSFER_SRC_OPTIMAL → GENERAL の遷移を兼ねる。フェンス完了後に ACK する。
    vk::ImageMemoryBarrier release{};
    release.sType = vk::StructureType::eImageMemoryBarrier;
    release.srcAccessMask = vk::AccessFlagBits::eTransferRead;
    release.dstAccessMask = vk::AccessFlags{};
    release.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
    release.newLayout = vk::ImageLayout::eGeneral;
    release.srcQueueFamilyIndex = qfi;
    release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    release.image = import_view;
    release.subresourceRange = range;
    copyCmd_.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                             vk::PipelineStageFlagBits::eBottomOfPipe, {}, nullptr, nullptr,
                             release);

    // 自前イメージは GENERAL で駐める。制御ブロック照合のサンプリング起点にする。
    vk::ImageMemoryBarrier own_park = own_to_dst;
    own_park.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    own_park.dstAccessMask = vk::AccessFlagBits::eShaderRead;
    own_park.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    own_park.newLayout = vk::ImageLayout::eGeneral;
    copyCmd_.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                             vk::PipelineStageFlagBits::eFragmentShader, {}, nullptr, nullptr,
                             own_park);

    InvokeVk("vkEndCommandBuffer", [&] { copyCmd_.end(); });
    vk::CommandBuffer raw_copy = *copyCmd_;
    vk::SubmitInfo submit{};
    submit.sType = vk::StructureType::eSubmitInfo;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &raw_copy;
    // submit の成否は実行済みか不明な境界である。失敗時は理由を問わず完了未確認とし、
    // Done を送らない（VK_ERROR_DEVICE_LOST 等でも CopyFailed に丸めない）。
    // submit 前の失敗（GPU は触っていない）とは区別する。
    try {
      queue.submit(submit, *fence_);
    } catch (const vk::SystemError& e) {
      const vk::Result result = static_cast<vk::Result>(e.code().value());
      throw UnconfirmedCompletion(std::string("vkQueueSubmit に失敗 (VkResult=") +
                                  vk::to_string(result) + ")");
    } catch (const std::exception& e) {
      throw UnconfirmedCompletion(std::string("vkQueueSubmit に失敗: ") + e.what());
    }
    // 以降の失敗は GPU が触った後でありうる。フェンス待ち・waitIdle の失敗は
    // 完了を確認できないため Done を送らず、呼び出し側は新規受付を止めて
    // 非ゼロ終了する。timeout は返却許可ではない。
    vk::Result fence_result = vk::Result::eSuccess;
    try {
      fence_result = device.waitForFences(*fence_, vk::True, 2000000000u);
    } catch (const vk::SystemError& e) {
      const vk::Result result = static_cast<vk::Result>(e.code().value());
      throw UnconfirmedCompletion(std::string("GPU コピーの完了待ちに失敗 (VkResult=") +
                                  vk::to_string(result) + ")");
    }
    if (fence_result != vk::Result::eSuccess) {
      try {
        device.waitIdle();
      } catch (const vk::SystemError& e) {
        const vk::Result result = static_cast<vk::Result>(e.code().value());
        throw UnconfirmedCompletion(std::string("GPU コピーの待避 waitIdle に失敗 (VkResult=") +
                                    vk::to_string(result) + ")");
      } catch (const std::exception& e) {
        throw UnconfirmedCompletion(std::string("GPU コピーの待避 waitIdle に失敗: ") + e.what());
      }
      // waitIdle 成功時は GPU が触っていないことが確定したため、保留資源を破棄してよい。
      ReleaseUnconfirmedCounted();
      throw UnconfirmedCompletion("GPU コピーの完了待ちが 2 秒でタイムアウト");
    }
    ownDefined_ = true;
    // コピー完了を確認できたため、保留していた import 資源を即座に破棄する（従来どおり）。
    ReleaseUnconfirmedCounted();
    return kStatusOk;
  } catch (const UnconfirmedCompletion& e) {
    // 完了を確認できない。Done を送ると main が release してしまうため、
    // 内部番兵で返し、呼び出し側に受付停止・切断・非ゼロ終了させる。
    // 保留中の import 資源は GPU 使用中の可能性があるため破棄せず保持し、
    // プロセス終了時の OS/ドライバ回収に委ねる（意図的リーク）。
    Error("Offer の GPU 完了が未確認のため ACK しない: %s", e.what());
    if (unconfirmedImage_ || unconfirmedMemory_) {
      Error("import 資源を保持したまま終了する（完了未確認のため破棄しない）");
    }
    return kStatusNoAck;
  } catch (const std::exception& e) {
    // submit 前の失敗（GPU は触っていない）のため、保留資源があれば破棄してよい。
    ReleaseUnconfirmedCounted();
    Warn("Offer 処理に失敗: %s", e.what());
    return kStatusCopyFailed;
  }
}

}  // namespace xrift::transfer

#else

#include <stdexcept>

#include "transfer/shared_texture.h"

namespace xrift::transfer {

TransferSession::TransferSession() : instance_(), vulkan_(instance_) {
  throw std::runtime_error("共有テクスチャ転送は Windows のみ（Linux は将来対応）");
}
TransferSession::~TransferSession() = default;
uint32_t TransferSession::ProcessOffer(uint64_t, uint32_t, uint32_t) {
  throw std::runtime_error("共有テクスチャ転送は Windows のみ（Linux は将来対応）");
}
void TransferSession::EnsureOwnImage(uint32_t, uint32_t) {}
void TransferSession::ReleaseUnconfirmedCounted() {}

}  // namespace xrift::transfer

#endif
