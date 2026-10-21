#pragma once

#include <cstdint>
#include <string>

// M2 の XR 提出モード。XR セッションを開始し、フレームループで描画レコードを
// 発行して RenderRequest を送り、受理した画像だけを左右 swapchain へ blit して
// そのレコードの pose/FOV で xrEndFrame する（DESIGN §4・§7・§9・§11 M2）。
// --transfer（セッションなし）と通常起動（クリア）は壊さない。別モードである。

namespace xrift::transfer {

struct XrTransferOptions {
  std::string pipeName;         // `\\.\pipe\<name>` 形式。必須。
  double renderScale = 0.5;     // 左右各眼の描画倍率（0.2〜1.0）。既定 0.5。
  uint64_t recordTtlMs = 500;   // 描画レコードの失効上限（ms）。既定 500。
                                // 実測: 本番サイトで 350〜500ms の遅延のため、
                                // app 経由の通常運用は暫定 2000 で渡す。
  int seconds = 0;              // 正ならその秒数で打ち切る。0 は HMD 終了まで。
  int injectStaleRecord = 0;    // N フレーム目の RenderRequest に旧 pose 混入の合図を載せる
                                // （harness 側フック用。既定 0=無効）。
  bool submitVerify = true;     // blit 後の左右分離 readback を行うか（既定 on）。
  std::string dumpSubmitDir;      // N 回目の提出時に両眼 swapchain を PPM 保存する先（既定空=無効）。
  int dumpFrame = 60;             // 保存する提出回（1 起点。既定 60）。dumpSubmitDir が空なら無視。
  std::string space = "auto";   // 参照空間の要求（auto/local/stage/local-floor。M4）。
  int injectSessionRestartMs = 0;  // 起動からその ms 後に意図的にセッションを破棄・再生成する
                                   // （復帰経路の検証用。既定 0=無効）。
  int injectSpaceChangeMs = 0;  // 起動からその ms 後に模擬の space 変更を 1 回適用する
                                // （spaceEpoch 更新・旧レコード失効の経路検証用。
                                // 実イベントではないことをログに明記し、統計の space変更に計上する。
                                // 既定 0=無効）。
  int producerIdleTimeoutMs = 5000;  // Offer 途絶で producer 終了とみなす無受信の上限（ミリ秒）。
                                     // 0 は無効（パイプ切断でのみ終了）。既定 5000 はハーネス向け。
  bool paceRecords = false;
  // 新規受理が無いフレームでも最後の受理画像を同じ pose/FOV で出し直すか（既定 on）。
  // off は旧挙動（layerCount=0 の空フレーム。比較用）。付け替えは行わない。
  bool resubmit = true;
  // 再提出を許す画像年齢の上限（ms）。既定 0=無制限。
  // 正なら受理起点で年齢を超えた再提出は行わず空フレームにする（診断用）。
  // 新規受理の record-ttl とは別枠であり、黒より古い絵を優先するため既定では制限しない。
  uint64_t resubmitMaxAgeMs = 0;  // 実パイプラインに合わせたレコード発行のフロー制御（既定 off）。
                             // on のとき未処理の画像がある間は新しいレコードを発行せず、
                             // Offer 処理後に次のフレームで発行する。応答停滞時は
                             // 新しいレコードで詰まりを解く（DESIGN §7）。
                             // off は現行どおり（ハーネスの 90fps 検証を保つ）。
};

// XR 提出ループを実行する。成功時は 0 を返す。
int RunXrTransfer(const XrTransferOptions& options);

}  // namespace xrift::transfer
