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
  double renderScale = 0.5;     // 左右各眼の描画倍率（0.25〜1.0）。既定 0.5。
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
};

// XR 提出ループを実行する。成功時は 0 を返す。
int RunXrTransfer(const XrTransferOptions& options);

}  // namespace xrift::transfer
