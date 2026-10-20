#pragma once

#include <string>

#include "transfer/protocol.h"

// 名前付きパイプのクライアント側。main プロセスがサーバ、exe が接続する。
// 受信はブロッキングでよい（受信ループは専用スレッド相当の main スレッドで行う）。
// Windows 専用。Linux 対応時に FD 転送へ差し替える境界がこのクラスである。

namespace xrift::transfer {

class PipeClient {
 public:
  // pipeName は `\\.\pipe\<name>` 形式。最大 5 秒リトライして接続する。
  // 接続できなければ std::runtime_error を投げる。
  explicit PipeClient(const std::string& pipeName);
  ~PipeClient();

  PipeClient(const PipeClient&) = delete;
  PipeClient& operator=(const PipeClient&) = delete;

  // 64 バイトちょうどを書き切る。失敗時は std::runtime_error を投げる。
  void Send(const TransferMessage& msg);
  // 64 バイトちょうどを読み切る。相手の正常切断時は false を返す。
  // それ以外の失敗時は std::runtime_error を投げる。
  bool Receive(TransferMessage* out);
  // 期限付きの受信。deadlineUs が 0 なら Receive と同じく無期限に待つ。
  // 戻り値は true=1 件受信、false=切断または期限切れ。timedOut に期限切れかを出す。
  // 到着待ちは overlapped I/O + イベント待機で行い、sleep ポーリングは使わない。
  // 部分受信（64B 未満）は内部に溜めて次回に継続する。期限は終了判定のみであり、
  // 返却許可にはならない。
  bool ReceiveWithDeadline(TransferMessage* out, uint64_t deadlineUs, bool* timedOut);

 private:
  void* handle_ = nullptr;  // HANDLE。Windows.h の漏洩を避けるため void* で持つ。
  void* io_ = nullptr;      // IoState*（OVERLAPPED・イベント・受信ステージング）。同上。
};

}  // namespace xrift::transfer
