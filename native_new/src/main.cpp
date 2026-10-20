#include <cstdio>
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
      "  --xr-transfer --pipe=\\\\.\\pipe\\<名前> [--render-scale=<0.25〜1.0>]\n"
      "            [--seconds=N] [--inject-stale-record=N] [--no-submit-verify]\n"
      "            M2 の XR 提出モード。XR セッションを開始し、pose/FOV の描画レコードを\n"
      "            発行して RenderRequest を送り、受理画像だけを左右 swapchain へ blit する。\n"
      "            --render-scale は既定 0.5。--inject-stale-record=N は N フレーム目の\n"
      "            RenderRequest に旧 pose 混入の合図を載せる（harness 側フック用）。\n"
      "            --no-submit-verify で blit 後の左右分離 readback を止める\n"
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
    if (opts.renderScale < 0.25 || opts.renderScale > 1.0) {
      xrift::Error("--render-scale は 0.25〜1.0 の範囲であること（指定=%f）", opts.renderScale);
      PrintHelp(argv[0]);
      return 1;
    }
    return xrift::transfer::RunXrTransfer(opts);
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
