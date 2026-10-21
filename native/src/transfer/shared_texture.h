#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "common/xr_vulkan.h"
#include "gfx/vulkan_context.h"
#include "xr/instance.h"

// Electron の OSR 共有テクスチャ（別プロセスの NT ハンドル複製済み）を、
// XR ランタイム指定 GPU の Vulkan デバイスへ import し、自前イメージへ GPU コピーする。
// 所有権は取らない。受領した複製ハンドルは import 成否に関わらず閉じる。
// 元の Electron 側ハンドルには触らない（閉じない）。
// Windows 専用。Linux 対応時に輸送部分を差し替える境界がこのクラスである。

namespace xrift::transfer {

class TransferSession {
 public:
  TransferSession();
  ~TransferSession();

  TransferSession(const TransferSession&) = delete;
  TransferSession& operator=(const TransferSession&) = delete;

  // 1 件の Offer を処理する。remoteHandle は exe プロセス内で有効な複製値。
  // 戻り値は protocol.h の kStatus*（kStatusOk / kStatusCopyFailed / kStatusVerifyFailed）
  // または内部番兵 kStatusNoAck。kStatusNoAck は「GPU 完了を確認できない」合図であり、
  // 呼び出し側は Done を送らず、新規受付を止めて接続を閉じ、非ゼロ終了すること。
  // timeout・切断・終了通知は返却許可にならない（DESIGN §5〜§6・§8）。
  // 受信ループは kStatusNoAck 以外では止めない。呼び出し側が Done として返す。
  uint32_t ProcessOffer(uint64_t remoteHandle, uint32_t width, uint32_t height);

  const std::string& gpuName() const { return vulkan_.gpuName(); }

  // XR 提出モード用の読み口。自前イメージ（横並びアトラスの最新コピー）と
  // 寸法・Vulkan 資源を借用する。所有権は移らない。破棄してはならない。
  // 自前イメージは GENERAL 駐めが不変条件であり、借用側は読み後に GENERAL へ戻すこと。
  vk::Image OwnImage() const {
    const vk::Image img = *ownImage_;
    return (static_cast<VkImage>(img) != VK_NULL_HANDLE) ? img : vk::Image{};
  }
  uint32_t OwnWidth() const { return ownWidth_; }
  uint32_t OwnHeight() const { return ownHeight_; }
  const gfx::VulkanContext& Vulkan() const { return vulkan_; }
  // XR セッション共有用の XR インスタンス。XR 提出モードは転送と XR で
  // 同一 VkDevice を使うため、TransferSession が作った instance を借用する。
  // 所有権は移らない。TransferSession より長生きさせてはならない。
  const xr::Instance& XrInstance() const { return instance_; }

 private:
  // 自前イメージを extent に合わせる。変化時のみ作り直す。
  void EnsureOwnImage(uint32_t width, uint32_t height);
  // 保留中の import 資源を破棄する。保持していない場合は何もしない。
  // 完了未確認の保持とは区別する（完了未確認時は呼ばず、OS/ドライバ回収に委ねる）。
  void ReleaseUnconfirmedCounted();

  xr::Instance instance_;
  gfx::VulkanContext vulkan_;

  vk::raii::CommandPool pool_{nullptr};
  vk::raii::CommandBuffer copyCmd_{nullptr};
  vk::raii::Fence fence_{nullptr};

  vk::raii::Image ownImage_{nullptr};
  vk::raii::DeviceMemory ownMemory_{nullptr};
  uint32_t ownWidth_ = 0;
  uint32_t ownHeight_ = 0;
  bool ownDefined_ = false;  // 自前イメージが UNDEFINED 脱出済みか（レイアウト追跡用）。

  // GPU 完了を確認できない Offer の import 資源の保留先。完了未確認のまま破棄すると
  // 使用中の資源を壊すため、フェンス完了か waitIdle 成功を確認できるまで保持する。
  // 確認できないまま終了する場合は解放せず、プロセス終了時の OS/ドライバ回収に委ねる
  // （意図的リーク。理由は ProcessOffer とデストラクタのコメントを参照）。
  // 成功時・submit 前の失敗時（GPU は触っていない）は従来どおり即座に破棄する。
  std::unique_ptr<vk::raii::Image> unconfirmedImage_;
  std::unique_ptr<vk::raii::DeviceMemory> unconfirmedMemory_;
};

}  // namespace xrift::transfer
