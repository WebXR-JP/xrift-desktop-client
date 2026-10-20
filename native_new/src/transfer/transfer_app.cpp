#include "transfer/transfer_app.h"

#ifdef _WIN32

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>

#include "common/log.h"
#include "transfer/mem_log.h"
#include "transfer/pipe_client.h"
#include "transfer/protocol.h"
#include "transfer/shared_texture.h"

namespace xrift::transfer {
namespace {

// プロセスのハンドル数を返す。取得失敗時は 0 を返す（統計用の観測点であり、
// 失敗自体は転送の成否に影響させない）。
DWORD CurrentHandleCount() {
  DWORD count = 0;
  if (::GetProcessHandleCount(::GetCurrentProcess(), &count) == 0) {
    return 0;
  }
  return count;
}

// 受信統計。終了時に必ず出す（段階 4 の根拠にする）。
struct Stats {
  uint64_t offers = 0;
  uint64_t ackOk = 0;
  uint64_t copyFailed = 0;
  uint64_t verifyFailed = 0;
  uint64_t invalidOffers = 0;
  uint64_t importUsTotal = 0;
  uint64_t copyUsTotal = 0;
  uint64_t verifyUsTotal = 0;
  uint64_t importUsMax = 0;
  uint64_t copyUsMax = 0;
  uint64_t verifyUsMax = 0;
  // M1b 全画面 GPU 検査の統計。画像全体は読まず、結果バッファだけを集計する。
  uint64_t gpuChecked = 0;        // GPU 検査を実際に完了したフレーム数。
  uint64_t gpuSkipped = 0;        // GPU 検査が有効でも完了しなかったフレーム数
                                  // （import/copy 失敗等。検査済みに数えない）。
  uint64_t gpuMismatchFrames = 0;  // 不一致が 1 件以上あったフレーム数。
  uint64_t gpuMismatchTotal = 0;   // 不一致タイル数の累計。
  uint64_t checkUsTotal = 0;
  uint64_t checkUsMax = 0;
  int64_t lastDecoded = -1;  // 復号した前回のフレーム番号。未受信時は -1。
  uint64_t decodedForward = 0;   // 前回より進んだ（16bit 周回を許容）。
  uint64_t decodedStale = 0;     // 前回と同じ（繰り返し paint の再利用）。
  uint64_t decodedBackward = 0;  // 前回より戻った。
  uint64_t firstUs = 0;
  uint64_t lastUs = 0;
  // ハンドル数の観測点（--delay-ms 検証用）。増加し続けないことの証拠にする。
  // 取得は 60 件ごとの進捗と終了時だけであり、通常経路の負荷にはならない。
  DWORD handleFirst = 0;
  DWORD handleLast = 0;
  DWORD handleMax = 0;

  void Add(const TransferSession::Timings& t) {
    importUsTotal += t.importUs;
    copyUsTotal += t.copyUs;
    verifyUsTotal += t.verifyUs;
    checkUsTotal += t.checkUs;
    if (t.importUs > importUsMax) {
      importUsMax = t.importUs;
    }
    if (t.copyUs > copyUsMax) {
      copyUsMax = t.copyUs;
    }
    if (t.verifyUs > verifyUsMax) {
      verifyUsMax = t.verifyUs;
    }
    if (t.checkUs > checkUsMax) {
      checkUsMax = t.checkUs;
    }
  }

  // GPU 検査の 1 件を集計する。復号番号の推移（前回との差）と停滞を数える。
  // 繰り返し paint は同じ番号になりうるため停滞は失敗にしない（DESIGN §7）。
  void AddGpuCheck(const GpuChecker::Report& report) {
    ++gpuChecked;
    if (report.mismatchCount > 0) {
      ++gpuMismatchFrames;
      gpuMismatchTotal += report.mismatchCount;
    }
    if (lastDecoded >= 0) {
      const uint32_t diff =
          (report.decodedFrame - static_cast<uint32_t>(lastDecoded)) & 0xFFFFu;
      if (diff == 0) {
        ++decodedStale;
      } else if (diff > 0x7FFFu) {
        ++decodedBackward;
      } else {
        ++decodedForward;
      }
    }
    lastDecoded = static_cast<int64_t>(report.decodedFrame);
  }

  void Print() const {
    const double spanS =
        firstUs != 0 && lastUs > firstUs ? static_cast<double>(lastUs - firstUs) / 1e6 : 0.0;
    const double fps = spanS > 0.0 ? static_cast<double>(offers) / spanS : 0.0;
    const double denom = offers > 0 ? static_cast<double>(offers) : 1.0;
    Info("転送統計: 受信=%llu ok=%llu copy失敗=%llu verify失敗=%llu 不正=%llu",
         static_cast<unsigned long long>(offers), static_cast<unsigned long long>(ackOk),
         static_cast<unsigned long long>(copyFailed), static_cast<unsigned long long>(verifyFailed),
         static_cast<unsigned long long>(invalidOffers));
    Info("転送統計: 受信fps=%.2f（受信間隔からの算出。描画fpsではない）", fps);
    Info("転送統計: import 平均=%lluus 最大=%lluus", static_cast<unsigned long long>(importUsTotal / denom),
         static_cast<unsigned long long>(importUsMax));
    Info("転送統計: copy 平均=%lluus 最大=%lluus", static_cast<unsigned long long>(copyUsTotal / denom),
         static_cast<unsigned long long>(copyUsMax));
    Info("転送統計: verify 平均=%lluus 最大=%lluus", static_cast<unsigned long long>(verifyUsTotal / denom),
         static_cast<unsigned long long>(verifyUsMax));
    Info("GPU検査: 検査=%llu スキップ=%llu 不一致フレーム=%llu 不一致合計=%llu",
         static_cast<unsigned long long>(gpuChecked),
         static_cast<unsigned long long>(gpuSkipped),
         static_cast<unsigned long long>(gpuMismatchFrames),
         static_cast<unsigned long long>(gpuMismatchTotal));
    Info("GPU検査: check 平均=%lluus 最大=%lluus", static_cast<unsigned long long>(checkUsTotal / denom),
         static_cast<unsigned long long>(checkUsMax));
    Info("GPU検査: 最終復号=%lld 前進=%llu 停滞=%llu 後退=%llu",
         static_cast<long long>(lastDecoded), static_cast<unsigned long long>(decodedForward),
         static_cast<unsigned long long>(decodedStale),
         static_cast<unsigned long long>(decodedBackward));
    Info("転送統計: ハンドル数 初回=%lu 最終=%lu 最大=%lu（増加し続けないこと）",
         static_cast<unsigned long>(handleFirst), static_cast<unsigned long>(handleLast),
         static_cast<unsigned long>(handleMax));
  }

  void NoteHandles(DWORD count) {
    if (handleFirst == 0) {
      handleFirst = count;
    }
    handleLast = count;
    if (count > handleMax) {
      handleMax = count;
    }
  }
};

uint64_t NowUs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

int RunTransferLoop(const std::string& pipeName, bool verify, int seconds, int failAfter,
                    bool gpuCheck, int injectUnconfirmed, int delayMs,
                    const std::string& modeName, const std::string& memLogPath,
                    TransferSession& session) {
  Info("転送待機: pipe=%s verify=%s gpu-check=%s mode=%s 制限=%s", pipeName.c_str(),
       verify ? "on" : "off", gpuCheck ? "on" : "off", modeName.c_str(),
       seconds > 0 ? (std::to_string(seconds) + "秒").c_str() : "無し");
  if (injectUnconfirmed > 0) {
    Info("検証フック有効: --inject-unconfirmed=%d（その件目を完了未確認として扱う）",
         injectUnconfirmed);
  }
  if (delayMs > 0) {
    Info("検証フック有効: --delay-ms=%d（1 フレームの処理前に待つ）", delayMs);
  }

  PipeClient pipe(pipeName);
  Info("パイプ接続: main プロセスと接続した");
  Stats stats{};
  // メモリログ用の完了・失敗計数。ログスレッドが読むため atomic にする
  // （stats 自体はループスレッド専用のため参照しない）。
  std::atomic<uint64_t> memOk{0};
  std::atomic<uint64_t> memFailed{0};
  MemLogger memLogger(memLogPath, modeName, &memOk, &memFailed);
  memLogger.Start();
  // 接続直後のハンドル数を基準にする（増加検知の起点）。
  stats.NoteHandles(CurrentHandleCount());

  const uint64_t deadlineUs =
      seconds > 0 ? NowUs() + static_cast<uint64_t>(seconds) * 1000000u : 0;
  TransferMessage msg{};

  for (;;) {
    bool timedOut = false;
    bool received = false;
    try {
      received = pipe.ReceiveWithDeadline(&msg, deadlineUs, &timedOut);
    } catch (const std::exception& e) {
      Error("パイプ受信に失敗: %s", e.what());
      stats.Print();
      return 1;
    }
    if (timedOut) {
      Info("指定秒数を経過したため受信を打ち切る");
      break;
    }
    if (!received) {
      Info("main プロセスが切断したため終了する");
      break;
    }
    if (!IsValidOffer(msg)) {
      ++stats.invalidOffers;
      Warn("不正な Offer を破棄（magic/version/type/format の不一致）");
      continue;
    }
    if (stats.offers == 0) {
      stats.firstUs = NowUs();
    }
    ++stats.offers;
    stats.lastUs = NowUs();

    // 検証用の consumer 遅延フック（--delay-ms=N）。1 フレームの処理前に待つ。
    // 既定 0 は待たない。通常経路の挙動は変えない。
    if (delayMs > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    }

    TransferSession::Timings timings{};
    GpuChecker::Report gpu_report{};
    const uint32_t status =
        session.ProcessOffer(msg.handle, msg.width, msg.height, &timings, &gpu_report);
    stats.Add(timings);
    if (gpuCheck) {
      if (timings.checkRan) {
        stats.AddGpuCheck(gpu_report);
      } else {
        // import/copy に失敗したフレームは検査を完了していない。
        // 以前は既定値の報告を検査済みに数えていたが、正直化のため別計上する。
        ++stats.gpuSkipped;
      }
    }
    if (status == kStatusNoAck) {
      // GPU 完了を確認できない。Done を送ると main が release してしまうため送らない。
      // 以後の新規受付を停止し、接続を閉じて非ゼロ終了する（DESIGN §6②）。
      // timeout は返却許可ではない。
      Error("GPU 完了未確認のため受付を停止して終了する（borrow への ACK なし）");
      stats.NoteHandles(CurrentHandleCount());
      stats.Print();
      return 2;
    }
    switch (status) {
      case kStatusOk:
        ++stats.ackOk;
        memOk.fetch_add(1, std::memory_order_relaxed);
        break;
      case kStatusVerifyFailed:
        ++stats.verifyFailed;
        memFailed.fetch_add(1, std::memory_order_relaxed);
        break;
      default:
        ++stats.copyFailed;
        memFailed.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    const TransferMessage done = MakeDone(msg.borrowId, status);
    try {
      pipe.Send(done);
    } catch (const std::exception& e) {
      Error("ACK 送信に失敗: %s", e.what());
      stats.Print();
      return 1;
    }
    // 検証 5 用の人為的非ゼロ終了フック。既定 off（failAfter <= 0）であり通常経路では
    // 到達しない。指定件数の ACK 後に Done 済みの状態で異常終了し、harness 側の
    // 終了コード反映を確かめる。
    if (failAfter > 0 && static_cast<int64_t>(stats.offers) >= failAfter) {
      Error("検証フック --fail-after=%d に到達したため異常終了する", failAfter);
      stats.Print();
      return 3;
    }
    // 進捗の観測点。60 件ごとに中間統計を出す。
    if (stats.offers % 60 == 0) {
      const DWORD handles = CurrentHandleCount();
      stats.NoteHandles(handles);
      Info("進行: 受信=%llu ok=%llu copy失敗=%llu verify失敗=%llu ハンドル数=%lu",
           static_cast<unsigned long long>(stats.offers), static_cast<unsigned long long>(stats.ackOk),
           static_cast<unsigned long long>(stats.copyFailed),
           static_cast<unsigned long long>(stats.verifyFailed),
           static_cast<unsigned long>(handles));
      if (gpuCheck) {
        Info("進行: gpu検査=%llu スキップ=%llu 不一致フレーム=%llu 復号=%lld 停滞=%llu 後退=%llu",
             static_cast<unsigned long long>(stats.gpuChecked),
             static_cast<unsigned long long>(stats.gpuSkipped),
             static_cast<unsigned long long>(stats.gpuMismatchFrames),
             static_cast<long long>(stats.lastDecoded),
             static_cast<unsigned long long>(stats.decodedStale),
             static_cast<unsigned long long>(stats.decodedBackward));
      }
    }
  }

  stats.NoteHandles(CurrentHandleCount());
  stats.Print();
  // 終了時の Vulkan 生存資源の証拠（imports=frees か、残留があるか）。
  // mem-log CSV の最終行と同じ値である。
  const VkLive live = GetVkLive();
  Info("Vk生存: images=%lld(%lldB) memories=%lld(%lldB) imports=%lld frees=%lld",
       static_cast<long long>(live.images), static_cast<long long>(live.imageBytes),
       static_cast<long long>(live.memories), static_cast<long long>(live.memoryBytes),
       static_cast<long long>(live.imports), static_cast<long long>(live.frees));
  // 失敗なしのときだけ 0 を返す。copy/verify 失敗・GPU検査の不一致・不正 Offer が
  // 1 件でもあれば非ゼロ。
  if (stats.copyFailed > 0 || stats.verifyFailed > 0 || stats.invalidOffers > 0 ||
      stats.gpuMismatchFrames > 0) {
    return 1;
  }
  return 0;
}

}  // namespace

int RunTransfer(const std::string& pipeName, bool verify, int seconds, int failAfter,
                bool gpuCheck, int injectUnconfirmed, int delayMs, const std::string& mode,
                const std::string& memLogPath) {
  TransferMode transferMode = TransferMode::Vulkan;
  try {
    transferMode = ParseTransferMode(mode);
  } catch (const std::exception& e) {
    Error("%s", e.what());
    return 1;
  }
  TransferSession::Options options{};
  options.verify = verify;
  options.gpuCheck = gpuCheck;
  options.injectUnconfirmed = injectUnconfirmed;
  options.mode = transferMode;
  const std::string modeName = TransferModeName(transferMode);
  // 初期化失敗はクラッシュではなく文面付きの終了にする（未処理例外は abort になる）。
  try {
    TransferSession session(options);
    return RunTransferLoop(pipeName, verify, seconds, failAfter, gpuCheck, injectUnconfirmed,
                           delayMs, modeName, memLogPath, session);
  } catch (const std::exception& e) {
    Error("転送の初期化に失敗: %s", e.what());
    return 1;
  }
}

}  // namespace xrift::transfer

#else

#include <string>

#include "common/log.h"
#include "transfer/transfer_app.h"

namespace xrift::transfer {

int RunTransfer(const std::string&, bool, int, int, bool, int, int, const std::string&,
                const std::string&) {
  Error("--transfer は Windows のみ（Linux は将来対応）");
  return 1;
}

}  // namespace xrift::transfer

#endif
