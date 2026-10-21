#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include "app.h"
#include "common/log.h"
#include "transfer/m2_selftest.h"
#include "transfer/transfer_app.h"
#include "transfer/xr_transfer_app.h"

namespace {

void PrintHelp(const char* argv0) {
  std::printf(
      "使い方: %s [--probe] [--transfer --pipe=<名前>] [--xr-transfer --pipe=<名前>] [--help]\n"
      "  引数なし  HMD に両眼クリア色を表示する\n"
      "  --probe   インスタンスと Vulkan 初期化だけ行い環境情報を出す（セッションは作らない）\n"
      "  --transfer --pipe=\\\\.\\pipe\\<名前> [--verify|--no-verify] [--gpu-check|--no-gpu-check]\n"
      "            [--seconds=N] [--mode=<vulkan|d3d-open-only|no-gpu>] [--mem-log=<path>]\n"
      "            Electron の OSR 共有テクスチャを受けて GPU 内コピーする（セッションは作らない）\n"
      "            --verify は既定 on。--seconds が正ならその秒数で受信を打ち切る\n"
      "            --gpu-check は既定 on。全画面を GPU 上で検査し、画像は CPU に戻さない\n"
      "            --mode は診断用（既定 vulkan=Vulkan 経路。D3D11 デバイスを作らない）。\n"
      "            d3d-open-only は D3D11 Open 経路の診断専用（Vulkan は使わない。\n"
      "            長時間動かすとメモリが増えるため切り分け以外の用途では使わない）。\n"
      "            no-gpu は GPU API を使わず即 Done（借用の返却だけ。いずれも ACK 経路は同じ）\n"
      "            --mem-log=<path> は 1 秒周期のメモリ CSV（計測用。既定は出さない）\n"
      "            --fail-after=N は検証用の人為的非ゼロ終了フック（既定 0=無効）\n"
      "            --inject-unconfirmed=N は N 件目の Offer を完了未確認として扱う\n"
      "            検証フック（既定 0=無効。Done を送らず終了 2。import 資源は破棄しない）\n"
      "            --delay-ms=N は 1 フレームの処理前に N ミリ秒待つ検証フック（既定 0=無効）\n"
      "  --xr-transfer --pipe=\\\\.\\pipe\\<名前> [--render-scale=<0.2〜1.0>]\n"
      "            [--record-ttl-ms=<ms>] [--seconds=N] [--inject-stale-record=N] [--no-submit-verify] [--dump-submit=<dir>] [--dump-frame=N] [--no-resubmit] [--resubmit-max-age-ms=<ms>]\n"
      "            [--producer-idle-timeout-ms=<ms>] [--pace-records]\n"
      "            M2 の XR 提出モード。XR セッションを開始し、pose/FOV の描画レコードを\n"
      "            発行して RenderRequest を送り、受理画像だけを左右 swapchain へ blit する。\n"
      "            --render-scale は既定 0.5。--record-ttl-ms は描画レコードの失効上限\n"
      "            （既定 500。実測: 本番サイトで 350〜500ms の遅延のため app 経由は暫定 2000）。\n"
      "            --inject-stale-record=N は N フレーム目の\n"
      "            RenderRequest に旧 pose 混入の合図を載せる（harness 側フック用）。\n"
      "            --no-submit-verify で blit 後の左右分離 readback を止める\n"
      "            --dump-submit=<dir> は N 回目の提出時に両眼 swapchain を PPM 保存する\n"
      "            （検証用。既定は保存しない。--dump-frame=N は既定 60）\n"
      "            [--space=<auto|local|stage|local-floor>] [--inject-session-restart=Ms]\n"
      "            [--inject-space-change=Ms]\n"
      "            --space は参照空間の要求（既定 auto=local-floor→stage→local の後退。\n"
      "            不可なら落とさず後退し、理由をログに出す）\n"
      "            --inject-session-restart=Ms は起動から Ms 後に意図的にセッションを\n"
      "            破棄・再生成する（復帰経路の検証用。既定 0=無効）\n"
      "            --inject-space-change=Ms は起動から Ms 後に模擬の space 変更を 1 回\n"
      "            適用する（spaceEpoch 更新・旧レコード失効の経路検証用。\n"
      "            実イベントではないことをログに明記し、統計の space変更に計上する。\n"
      "            既定 0=無効）\n"
      "            --producer-idle-timeout-ms=<ms> は Offer 途絶で producer 終了とみなす\n"
      "            無受信の上限（既定 5000。0 は無効でパイプ切断でのみ終了。\n"
      "            app 経由の通常運用は 0 を渡し、静止ページで paint が止まっても\n"
      "            終了しない。ハーネス向けの既定は保つ）\n"
      "            --pace-records は実パイプラインに合わせた発行のフロー制御\n"
      "            （既定 off。on のとき未処理の画像がある間は RenderRequest を送らず、\n"
      "            Offer 処理後の次のフレームで発行する。500ms 停滞したら新しい\n"
      "            レコードで詰まりを解く。app 経由の通常運用は on を渡す）\n"
  "            --dump-exit-stage は終了時の破棄段階ログ（既定 off。\n"
  "            環境変数 XRIFT_EXIT_STAGE があればそちらでも有効になる）\n"
  "            --no-quick-exit は終了時の即時終了を止め、旧来の RAII 破棄を行う\n"
  "            （既定は統計出力と明示 teardown の後に残りの破棄を省略して終了する。\n"
  "            終了時クラッシュの回避用。切り分け以外では付けない）\n"
      "            入力は毎秒サマリ（[M4入力]。その 1 秒間に押されたラッチ付き）に加え、\n"
      "            状態変化のフレームで [M4エッジ] を出す。haptics 受信は XrResult を\n"
      "            ログに残す\n"
      "            --resubmit-max-age-ms=<ms> は再提出を許す画像年齢の上限\n"
      "            （既定 0=無制限。正なら年齢超過の再提出は行わず空フレームにする。診断用）\n"
      "  --m2-selftest  M2 の値層（配置・pose タグ・受理/棄却）を HMD 無しに検証する\n"
      "  --help    この表示\n",
      argv0);
}

bool StartsWith(const char* s, const char* prefix) {
  return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

int RunTransferArgs(int argc, char** argv) {
  std::string pipe;
  bool verify = true;
  bool gpu_check = true;
  int seconds = 0;
  int fail_after = 0;
  int inject_unconfirmed = 0;
  int delay_ms = 0;
  std::string mode = "vulkan";
  std::string mem_log;
  for (int i = 2; i < argc; ++i) {
    if (StartsWith(argv[i], "--pipe=")) {
      pipe = argv[i] + std::strlen("--pipe=");
    } else if (std::strcmp(argv[i], "--verify") == 0) {
      verify = true;
    } else if (std::strcmp(argv[i], "--no-verify") == 0) {
      verify = false;
    } else if (std::strcmp(argv[i], "--gpu-check") == 0) {
      gpu_check = true;
    } else if (std::strcmp(argv[i], "--no-gpu-check") == 0) {
      gpu_check = false;
    } else if (StartsWith(argv[i], "--seconds=")) {
      seconds = std::atoi(argv[i] + std::strlen("--seconds="));
      if (seconds < 0) {
        seconds = 0;
      }
    } else if (StartsWith(argv[i], "--fail-after=")) {
      // 検証用の人為的非ゼロ終了フック。既定 0=無効。通常経路では使わない。
      fail_after = std::atoi(argv[i] + std::strlen("--fail-after="));
      if (fail_after < 0) {
        fail_after = 0;
      }
    } else if (StartsWith(argv[i], "--inject-unconfirmed=")) {
      // 検証用の完了未確認フック。既定 0=無効。通常経路では使わない。
      inject_unconfirmed = std::atoi(argv[i] + std::strlen("--inject-unconfirmed="));
      if (inject_unconfirmed < 0) {
        inject_unconfirmed = 0;
      }
    } else if (StartsWith(argv[i], "--delay-ms=")) {
      // 検証用の consumer 遅延フック。既定 0=無効。通常経路では使わない。
      delay_ms = std::atoi(argv[i] + std::strlen("--delay-ms="));
      if (delay_ms < 0) {
        delay_ms = 0;
      }
    } else if (StartsWith(argv[i], "--mode=")) {
      // 診断モード（既定 vulkan=Vulkan 経路）。不正値は受付時に落とす。
      mode = argv[i] + std::strlen("--mode=");
      if (mode != "vulkan" && mode != "d3d-open-only" && mode != "no-gpu") {
        xrift::Error("不明な --mode=%s（vulkan / d3d-open-only / no-gpu のいずれか）",
                     mode.c_str());
        PrintHelp(argv[0]);
        return 1;
      }
    } else if (StartsWith(argv[i], "--mem-log=")) {
      // 1 秒周期のメモリ CSV（計測用。既定は出さない）。
      mem_log = argv[i] + std::strlen("--mem-log=");
    } else {
      xrift::Error("不明な引数: %s", argv[i]);
      PrintHelp(argv[0]);
      return 1;
    }
  }
  if (pipe.empty()) {
    xrift::Error("--transfer には --pipe=<名前> が必要");
    PrintHelp(argv[0]);
    return 1;
  }
  return xrift::transfer::RunTransfer(pipe, verify, seconds, fail_after, gpu_check,
                                        inject_unconfirmed, delay_ms, mode, mem_log);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
    PrintHelp(argv[0]);
    return 0;
  }
  if (argc >= 2 && std::strcmp(argv[1], "--probe") == 0) {
    return xrift::Probe();
  }
  if (argc >= 2 && std::strcmp(argv[1], "--m2-selftest") == 0) {
    return xrift::transfer::RunM2SelfTest();
  }
  if (argc >= 2 && std::strcmp(argv[1], "--transfer") == 0) {
    return RunTransferArgs(argc, argv);
  }
  if (argc >= 2 && std::strcmp(argv[1], "--xr-transfer") == 0) {
    xrift::transfer::XrTransferOptions opts{};
    for (int i = 2; i < argc; ++i) {
      if (StartsWith(argv[i], "--pipe=")) {
        opts.pipeName = argv[i] + std::strlen("--pipe=");
      } else if (StartsWith(argv[i], "--render-scale=")) {
        opts.renderScale = std::atof(argv[i] + std::strlen("--render-scale="));
      } else if (StartsWith(argv[i], "--record-ttl-ms=")) {
        // 描画レコードの失効上限（ms）。既定 500。正でなければ受け付けない。
        const int ttl = std::atoi(argv[i] + std::strlen("--record-ttl-ms="));
        if (ttl <= 0) {
          xrift::Error("--record-ttl-ms は正の整数であること（指定=%s）",
                       argv[i] + std::strlen("--record-ttl-ms="));
          PrintHelp(argv[0]);
          return 1;
        }
        opts.recordTtlMs = static_cast<uint64_t>(ttl);
      } else if (StartsWith(argv[i], "--seconds=")) {
        opts.seconds = std::atoi(argv[i] + std::strlen("--seconds="));
        if (opts.seconds < 0) {
          opts.seconds = 0;
        }
      } else if (StartsWith(argv[i], "--inject-stale-record=")) {
        opts.injectStaleRecord = std::atoi(argv[i] + std::strlen("--inject-stale-record="));
        if (opts.injectStaleRecord < 0) {
          opts.injectStaleRecord = 0;
        }
      } else if (std::strcmp(argv[i], "--no-submit-verify") == 0) {
        opts.submitVerify = false;
      } else if (StartsWith(argv[i], "--dump-submit=")) {
        // 検証用の提出画像ダンプ。既定は空（無効）。通常経路では使わない。
        opts.dumpSubmitDir = argv[i] + std::strlen("--dump-submit=");
      } else if (StartsWith(argv[i], "--dump-frame=")) {
        // 保存する提出回（1 起点。既定 60）。dump-submit が空なら無視する。
        opts.dumpFrame = std::atoi(argv[i] + std::strlen("--dump-frame="));
        if (opts.dumpFrame < 1) {
          opts.dumpFrame = 1;
        }
      } else if (StartsWith(argv[i], "--space=")) {
        opts.space = argv[i] + std::strlen("--space=");
        if (opts.space != "auto" && opts.space != "local" && opts.space != "stage" &&
            opts.space != "local-floor") {
          xrift::Error("--space は auto/local/stage/local-floor のいずれかであること（指定=%s）",
                       opts.space.c_str());
          PrintHelp(argv[0]);
          return 1;
        }
      } else if (StartsWith(argv[i], "--inject-session-restart=")) {
        // 検証用の意図的セッション再生成フック。既定 0=無効。通常経路では使わない。
        opts.injectSessionRestartMs =
            std::atoi(argv[i] + std::strlen("--inject-session-restart="));
        if (opts.injectSessionRestartMs < 0) {
          opts.injectSessionRestartMs = 0;
        }
      } else if (StartsWith(argv[i], "--inject-space-change=")) {
        // 検証用の模擬 space 変更フック。既定 0=無効。通常経路では使わない。
        opts.injectSpaceChangeMs =
            std::atoi(argv[i] + std::strlen("--inject-space-change="));
        if (opts.injectSpaceChangeMs < 0) {
          opts.injectSpaceChangeMs = 0;
        }
      } else if (StartsWith(argv[i], "--producer-idle-timeout-ms=")) {
        // Offer 途絶の終了判定の上限。0 は無効（パイプ切断でのみ終了）。
        opts.producerIdleTimeoutMs =
            std::atoi(argv[i] + std::strlen("--producer-idle-timeout-ms="));
        if (opts.producerIdleTimeoutMs < 0) {
          opts.producerIdleTimeoutMs = 0;
        }
      } else if (std::strcmp(argv[i], "--pace-records") == 0) {
        opts.paceRecords = true;
      } else if (std::strcmp(argv[i], "--dump-exit-stage") == 0) {
        opts.dumpExitStage = true;
      } else if (std::strcmp(argv[i], "--no-quick-exit") == 0) {
        // 診断用。既定の即時終了を止め、旧来の RAII 破棄を行う。
        opts.quickExit = false;
      } else if (std::strcmp(argv[i], "--no-resubmit") == 0) {
        opts.resubmit = false;
      } else if (StartsWith(argv[i], "--resubmit-max-age-ms=")) {
        // 再提出の年齢上限（ms）。0 は無制限（既定）。負は受け付けない。
        const int resubmit_max_age =
            std::atoi(argv[i] + std::strlen("--resubmit-max-age-ms="));
        if (resubmit_max_age < 0) {
          xrift::Error("--resubmit-max-age-ms は 0 以上の整数であること（指定=%s）",
                       argv[i] + std::strlen("--resubmit-max-age-ms="));
          PrintHelp(argv[0]);
          return 1;
        }
        opts.resubmitMaxAgeMs = static_cast<uint64_t>(resubmit_max_age);
      } else {
        xrift::Error("不明な引数: %s", argv[i]);
        PrintHelp(argv[0]);
        return 1;
      }
    }
    if (opts.pipeName.empty()) {
      xrift::Error("--xr-transfer には --pipe=<名前> が必要");
      PrintHelp(argv[0]);
      return 1;
    }
    if (opts.renderScale < 0.2 || opts.renderScale > 1.0) {
      xrift::Error("--render-scale は 0.2〜1.0 の範囲であること（指定=%f）", opts.renderScale);
      PrintHelp(argv[0]);
      return 1;
    }
    // 生成・接続の失敗（パイプ不在等）は例外で返る。捕捉せずに抜けると
    // 未捕捉例外の abort になるため、ここで 1 に丸めて通常終了させる。
    // ADR: 終了時クラッシュの回避は RunXrTransfer 内の通常出口で行う
    // （統計出力と明示 teardown の後に残りの RAII 破棄を飛ばす）。
    // ここに届くのは旧経路（--no-quick-exit）か開始前の早期 return 経路だけであり、
    // 既定では stdout/stderr を flush してから残りの破棄（静的デストラクタ等）を
    // 飛ばして終了する。エラー経路の統計とメッセージは RunXrTransfer 側で出す。
    int code = 0;
    try {
      code = xrift::transfer::RunXrTransfer(opts);
    } catch (const std::exception& e) {
      xrift::Error("XR転送の異常終了: %s", e.what());
      code = 1;
    } catch (...) {
      xrift::Error("XR転送の異常終了（不明な例外）");
      code = 1;
    }
    if (opts.quickExit) {
      xrift::Info("終了: 残りの破棄を省略して終了する（--no-quick-exit で旧経路）");
      std::fflush(stdout);
      std::fflush(stderr);
      std::_Exit(code);
    }
    return code;
  }
  if (argc >= 2) {
    xrift::Error("不明な引数: %s", argv[1]);
    PrintHelp(argv[0]);
    return 1;
  }
  try {
    xrift::XrApp app;
    return app.Run();
  } catch (const std::exception& e) {
    xrift::Error("%s", e.what());
    return 1;
  }
}
