#pragma once

#include <cstdint>
#include <string>

// XR 提出モード。XR セッションを開始し、フレームループで描画レコードを
// 発行して RenderRequest を送り、受理した画像だけを左右 swapchain へ blit して
// そのレコードの pose/FOV で xrEndFrame する（docs/architecture.md「経路」「再提出」）。

namespace xrift::transfer {

struct XrTransferOptions {
  std::string pipeName;         // `\\.\pipe\<name>` 形式。必須。
  double renderScale = 0.5;     // 左右各眼の描画倍率（0.2〜1.0）。既定 0.5。
  uint64_t recordTtlMs = 500;   // 描画レコードの失効上限（ms）。既定 500。
                                // 実測: 本番サイトで 350〜500ms の遅延のため、
                                // app 経由の通常運用は暫定 2000 で渡す。
  int seconds = 0;              // 正ならその秒数で打ち切る。0 は HMD 終了まで。
  std::string space = "auto";   // 参照空間の要求（auto/local/stage/local-floor）。
};

// 固定の製品動作（引数では変えない）。
// レコード発行はフロー制御つき（未処理の画像がある間は発行せず、Offer 処理後の
// 次のフレームで発行する。応答が 500ms 停滞したら新しいレコードで詰まりを解く）。
// Offer 途絶での終了判定は行わない（パイプ切断でのみ終了する）。
// 再提出は常時 on・年齢無制限（新規受理が無いフレームは最終受理を同じ pose/FOV で出す）。
// 提出の左右分離照合（小領域マーカー）は常時 on である。
// 終了時は統計出力と明示 teardown の後に残りの破棄を省略して即時終了する。

// XR 提出ループを実行する。成功時は 0 を返す。
int RunXrTransfer(const XrTransferOptions& options);

}  // namespace xrift::transfer
