#include "transfer/pipe_client.h"

#ifdef _WIN32

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

namespace xrift::transfer {
namespace {

// 最後の Win32 エラーを添えた例外文を作る。
std::runtime_error WinError(const char* what) {
  const DWORD code = ::GetLastError();
  char* text = nullptr;
  ::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPSTR>(&text), 0, nullptr);
  std::string message = std::string(what) + " に失敗 (Win32=" + std::to_string(code);
  if (text != nullptr) {
    // 末尾の改行を取り除く。
    std::string detail = text;
    while (!detail.empty() &&
           (detail.back() == '\r' || detail.back() == '\n' || detail.back() == ' ')) {
      detail.pop_back();
    }
    message += ":" + detail;
    ::LocalFree(text);
  }
  message += ")";
  return std::runtime_error(message);
}

uint64_t NowUs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// 切断を示す Win32 エラーか。パイプの相手が消えた場合は例外ではなく切断として返す。
bool IsDisconnect(DWORD code) {
  return code == ERROR_BROKEN_PIPE || code == ERROR_PIPE_NOT_CONNECTED;
}

// overlapped I/O の保持状態。PipeClient のメンバ（io_）が所有する。
// 読みの部分受信（64B 未満）は staged に溜めて次回呼び出しへ継続する。
struct IoState {
  OVERLAPPED readOv{};
  HANDLE readEvent = nullptr;
  OVERLAPPED writeOv{};
  HANDLE writeEvent = nullptr;
  uint8_t staged[sizeof(TransferMessage)]{};
  size_t stagedBytes = 0;
};

void CloseIoState(HANDLE pipe, IoState* io) {
  if (io == nullptr) {
    return;
  }
  // 仕掛かりの非同期 I/O があれば取り消して回収してから閉じる。
  // デストラクタ経由のため例外は出さない。
  if (pipe != nullptr && pipe != INVALID_HANDLE_VALUE) {
    ::CancelIoEx(pipe, nullptr);
  }
  if (io->readEvent != nullptr) {
    ::CloseHandle(io->readEvent);
  }
  if (io->writeEvent != nullptr) {
    ::CloseHandle(io->writeEvent);
  }
  delete io;
}

// 期限までの残りミリ秒を切り上げで求める。INFINITE との衝突を避けるため
// 上限は MAXDWORD - 1 に抑える（deadlineUs が 0 の場合は呼ばない）。
DWORD RemainingMs(uint64_t deadlineUs) {
  const uint64_t now = NowUs();
  if (now >= deadlineUs) {
    return 0;
  }
  uint64_t ms = (deadlineUs - now + 999u) / 1000u;
  if (ms == 0) {
    ms = 1;
  }
  constexpr uint64_t kMax = static_cast<uint64_t>(MAXDWORD) - 1u;
  if (ms > kMax) {
    ms = kMax;
  }
  return static_cast<DWORD>(ms);
}

// 保留中の読みを取り消して回収する。取り消し時点で到着していた分も含め、
// 呼び出し側バッファへ書き込まれたバイト数は失わずに返す。
// 戻り値は回収したバイト数。切断・失敗時は例外ではなく 0 を返す場合がある。
DWORD CollectPendingRead(HANDLE pipe, IoState* io, DWORD* bytesOut) {
  if (!::CancelIoEx(pipe, &io->readOv)) {
    const DWORD code = ::GetLastError();
    if (code != ERROR_NOT_FOUND) {
      // 取り消し対象が無い（ERROR_NOT_FOUND）以外は、そのまま回収待ちに進む。
    }
  }
  DWORD done = 0;
  if (!::GetOverlappedResult(pipe, &io->readOv, &done, TRUE)) {
    const DWORD code = ::GetLastError();
    if (IsDisconnect(code) || code == ERROR_OPERATION_ABORTED) {
      if (bytesOut != nullptr) {
        *bytesOut = 0;
      }
      return 0;
    }
    throw WinError("名前付きパイプの読み取り取り消しの回収");
  }
  if (bytesOut != nullptr) {
    *bytesOut = done;
  }
  return done;
}

}  // namespace

PipeClient::PipeClient(const std::string& pipeName) {
  // サーバ（main プロセス）の listen が spawn より遅れるため、最大 5 秒だけ待つ。
  // 5 秒は仮の上限であり、接続試行のTimeoutではない（返却許可とは無関係）。
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  HANDLE h = INVALID_HANDLE_VALUE;
  for (;;) {
    // overlapped I/O で使うため FILE_FLAG_OVERLAPPED を付ける。以降の読み書きは
    // すべて OVERLAPPED 経由で行い、同期ハンドルのつもりで待機・ポーリングしない。
    h = ::CreateFileA(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                      FILE_FLAG_OVERLAPPED, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      break;
    }
    const DWORD code = ::GetLastError();
    if (code != ERROR_PIPE_BUSY && code != ERROR_FILE_NOT_FOUND) {
      throw WinError("名前付きパイプへの接続");
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      throw std::runtime_error("名前付きパイプへの接続が 5 秒でタイムアウト: " + pipeName);
    }
    if (code == ERROR_PIPE_BUSY) {
      ::WaitNamedPipeA(pipeName.c_str(), 500);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  // Node の net.createServer が作るパイプはバイト型であり、ここでメッセージ型への
  // 切り替えはできない（ERROR_INVALID_PARAMETER になる）。全メッセージが 64 バイト
  // 固定のため、バイト型のまま枠合わせすれば境界は崩れない。
  // メッセージモードの強制はしない。
  IoState* io = new IoState();
  io->readEvent = ::CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (io->readEvent == nullptr) {
    delete io;
    ::CloseHandle(h);
    throw WinError("名前付きパイプの読み取りイベント生成");
  }
  io->writeEvent = ::CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (io->writeEvent == nullptr) {
    ::CloseHandle(io->readEvent);
    delete io;
    ::CloseHandle(h);
    throw WinError("名前付きパイプの書き込みイベント生成");
  }
  io->readOv.hEvent = io->readEvent;
  io->writeOv.hEvent = io->writeEvent;
  handle_ = h;
  io_ = io;
}

PipeClient::~PipeClient() {
  IoState* io = static_cast<IoState*>(io_);
  io_ = nullptr;
  HANDLE h = static_cast<HANDLE>(handle_);
  handle_ = nullptr;
  CloseIoState(h, io);
  if (h != nullptr && h != INVALID_HANDLE_VALUE) {
    ::CloseHandle(h);
  }
}

void PipeClient::Send(const TransferMessage& msg) {
  HANDLE h = static_cast<HANDLE>(handle_);
  IoState* io = static_cast<IoState*>(io_);
  const uint8_t* cursor = reinterpret_cast<const uint8_t*>(&msg);
  DWORD remaining = static_cast<DWORD>(sizeof(msg));
  while (remaining > 0) {
    io->writeOv.Offset = 0;
    io->writeOv.OffsetHigh = 0;
    DWORD done = 0;
    if (!::WriteFile(h, cursor, remaining, nullptr, &io->writeOv)) {
      const DWORD code = ::GetLastError();
      if (code != ERROR_IO_PENDING) {
        throw WinError("名前付きパイプの書き込み");
      }
      // 非同期の完了をイベントで待つ（ポーリングしない）。書き込みの相手は
      // 常に生存している前提であり、期限は付けない。
      const DWORD waited = ::WaitForSingleObject(io->writeEvent, INFINITE);
      if (waited != WAIT_OBJECT_0) {
        throw WinError("名前付きパイプの書き込み完了待ち");
      }
      if (!::GetOverlappedResult(h, &io->writeOv, &done, FALSE)) {
        throw WinError("名前付きパイプの書き込み");
      }
    } else {
      // 即時完了時も転送量は GetOverlappedResult で取る。
      if (!::GetOverlappedResult(h, &io->writeOv, &done, FALSE)) {
        throw WinError("名前付きパイプの書き込み");
      }
    }
    if (done == 0) {
      throw WinError("名前付きパイプの書き込み");
    }
    cursor += done;
    remaining -= done;
  }
}

bool PipeClient::Receive(TransferMessage* out) { return ReceiveWithDeadline(out, 0, nullptr); }

bool PipeClient::ReceiveWithDeadline(TransferMessage* out, uint64_t deadlineUs, bool* timedOut) {
  HANDLE h = static_cast<HANDLE>(handle_);
  IoState* io = static_cast<IoState*>(io_);
  if (timedOut != nullptr) {
    *timedOut = false;
  }
  uint8_t* dst = reinterpret_cast<uint8_t*>(out);
  // 前回の部分受信があれば先頭に復元する。
  size_t have = 0;
  if (io->stagedBytes > 0) {
    have = io->stagedBytes;
    if (have > sizeof(*out)) {
      have = sizeof(*out);
    }
    std::memcpy(dst, io->staged, have);
    io->stagedBytes = 0;
  }
  // 部分受信の退避。呼び出し側バッファの先頭 have バイトを溜めて false を返す。
  auto stash = [&]() {
    if (have > 0) {
      std::memcpy(io->staged, dst, have);
      io->stagedBytes = have;
    } else {
      io->stagedBytes = 0;
    }
  };
  while (have < sizeof(*out)) {
    if (deadlineUs != 0 && NowUs() >= deadlineUs) {
      stash();
      if (timedOut != nullptr) {
        *timedOut = true;
      }
      return false;
    }
    io->readOv.Offset = 0;
    io->readOv.OffsetHigh = 0;
    const DWORD want = static_cast<DWORD>(sizeof(*out) - have);
    DWORD done = 0;
    if (!::ReadFile(h, dst + have, want, nullptr, &io->readOv)) {
      const DWORD code = ::GetLastError();
      if (IsDisconnect(code)) {
        stash();
        return false;
      }
      if (code != ERROR_IO_PENDING) {
        stash();
        throw WinError("名前付きパイプの読み取り");
      }
      // 到着待ちはイベントの期限付き待機で行う。sleep ポーリングは使わない。
      // 期限は終了判定のみであり、返却許可にはならない。
      const DWORD wait_ms = deadlineUs == 0 ? INFINITE : RemainingMs(deadlineUs);
      DWORD waited = WAIT_TIMEOUT;
      if (wait_ms == 0) {
        // 期限到達。発行済みの読みは無い（ReadFile 自体が PENDING を返した直後のため、
        // 取り消し対象は存在する）。回収して部分受信として溜める。
        DWORD collected = 0;
        CollectPendingRead(h, io, &collected);
        have += collected;
        stash();
        if (timedOut != nullptr) {
          *timedOut = true;
        }
        return false;
      } else {
        waited = ::WaitForSingleObject(io->readEvent, wait_ms);
      }
      if (waited == WAIT_TIMEOUT) {
        // 期限切れ。到着直後の到着分も失わないよう回収して溜める。
        DWORD collected = 0;
        CollectPendingRead(h, io, &collected);
        have += collected;
        stash();
        if (timedOut != nullptr) {
          *timedOut = true;
        }
        return false;
      }
      if (waited != WAIT_OBJECT_0) {
        stash();
        throw WinError("名前付きパイプの到着待ち");
      }
      if (!::GetOverlappedResult(h, &io->readOv, &done, FALSE)) {
        const DWORD result_code = ::GetLastError();
        if (IsDisconnect(result_code)) {
          stash();
          return false;
        }
        stash();
        throw WinError("名前付きパイプの読み取り");
      }
    } else {
      // 即時完了時も転送量は GetOverlappedResult で取る。
      if (!::GetOverlappedResult(h, &io->readOv, &done, FALSE)) {
        const DWORD result_code = ::GetLastError();
        if (IsDisconnect(result_code)) {
          stash();
          return false;
        }
        stash();
        throw WinError("名前付きパイプの読み取り");
      }
    }
    if (done == 0) {
      stash();
      return false;
    }
    have += done;
  }
  io->stagedBytes = 0;
  return true;
}

}  // namespace xrift::transfer

#else

#include <stdexcept>

namespace xrift::transfer {

PipeClient::PipeClient(const std::string&) {
  throw std::runtime_error("名前付きパイプ転送は Windows のみ（Linux は将来対応）");
}
PipeClient::~PipeClient() = default;
void PipeClient::Send(const TransferMessage&) {
  throw std::runtime_error("名前付きパイプ転送は Windows のみ（Linux は将来対応）");
}
bool PipeClient::Receive(TransferMessage*) {
  throw std::runtime_error("名前付きパイプ転送は Windows のみ（Linux は将来対応）");
}
bool PipeClient::ReceiveWithDeadline(TransferMessage*, uint64_t, bool*) {
  throw std::runtime_error("名前付きパイプ転送は Windows のみ（Linux は将来対応）");
}

}  // namespace xrift::transfer

#endif
