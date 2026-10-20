#pragma once

#include <string>

// --transfer モードの受信ループ。XR instance + ランタイム指定 GPU の Vulkan device を作り
// （セッションは作らない）、パイプで Offer を受けて import → 自前イメージへコピー →

// 検証 → ACK を返す。借用の所有権は取らず、release は main プロセスの責務である。
// GPU 完了を確認できない Offer には ACK を送らず、新規受付を止めて接続を閉じ、
// 非ゼロ終了する（timeout は返却許可にならない）。

namespace xrift::transfer {

// コマンドラインを引き受けて転送ループを実行する。成功時は 0 を返す。
// pipeName は `\\.\pipe\<name>` 形式。verify が真なら画素検証を行う（既定 on）。
// seconds が正ならその秒数で受信を打ち切り、切断と同様に統計を出して終了する。
// failAfter が正ならその件数の Offer に ACK した直後に異常終了する（検証 5 用の
// 人為的非ゼロ終了フック。既定 0=無効であり、通常経路では使わない）。
int RunTransfer(const std::string& pipeName, bool verify, int seconds, int failAfter = 0);

}  // namespace xrift::transfer
