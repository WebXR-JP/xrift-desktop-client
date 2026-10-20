// shared_texture の実体。Windows 専用であるため、非 Windows では説明文だけにして
// ビルドを通す（Linux の輸送実装は将来の差し替え対象）。
// VK_USE_PLATFORM_WIN32_KHR は CMake が Windows ビルド全体に定義する。
#ifdef _WIN32

#include "transfer/shared_texture.h"

#include <windows.h>

#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/log.h"
#include "common/xr_result.h"
#include "transfer/mem_log.h"
#include "transfer/protocol.h"

namespace xrift::transfer {

TransferMode ParseTransferMode(const std::string& mode) {
  if (mode == "vulkan") {
    return TransferMode::Vulkan;
  }
  if (mode == "d3d-open-only") {
    return TransferMode::D3dOpenOnly;
  }
  if (mode == "no-gpu") {
    return TransferMode::NoGpu;
  }
  throw std::runtime_error("不明な --mode=" + mode +
                           "（vulkan / d3d-open-only / no-gpu のいずれか）");
}

const char* TransferModeName(TransferMode mode) {
  switch (mode) {
    case TransferMode::Vulkan:
      return "vulkan";
    case TransferMode::D3dOpenOnly:
      return "d3d-open-only";
    case TransferMode::NoGpu:
      return "no-gpu";
  }
  return "不明";
}
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

uint64_t NowUs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// GPU 完了を確認できない失敗（フェンス待ち失敗・device lost・waitIdle 失敗）の合図。
// timeout は返却許可ではないため、呼び出し側は Done を送らず新規受付を止める。
// submit 前の失敗（GPU は触っていない）や完了確認済みの失敗とは区別する。
class UnconfirmedCompletion : public std::runtime_error {
 public:
  explicit UnconfirmedCompletion(const std::string& message) : std::runtime_error(message) {}
};

// Vulkan 物理デバイスの deviceLUID に一致する DXGI アダプタ上で D3D11 デバイスを作る。
// ID3D11Device1* と ID3D11DeviceContext* を出す。失敗時は理由付きの例外にする。
// 既定アダプタ（nullptr 指定）での生成はしない。別 GPU で開くと同一 GPU の検証に
// ならないためである。
void CreateD3D11DeviceOnVulkanAdapter(const vk::raii::PhysicalDevice& physical,
                                     ID3D11Device1** outDevice, ID3D11DeviceContext** outContext) {
  *outDevice = nullptr;
  *outContext = nullptr;
  using IdChain = vk::StructureChain<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceIDProperties>;
  IdChain chain{};
  InvokeVk("vkGetPhysicalDeviceProperties2", [&] {
    chain = physical.template getProperties2<vk::PhysicalDeviceProperties2,
                                             vk::PhysicalDeviceIDProperties>();
  });
  const vk::PhysicalDeviceIDProperties& id = chain.template get<vk::PhysicalDeviceIDProperties>();
  if (id.deviceLUIDValid != VK_TRUE) {
    throw std::runtime_error("Vulkan 物理デバイスに有効な deviceLUID が無い");
  }
  char luid_text[32]{};
  for (int i = 0; i < 8; ++i) {
    std::snprintf(luid_text + i * 2, sizeof(luid_text) - static_cast<size_t>(i) * 2, "%02x",
                  id.deviceLUID[static_cast<size_t>(i)]);
  }
  Info("Vulkan deviceLUID=%s", luid_text);

  IDXGIFactory1* factory = nullptr;
  HRESULT hr = ::CreateDXGIFactory1(IID_IDXGIFactory1, reinterpret_cast<void**>(&factory));
  if (FAILED(hr) || factory == nullptr) {
    throw std::runtime_error("DXGI ファクトリの生成に失敗 (HRESULT=" +
                             std::to_string(static_cast<uint32_t>(hr)) + ")");
  }
  IDXGIAdapter1* match = nullptr;
  for (UINT i = 0;; ++i) {
    IDXGIAdapter1* adapter = nullptr;
    hr = factory->EnumAdapters1(i, &adapter);
    if (hr == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    if (FAILED(hr) || adapter == nullptr) {
      continue;
    }
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
      // LUID は LowPart（下位 4 バイト）+ HighPart（上位 4 バイト）の 8 バイトであり、
      // Vulkan の deviceLUID とバイト列で一致する（同一アダプタの同一値）。
      uint8_t bytes[8]{};
      std::memcpy(bytes, &desc.AdapterLuid.LowPart, 4);
      std::memcpy(bytes + 4, &desc.AdapterLuid.HighPart, 4);
      if (std::memcmp(bytes, id.deviceLUID.data(), 8) == 0) {
        match = adapter;
        Info("DXGI 一致: %ws (LUID 一致)", desc.Description);
        break;
      }
    }
    adapter->Release();
  }
  factory->Release();
  if (match == nullptr) {
    throw std::runtime_error("Vulkan deviceLUID に一致する DXGI アダプタが無い");
  }
  // アダプタ指定時は DriverType を UNKNOWN にする（既定アダプタの nullptr 指定はしない）。
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  hr = ::D3D11CreateDevice(match, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                           &dev, nullptr, &ctx);
  match->Release();
  if (FAILED(hr) || dev == nullptr || ctx == nullptr) {
    if (dev != nullptr) {
      dev->Release();
    }
    if (ctx != nullptr) {
      ctx->Release();
    }
    throw std::runtime_error("転送用の D3D11 デバイス生成に失敗 (HRESULT=" +
                             std::to_string(static_cast<uint32_t>(hr)) + ")");
  }
  ID3D11Device1* dev1 = nullptr;
  hr = dev->QueryInterface(IID_ID3D11Device1, reinterpret_cast<void**>(&dev1));
  dev->Release();
  if (FAILED(hr) || dev1 == nullptr) {
    ctx->Release();
    throw std::runtime_error("ID3D11Device1 の取得に失敗");
  }
  *outDevice = dev1;
  *outContext = ctx;
}

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

// 検証パターンの配置（harness/electron/index.html と一致させること）。
constexpr uint32_t kCornerBlock = 64;   // 4 隅の固定色ブロックの一辺。
constexpr uint32_t kFrameOrgX = 64;     // フレーム番号ブロックの左上 x。
constexpr uint32_t kFrameOrgY = 64;     // フレーム番号ブロックの左上 y。
constexpr uint32_t kFrameCellsX = 8;    // 横 8 セル。
constexpr uint32_t kFrameCellsY = 2;    // 縦 2 セル（合計 16bit）。
constexpr uint32_t kCell = 4;           // 1 セルの一辺。
constexpr uint32_t kFrameBlock = 32;    // フレーム番号ブロックの一辺。
constexpr size_t kVerifyBufferSize = 16 + kFrameBlock * kFrameBlock * 4;  // 隅 4 画素 + 32x32。

bool IsRed(uint8_t b, uint8_t g, uint8_t r) { return r > 200 && g < 55 && b < 55; }
bool IsGreen(uint8_t b, uint8_t g, uint8_t r) { return g > 200 && r < 55 && b < 55; }
bool IsBlue(uint8_t b, uint8_t g, uint8_t r) { return b > 200 && r < 55 && g < 55; }
bool IsWhite(uint8_t b, uint8_t g, uint8_t r) { return r > 200 && g > 200 && b > 200; }

}  // namespace

TransferSession::TransferSession(const Options& options) : options_(options), vulkan_(instance_, RequiredExtensionList()) {
  const vk::raii::PhysicalDevice& physical = vulkan_.physicalDevice();
  const vk::raii::Device& device = vulkan_.device();

  // 物理デバイスの API 版を確認する。外部形式の問い合わせは 1.1 以降の core 関数を使う。
  const uint32_t apiVersion = physical.getProperties().apiVersion;
  if (apiVersion < VK_API_VERSION_1_1) {
    throw std::runtime_error("転送には Vulkan 1.1 以降の物理デバイスが必要");
  }

  // 必須のデバイス拡張の有無は VulkanContext が vkCreateDevice の前に名指しで確認する。
  // ここでは物理デバイスの API 版だけを確認する。

  // D3D11 照合用デバイスは d3d-open-only の診断時にだけ作る。既定の Vulkan 経路では
  // D3D11CreateDevice すら呼ばない（毎フレームの OpenSharedResource1 が実測で
  // 持続不能なため。約 +16MB/分。詳細は shared_texture.h の TransferMode を参照）。
  // 別 GPU で開けてしまうと同一 GPU の検証にならないため、失敗はそのまま返す。
  // no-gpu モードでは GPU API を一切使わないため作らない（帰属切り分けの起点）。
  // Vulkan 経路では D3D11 を一切使わない。Offer の寸法・形式を信じて Vulkan 側だけで
  // import する。
  ID3D11Device1* dev1 = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  if (options_.mode == TransferMode::D3dOpenOnly) {
    CreateD3D11DeviceOnVulkanAdapter(physical, &dev1, &ctx);
  }
  d3dDevice_ = dev1;
  d3dContext_ = ctx;

  vk::CommandPoolCreateInfo pool_info{};
  pool_info.sType = vk::StructureType::eCommandPoolCreateInfo;
  pool_info.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
  pool_info.queueFamilyIndex = vulkan_.queueFamilyIndex();
  InvokeVk("vkCreateCommandPool", [&] { pool_ = vk::raii::CommandPool(device, pool_info); });

  vk::CommandBufferAllocateInfo alloc{};
  alloc.sType = vk::StructureType::eCommandBufferAllocateInfo;
  alloc.commandPool = *pool_;
  alloc.level = vk::CommandBufferLevel::ePrimary;
  alloc.commandBufferCount = 2;
  InvokeVk("vkAllocateCommandBuffers", [&] {
    vk::raii::CommandBuffers buffers = device.allocateCommandBuffers(alloc);
    copyCmd_ = std::move(buffers[0]);
    verifyCmd_ = std::move(buffers[1]);
  });

  vk::FenceCreateInfo fence_info{};
  fence_info.sType = vk::StructureType::eFenceCreateInfo;
  InvokeVk("vkCreateFence", [&] { fence_ = vk::raii::Fence(device, fence_info); });

  // M1b 全画面 GPU 検査は起動時に確定する。--no-gpu-check では作らず、
  // 無効経路に compute 由来の負荷も失敗要因も残さない。
  // 診断モード（d3d-open-only / no-gpu）では Vulkan を使わないため作らない。
  if (options_.verify) {
    Info("部分検証 (--verify) は有効。デバッグ用の範囲に隔離した読み戻しである");
  }
  if (options_.gpuCheck && options_.mode == TransferMode::Vulkan) {
    checker_ = std::make_unique<GpuChecker>(vulkan_);
  }
  if (options_.mode == TransferMode::D3dOpenOnly) {
    Warn("--mode=d3d-open-only は D3D11 Open 経路の診断専用。長時間動かすとメモリが増える"
         "（実測約 +16MB/分）ため、切り分け以外の用途では使わないこと");
  }
  if (options_.mode == TransferMode::NoGpu) {
    Info("診断モード --mode=no-gpu（Vulkan import/コピー/検査は行わない）");
  }
  if (options_.mode == TransferMode::Vulkan) {
    Info("--mode=vulkan（既定。D3D11 を使わず Offer 寸法で Vulkan import/コピー/検査を行う）");
  }

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
    // 意図的リークのため生存計数は残す（破棄していないことを正直に表す）。
    // プロセス終了時の OS/ドライバ回収に委ねる。
  }
  if (idle_ok) {
    // GPU が触っていないことが確定したため、残っていれば通常破棄する。
    ReleaseUnconfirmedCounted();
  }
  // 自前イメージの破棄に合わせて生存計数を落とす（以降は raii の逆順破棄）。
  if (ownImageBytes_ > 0) {
    TrackVkImageDestroy(ownImageBytes_);
    ownImageBytes_ = 0;
  }
  if (ownMemoryBytes_ > 0) {
    TrackVkMemoryFree(ownMemoryBytes_);
    ownMemoryBytes_ = 0;
  }
  if (d3dContext_ != nullptr) {
    static_cast<ID3D11DeviceContext*>(d3dContext_)->Release();
    d3dContext_ = nullptr;
  }
  if (d3dDevice_ != nullptr) {
    static_cast<ID3D11Device1*>(d3dDevice_)->Release();
    d3dDevice_ = nullptr;
  }
  // 以降は raii の逆順破棄（staging → 自前 → fence → pool → device → XR instance）。
}

void TransferSession::ValidateHandle(uint64_t remoteHandle, uint32_t width, uint32_t height) {
  auto* device = static_cast<ID3D11Device1*>(d3dDevice_);
  ID3D11Texture2D* tex = nullptr;
  const HANDLE h = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(remoteHandle));
  HRESULT hr = device->OpenSharedResource1(h, IID_ID3D11Texture2D,
                                           reinterpret_cast<void**>(&tex));
  if (FAILED(hr) || tex == nullptr) {
    throw std::runtime_error("D3D11 として開けない (HRESULT=" +
                             std::to_string(static_cast<uint32_t>(hr)) + ")");
  }
  D3D11_TEXTURE2D_DESC desc{};
  tex->GetDesc(&desc);
  tex->Release();
  if (!loggedDesc_) {
    loggedDesc_ = true;
    Info("共有テクスチャ実 Desc: %ux%u format=%u mip=%u array=%u", desc.Width, desc.Height,
         static_cast<unsigned>(desc.Format), static_cast<unsigned>(desc.MipLevels),
         static_cast<unsigned>(desc.ArraySize));
  }
  if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
    throw std::runtime_error("共有テクスチャの形式が B8G8R8A8_UNORM ではない");
  }
  if (desc.Width != width || desc.Height != height) {
    throw std::runtime_error("共有テクスチャの実サイズが Offer と不一致");
  }
}

void TransferSession::ReleaseUnconfirmedCounted() {
  const bool held = (unconfirmedImage_ != nullptr) || (unconfirmedMemory_ != nullptr);
  if (unconfirmedMemory_) {
    TrackVkMemoryFree(unconfirmedMemoryBytes_);
    unconfirmedMemory_.reset();
    unconfirmedMemoryBytes_ = 0;
  }
  if (unconfirmedImage_) {
    TrackVkImageDestroy(unconfirmedImageBytes_);
    unconfirmedImage_.reset();
    unconfirmedImageBytes_ = 0;
  }
  if (held) {
    TrackImportFree();
  }
}

void TransferSession::EnsureOwnImage(uint32_t width, uint32_t height) {
  if (ownDefined_ && ownWidth_ == width && ownHeight_ == height) {
    return;
  }
  const vk::raii::Device& device = vulkan_.device();
  // 作り直し前に生存計数を落とす（バイト数は引き当て分と一致させる）。
  if (ownImageBytes_ > 0) {
    TrackVkImageDestroy(ownImageBytes_);
    ownImageBytes_ = 0;
  }
  if (ownMemoryBytes_ > 0) {
    TrackVkMemoryFree(ownMemoryBytes_);
    ownMemoryBytes_ = 0;
  }
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
  ownImageBytes_ = req.size;
  TrackVkImageCreate(req.size);
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
  ownMemoryBytes_ = alloc.allocationSize;
  TrackVkMemoryAlloc(alloc.allocationSize);
  InvokeVk("vkBindImageMemory(自前)",
           [&] { ownImage_.bindMemory(*ownMemory_, 0); });
  ownWidth_ = width;
  ownHeight_ = height;
  ownDefined_ = false;
  // GPU 検査の結び付けは作り直し時だけ行う。同寸法の再利用では結び付けが残る。
  if (checker_) {
    checker_->BindImage(*ownImage_, width, height);
  }
}

uint32_t TransferSession::ProcessOffer(uint64_t remoteHandle, uint32_t width, uint32_t height,
                                      Timings* timings, GpuChecker::Report* gpuReport) {
  Timings local{};
  const uint64_t t0 = NowUs();
  const vk::raii::Device& device = vulkan_.device();
  const vk::raii::Queue& queue = vulkan_.queue();

  try {
    // 受領した複製ハンドルは受領直後から単一の所有ガードで保持し、全経路で閉じる。
    // import 前の失敗（照合・生成の失敗）でも漏らさない。
    DuplicatedHandle owned(remoteHandle);
    ++offerCount_;

    // 前回の完了未確認資源が残っている場合の新規受付は誤りである。
    // 古い保留を上書き破棄するより保持を優先し、ACK せず受付停止に回す。
    if (unconfirmedImage_ || unconfirmedMemory_) {
      throw UnconfirmedCompletion("前回の完了未確認 import 資源が残っているため新規受付を停止");
    }

    // 診断モードの分岐。借用台帳・ACK・release は呼び出し側で同一経路を通すため、
    // ここでは Done の内容（kStatusOk）だけを返し、受領ハンドルは所有ガードが閉じる。
    if (options_.mode == TransferMode::NoGpu) {
      if (timings != nullptr) {
        *timings = local;
      }
      return kStatusOk;
    }
    if (options_.mode == TransferMode::D3dOpenOnly) {
      ValidateHandle(remoteHandle, width, height);
      local.importUs = NowUs() - t0;
      if (timings != nullptr) {
        *timings = local;
      }
      return kStatusOk;
    }

    // 1. D3D11 照合は行わない。Vulkan 経路では Offer の width/height/format を信じて
    // import し、妥当性は Vulkan 側の import・コピーと全画面 GPU 検査で担保する。
    // 不正な寸法・形式は失敗側（kStatusCopyFailed / kStatusVerifyFailed）に回り、
    // 黙って成功にはならない。毎フレームの OpenSharedResource1 を避けるのは、
    // 実測で持続不能（4096x2048・90Hz で約 +16MB/分）のに対し Vulkan 経路は
    // 10 分で平坦（Private Bytes 96.2→97.2MB、imports=frees）なためである。
    // 詳細は shared_texture.h の TransferMode を参照。

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
    // bind 完了で import 1 件の確保対が成立した。生存計数と累計を更新する。
    unconfirmedImageBytes_ = req.size;
    unconfirmedMemoryBytes_ = alloc.allocationSize;
    TrackVkImageCreate(req.size);
    TrackVkMemoryAlloc(alloc.allocationSize);
    TrackImport();
    local.importUs = NowUs() - t0;

    // import 資源は submit 以降の失敗（完了未確認）に備えて保留メンバへ移す。
    // 以降は非所有の表示ハンドル経由で参照し、破棄は「フェンス完了を確認できた」か
    // 「waitIdle が成功した」場合だけ行う。完了未確認時は保持し、プロセス終了時の
    // OS/ドライバ回収に委ねる（意図的リーク）。成功時・submit 前の失敗時は従来どおり
    // 即座に破棄する。
    unconfirmedImage_ = std::make_unique<vk::raii::Image>(std::move(import_image));
    unconfirmedMemory_ = std::make_unique<vk::raii::DeviceMemory>(std::move(import_memory));
    const vk::Image import_view = **unconfirmedImage_;

    // 検証用の障害注入フック（--inject-unconfirmed=N）。N 件目の Offer で完了未確認を
    // 強制する。実際のフェンス失敗と同じ UnconfirmedCompletion 例外で同じ catch 経路
    // （kStatusNoAck→Done なし→受付停止→終了 2）を通すため、別経路は作らない。
    // 注入点を import 資源の保留直後に置くのは、実際の失敗と同じく資源を破棄せず保持する
    // 扱いにするためである（成功後の Done 直前で偽装すると資源は既に解放済みになり、
    // 実経路と状態が一致しなくなる）。既定 0 は未到達である。
    if (options_.injectUnconfirmed > 0 &&
        offerCount_ == static_cast<uint64_t>(options_.injectUnconfirmed)) {
      throw UnconfirmedCompletion("検証フック --inject-unconfirmed=" +
                                  std::to_string(options_.injectUnconfirmed) +
                                  " に到達したため完了未確認として扱う");
    }

    // 5. 自前イメージの用意とコピー。完了はフェンスで待ち、完了後に ACK する。
    EnsureOwnImage(width, height);
    const uint64_t t1 = NowUs();
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

    // 自前イメージは GENERAL で駐める。M2 のサンプリング起点にする。
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
    local.copyUs = NowUs() - t1;
    // コピー完了を確認できたため、保留していた import 資源を即座に破棄する（従来どおり）。
    ReleaseUnconfirmedCounted();

    // 6. 全画面 GPU 検査（--gpu-check の範囲に隔離）。画像全体は読まず、
    // 176 バイトの結果バッファだけを読む。M2 以降の本番経路にも readback を入れない。
    // 不一致は画素検証の失敗として Done に載せる（kStatusVerifyFailed）。
    GpuChecker::Report check_report{};
    if (checker_) {
      const uint64_t t15 = NowUs();
      try {
        check_report = checker_->Check(width, height);
        local.checkRan = true;
      } catch (const UnconfirmedCheck& e) {
        // 検証 submit 後の失敗は完了未確認として上位へ伝える。
        // timeout は返却許可ではない。
        throw UnconfirmedCompletion(std::string("GPU検査の完了が未確認: ") + e.what());
      }
      local.checkUs = NowUs() - t15;
    }
    if (gpuReport != nullptr) {
      *gpuReport = check_report;
    }
    uint32_t status = kStatusOk;
    if (checker_ && check_report.mismatchCount > 0) {
      const GpuChecker::Record& first = check_report.records[0];
      Warn("GPU検査: 不一致 %u 件（復号=%u タイル=%u）。先頭: x=%u y=%u 期待=%08x 実際=%08x 種別=%s",
           static_cast<unsigned>(check_report.mismatchCount),
           static_cast<unsigned>(check_report.decodedFrame),
           static_cast<unsigned>(check_report.checkedTiles), first.x, first.y, first.expected,
           first.actual, MismatchKindName(first.kind));
      status = kStatusVerifyFailed;
    }

    // 7. 部分検証（--verify の範囲に隔離）。画像全体は読まない。
    // 本番経路に readback を入れないこと。M2 以降のサンプリングは GPU 内で行う。
    if (options_.verify) {
      const uint64_t t2 = NowUs();
      const uint32_t verify_status = VerifyLocked(width, height);
      local.verifyUs = NowUs() - t2;
      if (verify_status != kStatusOk) {
        status = verify_status;
      }
    }
    if (timings != nullptr) {
      *timings = local;
    }
    return status;
  } catch (const UnconfirmedCompletion& e) {
    // 完了を確認できない。Done を送ると main が release してしまうため、
    // 内部番兵で返し、呼び出し側に受付停止・切断・非ゼロ終了させる。
    // 保留中の import 資源は GPU 使用中の可能性があるため破棄せず保持し、
    // プロセス終了時の OS/ドライバ回収に委ねる（意図的リーク）。
    Error("Offer の GPU 完了が未確認のため ACK しない: %s", e.what());
    if (unconfirmedImage_ || unconfirmedMemory_) {
      Error("import 資源を保持したまま終了する（完了未確認のため破棄しない）");
    }
    if (timings != nullptr) {
      *timings = local;
    }
    return kStatusNoAck;
  } catch (const std::exception& e) {
    // submit 前の失敗（GPU は触っていない）のため、保留資源があれば破棄してよい。
    ReleaseUnconfirmedCounted();
    Warn("Offer 処理に失敗: %s", e.what());
    if (timings != nullptr) {
      *timings = local;
    }
    return kStatusCopyFailed;
  }
}

uint32_t TransferSession::VerifyLocked(uint32_t width, uint32_t height) {
  // 検証パターンが収まる大きさが無ければ判定不能として失敗にする（黙って通さない）。
  // フレーム番号ブロックは (64,64)-(95,95) を読むため幅・高さとも 96 以上を要求し、
  // 範囲外コピーはしない。隅のサンプル座標も含めて境界を確認する。
  if (width < kFrameOrgX + kFrameBlock || height < kFrameOrgY + kFrameBlock ||
      width < kCornerBlock || height < kCornerBlock) {
    Warn("検証: 画像が小さすぎてパターン判定ができない (%ux%u)", width, height);
    return kStatusVerifyFailed;
  }
  const uint32_t corner_x[4] = {kCornerBlock / 2, width - kCornerBlock / 2, kCornerBlock / 2,
                                width - kCornerBlock / 2};
  const uint32_t corner_y[4] = {kCornerBlock / 2, kCornerBlock / 2, height - kCornerBlock / 2,
                                height - kCornerBlock / 2};
  for (int i = 0; i < 4; ++i) {
    if (corner_x[i] >= width || corner_y[i] >= height) {
      Warn("検証: 隅サンプルが範囲外 (%ux%u)", width, height);
      return kStatusVerifyFailed;
    }
  }
  const vk::raii::Device& device = vulkan_.device();
  const vk::raii::Queue& queue = vulkan_.queue();

  if (static_cast<VkBuffer>(*verifyBuffer_) == VK_NULL_HANDLE) {
    vk::BufferCreateInfo info{};
    info.sType = vk::StructureType::eBufferCreateInfo;
    info.size = kVerifyBufferSize;
    info.usage = vk::BufferUsageFlagBits::eTransferDst;
    info.sharingMode = vk::SharingMode::eExclusive;
    InvokeVk("vkCreateBuffer(検証)", [&] { verifyBuffer_ = vk::raii::Buffer(device, info); });
    // 所要量の問い合わせは 1.0 系の vkGetBufferMemoryRequirements を使う。
    // DeviceBufferMemoryRequirements 経由（1.3 / maintenance4 相当）は使わない。
    const vk::MemoryRequirements req = verifyBuffer_.getMemoryRequirements();
    const vk::PhysicalDeviceMemoryProperties mem_props =
        vulkan_.physicalDevice().getMemoryProperties();
    int32_t index = -1;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
      const vk::MemoryPropertyFlags flags =
          vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
      if ((req.memoryTypeBits & (1u << i)) != 0u &&
          (mem_props.memoryTypes[i].propertyFlags & flags) == flags) {
        index = static_cast<int32_t>(i);
        break;
      }
    }
    if (index < 0) {
      throw std::runtime_error("検証バッファの host-visible メモリタイプが無い");
    }
    vk::MemoryAllocateInfo alloc{};
    alloc.sType = vk::StructureType::eMemoryAllocateInfo;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = static_cast<uint32_t>(index);
    InvokeVk("vkAllocateMemory(検証)",
             [&] { verifyBufferMemory_ = vk::raii::DeviceMemory(device, alloc); });
    InvokeVk("vkBindBufferMemory(検証)",
             [&] { verifyBuffer_.bindMemory(*verifyBufferMemory_, 0); });
  }

  InvokeVk("vkResetFences", [&] { device.resetFences(*fence_); });
  InvokeVk("vkResetCommandBuffer", [&] { verifyCmd_.reset(); });
  vk::CommandBufferBeginInfo begin{};
  begin.sType = vk::StructureType::eCommandBufferBeginInfo;
  begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
  InvokeVk("vkBeginCommandBuffer(検証)", [&] { verifyCmd_.begin(begin); });

  const vk::ImageSubresourceRange range = FullColorRange();
  vk::ImageMemoryBarrier to_src{};
  to_src.sType = vk::StructureType::eImageMemoryBarrier;
  to_src.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
  to_src.dstAccessMask = vk::AccessFlagBits::eTransferRead;
  to_src.oldLayout = vk::ImageLayout::eGeneral;
  to_src.newLayout = vk::ImageLayout::eTransferSrcOptimal;
  to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_src.image = *ownImage_;
  to_src.subresourceRange = range;
  verifyCmd_.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                             vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr, to_src);

  std::array<vk::BufferImageCopy, 5> regions{};
  for (int i = 0; i < 4; ++i) {
    regions[i].bufferOffset = static_cast<vk::DeviceSize>(i * 4);
    regions[i].imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    regions[i].imageSubresource.layerCount = 1;
    regions[i].imageOffset = vk::Offset3D{static_cast<int32_t>(corner_x[i]),
                                          static_cast<int32_t>(corner_y[i]), 0};
    regions[i].imageExtent = vk::Extent3D{1, 1, 1};
  }
  regions[4].bufferOffset = 16;
  regions[4].imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
  regions[4].imageSubresource.layerCount = 1;
  regions[4].imageOffset =
      vk::Offset3D{static_cast<int32_t>(kFrameOrgX), static_cast<int32_t>(kFrameOrgY), 0};
  regions[4].imageExtent = vk::Extent3D{kFrameBlock, kFrameBlock, 1};
  verifyCmd_.copyImageToBuffer(*ownImage_, vk::ImageLayout::eTransferSrcOptimal, *verifyBuffer_,
                               regions);

  // バッファへのコピー後は host 可視性を確保する（TRANSFER_WRITE → HOST_READ）。
  vk::BufferMemoryBarrier host_barrier{};
  host_barrier.sType = vk::StructureType::eBufferMemoryBarrier;
  host_barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
  host_barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
  host_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  host_barrier.buffer = *verifyBuffer_;
  host_barrier.offset = 0;
  host_barrier.size = kVerifyBufferSize;
  verifyCmd_.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                             vk::PipelineStageFlagBits::eHost, {}, nullptr, host_barrier,
                             nullptr);

  vk::ImageMemoryBarrier park = to_src;
  park.srcAccessMask = vk::AccessFlagBits::eTransferRead;
  park.dstAccessMask = vk::AccessFlagBits::eShaderRead;
  park.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
  park.newLayout = vk::ImageLayout::eGeneral;
  verifyCmd_.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                             vk::PipelineStageFlagBits::eFragmentShader, {}, nullptr, nullptr,
                             park);

  InvokeVk("vkEndCommandBuffer(検証)", [&] { verifyCmd_.end(); });
  vk::CommandBuffer raw = *verifyCmd_;
  vk::SubmitInfo submit{};
  submit.sType = vk::StructureType::eSubmitInfo;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &raw;
  // コピー側と同様、submit 失敗は理由を問わず完了未確認として上位へ伝える。
  try {
    queue.submit(submit, *fence_);
  } catch (const vk::SystemError& e) {
    const vk::Result result = static_cast<vk::Result>(e.code().value());
    throw UnconfirmedCompletion(std::string("vkQueueSubmit(検証) に失敗 (VkResult=") +
                                vk::to_string(result) + ")");
  } catch (const std::exception& e) {
    throw UnconfirmedCompletion(std::string("vkQueueSubmit(検証) に失敗: ") + e.what());
  }
  // 検証コピーの完了待ちに失敗したら完了未確認として上位へ伝える。
  // timeout は返却許可ではない。
  vk::Result verify_fence = vk::Result::eSuccess;
  try {
    verify_fence = device.waitForFences(*fence_, vk::True, 2000000000u);
  } catch (const vk::SystemError& e) {
    const vk::Result result = static_cast<vk::Result>(e.code().value());
    throw UnconfirmedCompletion(std::string("検証コピーの完了待ちに失敗 (VkResult=") +
                                vk::to_string(result) + ")");
  }
  if (verify_fence != vk::Result::eSuccess) {
    try {
      device.waitIdle();
    } catch (const vk::SystemError& e) {
      const vk::Result result = static_cast<vk::Result>(e.code().value());
      throw UnconfirmedCompletion(std::string("検証コピーの待避 waitIdle に失敗 (VkResult=") +
                                  vk::to_string(result) + ")");
    } catch (const std::exception& e) {
      throw UnconfirmedCompletion(std::string("検証コピーの待避 waitIdle に失敗: ") + e.what());
    }
    throw UnconfirmedCompletion("検証コピーの完了待ちが 2 秒でタイムアウト");
  }

  void* mapped = nullptr;
  InvokeVk("vkMapMemory(検証)", [&] {
    mapped = verifyBufferMemory_.mapMemory(0, kVerifyBufferSize);
  });
  const uint8_t* bytes = static_cast<const uint8_t*>(mapped);
  bool ok = true;

  // 4 隅の固定色（メモリ順 B,G,R,A）。
  const uint8_t* tl = bytes;
  const uint8_t* tr = bytes + 4;
  const uint8_t* bl = bytes + 8;
  const uint8_t* br = bytes + 12;
  if (!IsRed(tl[0], tl[1], tl[2])) {
    Warn("検証: 左上が赤ではない (B=%u G=%u R=%u)", tl[0], tl[1], tl[2]);
    ok = false;
  }
  if (!IsGreen(tr[0], tr[1], tr[2])) {
    Warn("検証: 右上が緑ではない (B=%u G=%u R=%u)", tr[0], tr[1], tr[2]);
    ok = false;
  }
  if (!IsBlue(bl[0], bl[1], bl[2])) {
    Warn("検証: 左下が青ではない (B=%u G=%u R=%u)", bl[0], bl[1], bl[2]);
    ok = false;
  }
  if (!IsWhite(br[0], br[1], br[2])) {
    Warn("検証: 右下が白ではない (B=%u G=%u R=%u)", br[0], br[1], br[2]);
    ok = false;
  }

  // フレーム番号ブロックの復号とセル単色の確認（ティアリング検出の簡易版）。
  const uint8_t* block = bytes + 16;
  auto at = [&](uint32_t x, uint32_t y) -> const uint8_t* {
    return block + (static_cast<size_t>(y) * kFrameBlock + x) * 4;
  };
  auto bitOf = [](const uint8_t* p) -> int {
    return (static_cast<int>(p[0]) + static_cast<int>(p[1]) + static_cast<int>(p[2])) / 3 > 128
               ? 1
               : 0;
  };
  uint32_t frame = 0;
  for (uint32_t b = 0; b < 16; ++b) {
    const uint32_t cx = (b % kFrameCellsX) * kCell;
    const uint32_t cy = (b / kFrameCellsX) * kCell;
    const int center = bitOf(at(cx + 2, cy + 2));
    frame |= static_cast<uint32_t>(center) << b;
    for (uint32_t dy = 0; dy < kCell; ++dy) {
      for (uint32_t dx = 0; dx < kCell; ++dx) {
        if (bitOf(at(cx + dx, cy + dy)) != center) {
          Warn("検証: フレーム番号セル %u が単色ではない（ティアリングの疑い）", b);
          ok = false;
          dx = kCell;
          dy = kCell;
        }
      }
    }
  }
  if (lastFrame_ >= 0) {
    // 繰り返し paint は同じ番号になりうるため diff 0 は許容する（DESIGN §7）。
    // 16bit 周回を許容し、後退だけを失敗にする。
    const uint32_t diff = (frame - static_cast<uint32_t>(lastFrame_)) & 0xFFFFu;
    if (diff != 0 && diff > 0x7FFFu) {
      Warn("検証: フレーム番号が後退した (前=%d 今=%u)", static_cast<int>(lastFrame_), frame);
      ok = false;
    }
  }
  lastFrame_ = static_cast<int64_t>(frame);

  verifyBufferMemory_.unmapMemory();
  return ok ? kStatusOk : kStatusVerifyFailed;
}

}  // namespace xrift::transfer

#else

#include <stdexcept>

#include "transfer/shared_texture.h"

namespace xrift::transfer {

TransferMode ParseTransferMode(const std::string& mode) {
  if (mode == "vulkan") {
    return TransferMode::Vulkan;
  }
  if (mode == "d3d-open-only") {
    return TransferMode::D3dOpenOnly;
  }
  if (mode == "no-gpu") {
    return TransferMode::NoGpu;
  }
  throw std::runtime_error("不明な --mode=" + mode);
}

const char* TransferModeName(TransferMode mode) {
  switch (mode) {
    case TransferMode::Vulkan:
      return "vulkan";
    case TransferMode::D3dOpenOnly:
      return "d3d-open-only";
    case TransferMode::NoGpu:
      return "no-gpu";
  }
  return "不明";
}

TransferSession::TransferSession(const Options&) : instance_(), vulkan_(instance_) {
  throw std::runtime_error("共有テクスチャ転送は Windows のみ（Linux は将来対応）");
}
TransferSession::~TransferSession() = default;
uint32_t TransferSession::ProcessOffer(uint64_t, uint32_t, uint32_t, Timings*,
                                         GpuChecker::Report*) {
  throw std::runtime_error("共有テクスチャ転送は Windows のみ（Linux は将来対応）");
}
void TransferSession::EnsureOwnImage(uint32_t, uint32_t) {}
void TransferSession::ReleaseUnconfirmedCounted() {}
void TransferSession::ValidateHandle(uint64_t, uint32_t, uint32_t) {}
uint32_t TransferSession::VerifyLocked(uint32_t, uint32_t) { return 0; }

}  // namespace xrift::transfer

#endif
