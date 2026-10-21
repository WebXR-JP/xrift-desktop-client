#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include "app.h"
#include "common/log.h"
#include "transfer/xr_transfer_app.h"
#include "transfer_selftest.h"

namespace {

void PrintHelp(const char* argv0) {
  std::printf(
      "使い方: %s [--probe] [--xr-transfer --pipe=<名前>] [--m2-selftest] [--help]\n"
      "  引数なし  HMD に両眼クリア色を表示する\n"
      "  --probe   インスタンスと Vulkan 初期化だけ行い環境情報を出す（セッションは作らない）\n"
      "  --xr-transfer --pipe=\\\\.\\pipe\\<名前> [--render-scale=<0.2〜1.0>]\n"
      "            [--record-ttl-ms=<ms>] [--seconds=N] [--space=<auto|local|stage|local-floor>]\n"
      "            M2 の XR 提出モード。XR セッションを開始し、pose/FOV の描画レコードを\n"
      "            発行して RenderRequest を送り、受理画像だけを左右 swapchain へ blit する。\n"
      "            --render-scale は既定 0.5。--record-ttl-ms は描画レコードの失効上限\n"
      "            （既定 500。実測: 本番サイトで 350〜500ms の遅延のため app 経由は暫定 2000）。\n"
      "            --space は参照空間の要求（既定 auto=local-floor→stage→local の後退。\n"
      "            不可なら落とさず後退し、理由をログに出す）\n"
      "            入力は毎秒サマリ（[M4入力]。その 1 秒間に押されたラッチ付き）に加え、\n"
      "            状態変化のフレームで [M4エッジ] を出す。haptics 受信は XrResult を\n"
      "            ログに残す\n"
      "  --m2-selftest  M2 の値層（配置・pose タグ・受理/棄却）を HMD 無しに検証する\n"
      "  --help    この表示\n",
      argv0);
}

bool StartsWith(const char* s, const char* prefix) {
  return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
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
      } else if (StartsWith(argv[i], "--space=")) {
        opts.space = argv[i] + std::strlen("--space=");
        if (opts.space != "auto" && opts.space != "local" && opts.space != "stage" &&
            opts.space != "local-floor") {
          xrift::Error("--space は auto/local/stage/local-floor のいずれかであること（指定=%s）",
                       opts.space.c_str());
          PrintHelp(argv[0]);
          return 1;
        }
      } else {
        // 撤去済みの実験フラグ（--transfer・--pace-records・--inject-* 等）もここで落とす。
        // 黙って無視せず usage を出して非ゼロ終了する。
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
    xrift::Info("終了: 残りの破棄を省略して終了する");
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(code);
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
