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
  struct Options {
    bool verify = true;  // 部分 readback による画素検証を行うか。
  };

  struct Timings {
    uint64_t importUs = 0;  // D3D11 照合 + import 用 image 生成 + allocate + bind。
    uint64_t copyUs = 0;    // 自前イメージへのコピー提出とフェンス待ち。
    uint64_t verifyUs = 0;  // 検証用 readback と判定（無効時は 0）。
  };

  explicit TransferSession(const Options& options);
  ~TransferSession();

  TransferSession(const TransferSession&) = delete;
  TransferSession& operator=(const TransferSession&) = delete;

  // 1 件の Offer を処理する。remoteHandle は exe プロセス内で有効な複製値。
  // 戻り値は protocol.h の kStatus*（kStatusOk / kStatusCopyFailed / kStatusVerifyFailed）
  // または内部番兵 kStatusNoAck。kStatusNoAck は「GPU 完了を確認できない」合図であり、
  // 呼び出し側は Done を送らず、新規受付を止めて接続を閉じ、非ゼロ終了すること。
  // timeout・切断・終了通知は返却許可にならない（DESIGN §5〜§6・§8）。
  // 受信ループは kStatusNoAck 以外では止めない。呼び出し側が Done として返す。
  // timings が非 null なら内訳を出す。
  uint32_t ProcessOffer(uint64_t remoteHandle, uint32_t width, uint32_t height,
                        Timings* timings);

  const std::string& gpuName() const { return vulkan_.gpuName(); }

 private:
  // 自前イメージを extent に合わせる。変化時のみ作り直す。
  void EnsureOwnImage(uint32_t width, uint32_t height);
  // D3D11 として開けることと実 Desc が Offer と一致することの照合（所有は取らない）。
  void ValidateHandle(uint64_t remoteHandle, uint32_t width, uint32_t height);
  // 自前イメージの一部分だけを readback して検証する。検証用途に隔離した読み戻しであり、
  // 本番経路には入れない。戻り値は kStatusOk / kStatusVerifyFailed。
  // 例外は呼び出し側で kStatusCopyFailed に丸める。
  uint32_t VerifyLocked(uint32_t width, uint32_t height);

  Options options_;
  xr::Instance instance_;
  gfx::VulkanContext vulkan_;
  void* d3dDevice_ = nullptr;  // ID3D11Device1*。Windows.h の漏洩を避けるため void* で持つ。
  void* d3dContext_ = nullptr;  // ID3D11DeviceContext*。同上。

  vk::raii::CommandPool pool_{nullptr};
  vk::raii::CommandBuffer copyCmd_{nullptr};
  vk::raii::CommandBuffer verifyCmd_{nullptr};
  vk::raii::Fence fence_{nullptr};

  vk::raii::Image ownImage_{nullptr};
  vk::raii::DeviceMemory ownMemory_{nullptr};
  uint32_t ownWidth_ = 0;
  uint32_t ownHeight_ = 0;
  bool ownDefined_ = false;  // 自前イメージが UNDEFINED 脱出済みか（レイアウト追跡用）。

  vk::raii::Buffer verifyBuffer_{nullptr};
  vk::raii::DeviceMemory verifyBufferMemory_{nullptr};

  // GPU 完了を確認できない Offer の import 資源の保留先。完了未確認のまま破棄すると
  // 使用中の資源を壊すため、フェンス完了か waitIdle 成功を確認できるまで保持する。
  // 確認できないまま終了する場合は解放せず、プロセス終了時の OS/ドライバ回収に委ねる
  // （意図的リーク。理由は ProcessOffer とデストラクタのコメントを参照）。
  // 成功時・submit 前の失敗時（GPU は触っていない）は従来どおり即座に破棄する。
  std::unique_ptr<vk::raii::Image> unconfirmedImage_;
  std::unique_ptr<vk::raii::DeviceMemory> unconfirmedMemory_;

  int64_t lastFrame_ = -1;  // 検証で復号した前回のフレーム番号。未受信時は -1。
  bool loggedDesc_ = false;
};

}  // namespace xrift::transfer
