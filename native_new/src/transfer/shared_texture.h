#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "common/xr_vulkan.h"
#include "gfx/vulkan_context.h"
#include "transfer/gpu_check.h"
#include "xr/instance.h"

// Electron の OSR 共有テクスチャ（別プロセスの NT ハンドル複製済み）を、
// XR ランタイム指定 GPU の Vulkan デバイスへ import し、自前イメージへ GPU コピーする。
// 所有権は取らない。受領した複製ハンドルは import 成否に関わらず閉じる。
// 元の Electron 側ハンドルには触らない（閉じない）。
// Windows 専用。Linux 対応時に輸送部分を差し替える境界がこのクラスである。

namespace xrift::transfer {

// 転送モード。借用台帳・ACK・release はどのモードでも同じ経路を通す
// （呼び出し側は変えない）。
// 既定は Vulkan であり、D3D11 デバイスを作らない。
// 理由: D3D11 の OpenSharedResource1 を毎フレーム呼ぶ経路は実測で持続不能である
// （4096x2048・90Hz で Private Bytes が約 +16MB/分で増加し続ける）。
// 一方 Vulkan 経路（Offer の width/height/format を信じて import し、
// 全画面 GPU 検査で妥当性を担保する）は 10 分で平坦（Private Bytes 96.2→97.2MB、
// imports=frees）である。Desc 照合を外しても、不正な寸法・形式は import・コピー・
// GPU 検査のいずれかで失敗側に回るため黙って通らない。
enum class TransferMode {
  Vulkan,  // 既定。D3D11 を使わず Offer 寸法で Vulkan import → コピー → 検査。
  // D3D11 Open 経路の診断専用。Vulkan は使わない。
  // 長時間動かすとメモリが増える（実測約 +16MB/分）ため、切り分け以外の用途では使わない。
  D3dOpenOnly,  // 受領ハンドルを D3D11 で開いて Desc を読み、即閉じる。Vulkan は使わない。
  NoGpu,        // 受領したら GPU API を一切使わず即 Done（借用の返却だけ）。
};

// --mode の値を列挙に直す。不正時は理由付きの例外にする（黙って既定にしない）。
TransferMode ParseTransferMode(const std::string& mode);
const char* TransferModeName(TransferMode mode);

class TransferSession {
 public:
  struct Options {
    bool verify = true;    // 部分 readback による画素検証を行うか（デバッグ用）。
    bool gpuCheck = true;  // 全画面 GPU 検査を行うか（既定 on。M1b の本命）。
    // 検証用の障害注入フック。何件目の Offer で完了未確認を強制するか（既定 0=無効）。
    // 通常経路では使わない。--fail-after と同様に 0 は未到達である。
    int injectUnconfirmed = 0;
    TransferMode mode = TransferMode::Vulkan;  // 既定は Vulkan 経路。
  };

  struct Timings {
    uint64_t importUs = 0;  // D3D11 照合 + import 用 image 生成 + allocate + bind。
    uint64_t copyUs = 0;    // 自前イメージへのコピー提出とフェンス待ち。
    uint64_t verifyUs = 0;  // 検証用 readback と判定（無効時は 0）。
    uint64_t checkUs = 0;   // 全画面 GPU 検査の提出・待ち・結果読み（無効時は 0）。
    // 全画面 GPU 検査を実際に完了したか。失敗フレームを「検査済み」に数えない
    // ための合図であり、呼び出し側の統計で使う（submit 前の失敗時は偽のまま）。
    bool checkRan = false;
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
  // timings が非 null なら内訳を出す。gpuReport が非 null かつ GPU 検査が有効なら
  // 検査結果を出す（無効時は既定値のまま）。
  uint32_t ProcessOffer(uint64_t remoteHandle, uint32_t width, uint32_t height,
                        Timings* timings, GpuChecker::Report* gpuReport = nullptr);

  const std::string& gpuName() const { return vulkan_.gpuName(); }

 private:
  // 自前イメージを extent に合わせる。変化時のみ作り直す。
  void EnsureOwnImage(uint32_t width, uint32_t height);
  // 保留中の import 資源を破棄し、生存計数と free 累計を更新する。
  // 保持していない場合は何もしない。完了未確認の保持とは区別する
  // （完了未確認時は呼ばず、OS/ドライバ回収に委ねる）。
  void ReleaseUnconfirmedCounted();
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
  uint64_t ownImageBytes_ = 0;   // 生存計数の引き当て用（自前イメージの要求バイト数）。
  uint64_t ownMemoryBytes_ = 0;  // 同上（自前メモリの確保バイト数）。
  uint32_t ownWidth_ = 0;
  uint32_t ownHeight_ = 0;
  bool ownDefined_ = false;  // 自前イメージが UNDEFINED 脱出済みか（レイアウト追跡用）。

  vk::raii::Buffer verifyBuffer_{nullptr};
  vk::raii::DeviceMemory verifyBufferMemory_{nullptr};

  // M1b 全画面 GPU 検査。--no-gpu-check では作らない（無効経路の負荷をゼロにする）。
  // 破棄順の担保: vulkan_ より後に宣言し、先に破棄する。
  std::unique_ptr<GpuChecker> checker_;

  // GPU 完了を確認できない Offer の import 資源の保留先。完了未確認のまま破棄すると
  // 使用中の資源を壊すため、フェンス完了か waitIdle 成功を確認できるまで保持する。
  // 確認できないまま終了する場合は解放せず、プロセス終了時の OS/ドライバ回収に委ねる
  // （意図的リーク。理由は ProcessOffer とデストラクタのコメントを参照）。
  // 成功時・submit 前の失敗時（GPU は触っていない）は従来どおり即座に破棄する。
  std::unique_ptr<vk::raii::Image> unconfirmedImage_;
  std::unique_ptr<vk::raii::DeviceMemory> unconfirmedMemory_;
  uint64_t unconfirmedImageBytes_ = 0;   // 保留資源の生存計数の引き当て用。
  uint64_t unconfirmedMemoryBytes_ = 0;  // 同上。

  int64_t lastFrame_ = -1;  // 検証で復号した前回のフレーム番号。未受信時は -1。
  bool loggedDesc_ = false;
  uint64_t offerCount_ = 0;  // 注入フック用の処理件数（ProcessOffer 到達ごとに加算）。
};

}  // namespace xrift::transfer
