#include "transfer/transfer_app.h"

#ifdef _WIN32

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "common/log.h"
#include "transfer/pipe_client.h"
#include "transfer/protocol.h"
#include "transfer/shared_texture.h"

namespace xrift::transfer {
namespace {

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
  uint64_t firstUs = 0;
  uint64_t lastUs = 0;

  void Add(const TransferSession::Timings& t) {
    importUsTotal += t.importUs;
    copyUsTotal += t.copyUs;
    verifyUsTotal += t.verifyUs;
    if (t.importUs > importUsMax) {
      importUsMax = t.importUs;
    }
    if (t.copyUs > copyUsMax) {
      copyUsMax = t.copyUs;
    }
    if (t.verifyUs > verifyUsMax) {
      verifyUsMax = t.verifyUs;
    }
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
  }
};

uint64_t NowUs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

int RunTransferLoop(const std::string& pipeName, bool verify, int seconds, int failAfter,
                    TransferSession& session) {
  Info("転送待機: pipe=%s verify=%s 制限=%s", pipeName.c_str(), verify ? "on" : "off",
       seconds > 0 ? (std::to_string(seconds) + "秒").c_str() : "無し");

  PipeClient pipe(pipeName);
  Info("パイプ接続: main プロセスと接続した");

  const uint64_t deadlineUs =
      seconds > 0 ? NowUs() + static_cast<uint64_t>(seconds) * 1000000u : 0;
  Stats stats{};
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

    TransferSession::Timings timings{};
    const uint32_t status = session.ProcessOffer(msg.handle, msg.width, msg.height, &timings);
    stats.Add(timings);
    if (status == kStatusNoAck) {
      // GPU 完了を確認できない。Done を送ると main が release してしまうため送らない。
      // 以後の新規受付を停止し、接続を閉じて非ゼロ終了する（DESIGN §6②）。
      // timeout は返却許可ではない。
      Error("GPU 完了未確認のため受付を停止して終了する（borrow への ACK なし）");
      stats.Print();
      return 2;
    }
    switch (status) {
      case kStatusOk:
        ++stats.ackOk;
        break;
      case kStatusVerifyFailed:
        ++stats.verifyFailed;
        break;
      default:
        ++stats.copyFailed;
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
      Info("進行: 受信=%llu ok=%llu copy失敗=%llu verify失敗=%llu",
           static_cast<unsigned long long>(stats.offers), static_cast<unsigned long long>(stats.ackOk),
           static_cast<unsigned long long>(stats.copyFailed),
           static_cast<unsigned long long>(stats.verifyFailed));
    }
  }

  stats.Print();
  // 失敗なしのときだけ 0 を返す。copy/verify 失敗・不正 Offer が 1 件でもあれば非ゼロ。
  if (stats.copyFailed > 0 || stats.verifyFailed > 0 || stats.invalidOffers > 0) {
    return 1;
  }
  return 0;
}

}  // namespace

int RunTransfer(const std::string& pipeName, bool verify, int seconds, int failAfter) {
  TransferSession::Options options{};
  options.verify = verify;
  // 初期化失敗はクラッシュではなく文面付きの終了にする（未処理例外は abort になる）。
  try {
    TransferSession session(options);
    return RunTransferLoop(pipeName, verify, seconds, failAfter, session);
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

int RunTransfer(const std::string&, bool, int) {
  Error("--transfer は Windows のみ（Linux は将来対応）");
  return 1;
}

}  // namespace xrift::transfer

#endif
