#include <cstdio>
#include <cstring>
#include <exception>

#include "app.h"
#include "common/log.h"

namespace {

void PrintHelp(const char* argv0) {
  std::printf(
      "使い方: %s [--probe] [--help]\n"
      "  引数なし  HMD に両眼クリア色を表示する\n"
      "  --probe   インスタンスと Vulkan 初期化だけ行い環境情報を出す（セッションは作らない）\n"
      "  --help    この表示\n",
      argv0);
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
