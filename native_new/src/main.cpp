#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

#include "app.h"
#include "common/log.h"
#include "transfer/transfer_app.h"

namespace {

void PrintHelp(const char* argv0) {
  std::printf(
      "使い方: %s [--probe] [--transfer --pipe=<名前>] [--help]\n"
      "  引数なし  HMD に両眼クリア色を表示する\n"
      "  --probe   インスタンスと Vulkan 初期化だけ行い環境情報を出す（セッションは作らない）\n"
      "  --transfer --pipe=\\\\.\\pipe\\<名前> [--verify|--no-verify] [--seconds=N]\n"
      "            Electron の OSR 共有テクスチャを受けて GPU 内コピーする（セッションは作らない）\n"
      "            --verify は既定 on。--seconds が正ならその秒数で受信を打ち切る\n"
      "            --fail-after=N は検証用の人為的非ゼロ終了フック（既定 0=無効）\n"
      "  --help    この表示\n",
      argv0);
}

bool StartsWith(const char* s, const char* prefix) {
  return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

int RunTransferArgs(int argc, char** argv) {
  std::string pipe;
  bool verify = true;
  int seconds = 0;
  int fail_after = 0;
  for (int i = 2; i < argc; ++i) {
    if (StartsWith(argv[i], "--pipe=")) {
      pipe = argv[i] + std::strlen("--pipe=");
    } else if (std::strcmp(argv[i], "--verify") == 0) {
      verify = true;
    } else if (std::strcmp(argv[i], "--no-verify") == 0) {
      verify = false;
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
  return xrift::transfer::RunTransfer(pipe, verify, seconds, fail_after);
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
  if (argc >= 2 && std::strcmp(argv[1], "--transfer") == 0) {
    return RunTransferArgs(argc, argv);
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
