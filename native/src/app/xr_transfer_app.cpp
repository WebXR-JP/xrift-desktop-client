#include "app/xr_transfer_app.h"

#ifdef _WIN32

#include <windows.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "common/log.h"
#include "common/xr_result.h"
#include "gfx/blit_pass.h"
#include "gfx/hold_image.h"
#include "gfx/swapchains.h"
#include "app/input_message.h"
#include "ipc/control_blocks.h"
#include "ipc/pipe_client.h"
#include "ipc/protocol.h"
#include "transfer/control_block_reader.h"
#include "transfer/render_record.h"
#include "transfer/shared_texture.h"
#include "xr/input.h"
#include "xr/session.h"

namespace xrift::transfer {
namespace {

// Offer 受信ポーリングの待ち上限（1ms は意図的な値であり、仮置きではない）。
// XR ループを止めないよう 1 フレームの待ちが 1ms 程度に収まることを保証する。
// ReceiveWithDeadline の期限切れは「その回の受信を打ち切る」意味であり、
// borrow 資源の返却許可にはならない。ここでの期限は受信のポーリング間隔である。
// なお deadlineUs=0 は無期限待ちの意味であるため、ノンブロッキングのつもりで
// 現在時刻や 0 をそのまま期限に渡さないこと。
constexpr uint64_t kOfferPollWaitUs = 1000u;
// 発行フロー制御の応答停滞の上限（500ms は意図的な値であり、仮置きではない）。
// 未処理の画像がある間は新しいレコードを発行せず待ち、500ms 応答が無ければ
// 同じレコードの再送ではなく新しいレコードを発行して詰まりを解く。
// 古いレコードは TTL と掃除で失効させる。500ms は OSR の実測遅延（350〜500ms）の
// 上端に合わせ、通常の paint 間隔（約 11ms・低速時約 135ms）より十分長い。
constexpr uint64_t kPaceStallTimeoutUs = 500000u;

uint64_t NowUs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// QPC 時刻（整数 µs）。paint タグ（addon の nowQpcUs）と同じ時計であり、
// steady_clock（NowUs）と混ぜないことが呼び出し側の契約である。
uint64_t NowQpcUs() {
  static LARGE_INTEGER freq = {0};
  static int freq_once = 0;
  if (!freq_once) {
    QueryPerformanceFrequency(&freq);
    freq_once = 1;
  }
  LARGE_INTEGER now = {0};
  QueryPerformanceCounter(&now);
  if (freq.QuadPart <= 0) {
    return 0;
  }
  const double us =
      (static_cast<double>(now.QuadPart) * 1000000.0) / static_cast<double>(freq.QuadPart);
  return static_cast<uint64_t>(us);
}

EyePose ToEyePose(const XrPosef& p) {
  EyePose e{};
  e.position[0] = p.position.x;
  e.position[1] = p.position.y;
  e.position[2] = p.position.z;
  e.orientation[0] = p.orientation.x;
  e.orientation[1] = p.orientation.y;
  e.orientation[2] = p.orientation.z;
  e.orientation[3] = p.orientation.w;
  return e;
}

EyeFov ToEyeFov(const XrFovf& f) {
  EyeFov e{};
  e.left = f.angleLeft;
  e.right = f.angleRight;
  e.up = f.angleUp;
  e.down = f.angleDown;
  return e;
}

// マーカー契約の組み立て。ipc/control_blocks.h の配置定数から作り、
// gfx::BlitPass へ値で渡す（gfx 側に ipc を漏らさない）。
gfx::MarkerLayout MakeMarkerLayout() {
  gfx::MarkerLayout layout{};
  layout.left.x = kMarkerLeftX;
  layout.left.y = kMarkerY;
  layout.left.size = kMarkerSize;
  // 従来の判定（r>200 && b>200 && g<80）と同値の範囲にする。
  layout.left.color = gfx::MarkerColor{201, 255, 0, 79, 201, 255};
  layout.right.x = kMarkerRightDx;
  layout.right.y = kMarkerY;
  layout.right.size = kMarkerSize;
  // 従来の判定（g>200 && b>200 && r<80）と同値の範囲にする。
  layout.right.color = gfx::MarkerColor{0, 79, 201, 255, 201, 255};
  return layout;
}

struct XrStats {
  uint64_t frames = 0;
  uint64_t rendered = 0;
  uint64_t requests = 0;
  uint64_t offers = 0;
  uint64_t doneOk = 0;
  uint64_t blits = 0;
  uint64_t blitUsTotal = 0;
  uint64_t blitUsMax = 0;
  uint64_t submitLayers = 0;
  uint64_t emptyFrames = 0;
  uint64_t resubmits = 0;  // 新規受理が無いフレームで hold から出し直した提出数。
  uint64_t pacedWaits = 0;  // フロー制御で未処理待ちのため発行を見送ったフレーム数。
  uint64_t verifyOk = 0;
  uint64_t verifyNg = 0;
  uint64_t firstUs = 0;
  uint64_t lastUs = 0;
  // 入力・haptics・空間・再生成の統計。
  uint64_t inputSent = 0;       // main へ送った InputState の件数。
  uint64_t hapticReceived = 0;  // main から受けた HapticCommand の件数。
  uint64_t spaceChanges = 0;    // spaceEpoch を進めた回数。
  uint64_t restarts = 0;        // セッション再生成の成功回数。
  uint64_t restartAttempts = 0;  // セッション再生成の試行回数。
  uint32_t connEpoch = 1;       // 現在の接続世代。

  void Print(const RenderLedger::Stats& ledger, const xr::InputStats& input) const {
    const double spanS =
        firstUs != 0 && lastUs > firstUs ? static_cast<double>(lastUs - firstUs) / 1e6 : 0.0;
    const double fps = spanS > 0.0 ? static_cast<double>(frames) / spanS : 0.0;
    Info("XR転送統計: フレーム=%llu 描画=%llu 要求=%llu 受信=%llu Done=%llu",
         static_cast<unsigned long long>(frames), static_cast<unsigned long long>(rendered),
         static_cast<unsigned long long>(requests), static_cast<unsigned long long>(offers),
         static_cast<unsigned long long>(doneOk));
    Info("XR転送統計: fps=%.2f（XR フレーム間隔からの算出）", fps);
    Info("XR転送統計: 発行レコード=%llu 追い出し=%llu pace待機フレーム=%llu",
         static_cast<unsigned long long>(ledger.issued),
         static_cast<unsigned long long>(ledger.evicted),
         static_cast<unsigned long long>(pacedWaits));
    Info("XR転送統計: 受理=%llu 不明ID=%llu pose不一致=%llu 世代不一致=%llu 失効=%llu",
         static_cast<unsigned long long>(ledger.accepted),
         static_cast<unsigned long long>(ledger.unknownId),
         static_cast<unsigned long long>(ledger.poseMismatch),
         static_cast<unsigned long long>(ledger.spaceMismatch),
         static_cast<unsigned long long>(ledger.expired));
    const double denom = blits > 0 ? static_cast<double>(blits) : 1.0;
    Info("XR転送統計: blit=%llu 平均=%lluus 最大=%lluus 層あり=%llu 再提出=%llu 空=%llu",
         static_cast<unsigned long long>(blits),
         static_cast<unsigned long long>(blitUsTotal / denom),
         static_cast<unsigned long long>(blitUsMax),
         static_cast<unsigned long long>(submitLayers),
         static_cast<unsigned long long>(resubmits),
         static_cast<unsigned long long>(emptyFrames));
    Info("XR転送統計: 左右分離検証 ok=%llu ng=%llu",
         static_cast<unsigned long long>(verifyOk), static_cast<unsigned long long>(verifyNg));
    Info("入力統計: connEpoch=%u InputState送信=%llu haptic受信=%llu space変更=%llu",
         connEpoch, static_cast<unsigned long long>(inputSent),
         static_cast<unsigned long long>(hapticReceived),
         static_cast<unsigned long long>(spaceChanges));
    Info("入力統計: セッション再生成 成功=%llu 試行=%llu",
         static_cast<unsigned long long>(restarts),
         static_cast<unsigned long long>(restartAttempts));
    Info("入力統計: 入力 sync=%llu 失敗=%llu snapshot=%llu profile変化=%llu",
         static_cast<unsigned long long>(input.syncs),
         static_cast<unsigned long long>(input.syncFailed),
         static_cast<unsigned long long>(input.snapshots),
         static_cast<unsigned long long>(input.profileChanges));
    Info("入力統計: haptics 成功=%llu 未送信(inactive等)=%llu 失敗=%llu",
         static_cast<unsigned long long>(input.hapticOk),
         static_cast<unsigned long long>(input.hapticInactive),
         static_cast<unsigned long long>(input.hapticFailed));
  }
};

// 要求→初回提出（A）・paint→初回提出（B）の計測用。
// A は exe 内で閉じる（RenderRecord::issuedUs と提出直前の NowUs）。
// B は QPC で閉じる（paint タグと提出直前の NowQpcUs）。両時計を混ぜない。
// 同じ受理画像は hold 経由の resubmit→次フレームの fresh 提出の順で 2 回載り得るため、
// 画像ごとのローカル通番（成功コピーごとの +1）で最初の 1 回だけ数える。
struct ImageMeta {
  uint64_t seq = 0;  // 0 は無効。成功コピーごとに +1 する通番である。
  uint64_t paintTagUs = 0;  // Offer の reserved 先頭 8B（0=欠測）。
  uint64_t offerRecvQpcUs = 0;  // 各 Offer 受信直前の QPC。
  uint64_t copyDoneQpcUs = 0;  // コピー成功直後の QPC。
};

// 5 秒窓と累計で別々に持つ集計。窓と累計は同じ形であり、窓は出力後に破棄する。
struct LatencyAgg {
  uint64_t aCount = 0;
  uint64_t aSumUs = 0;
  uint64_t aMaxUs = 0;
  uint64_t bCount = 0;
  uint64_t bSumUs = 0;
  uint64_t bMaxUs = 0;
  uint64_t bPaintToOfferSumUs = 0;
  uint64_t bOfferToCopySumUs = 0;
  uint64_t bCopyToSubmitSumUs = 0;
  uint64_t endFrameCount = 0;
  uint64_t endFrameSumUs = 0;
  uint64_t endFrameMaxUs = 0;
  uint64_t tagMissing = 0;
  uint64_t tagFuture = 0;
  uint64_t orderMismatch = 0;
  uint64_t duplicateSkip = 0;
  void Reset() { *this = LatencyAgg{}; }
};

// ms 1 桁の表示用。us を ms に直す（表示だけであり、集計は us のまま）。
inline double UsToMs(uint64_t us) { return static_cast<double>(us) / 1000.0; }

// 集計 1 件分の [XR計測] 行を出す。窓と累計で共用する（prefix は空または「累計 」）。
// B の内訳平均は B 標本があるときだけ意味を持ち、無いときは 0.0 を出す。
inline void PrintLatencyLine(const char* prefix, const LatencyAgg& agg) {
  const double aAvg = agg.aCount > 0 ? UsToMs(agg.aSumUs) / static_cast<double>(agg.aCount) : 0.0;
  const double aMax = UsToMs(agg.aMaxUs);
  const double bAvg = agg.bCount > 0 ? UsToMs(agg.bSumUs) / static_cast<double>(agg.bCount) : 0.0;
  const double bMax = UsToMs(agg.bMaxUs);
  const double bDen = agg.bCount > 0 ? static_cast<double>(agg.bCount) : 1.0;
  const double bOffer = UsToMs(agg.bPaintToOfferSumUs) / bDen;
  const double bCopy = UsToMs(agg.bOfferToCopySumUs) / bDen;
  const double bSubmit = UsToMs(agg.bCopyToSubmitSumUs) / bDen;
  const double efAvg = agg.endFrameCount > 0
                          ? UsToMs(agg.endFrameSumUs) / static_cast<double>(agg.endFrameCount)
                          : 0.0;
  const double efMax = UsToMs(agg.endFrameMaxUs);
  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "[XR計測] %s要求→初回提出 平均=%.1fms 最大=%.1fms n=%llu | "
                "paint→初回提出 平均=%.1fms 最大=%.1fms n=%llu"
                "（paint→受信=%.1f 受信→コピー=%.1f コピー→提出=%.1f）| "
                "xrEndFrame 平均=%.1fms 最大=%.1fms n=%llu | "
                "タグ欠測=%llu 未来=%llu 順序不正=%llu 重複skip=%llu",
                prefix, aAvg, aMax, static_cast<unsigned long long>(agg.aCount), bAvg, bMax,
                static_cast<unsigned long long>(agg.bCount), bOffer, bCopy, bSubmit, efAvg, efMax,
                static_cast<unsigned long long>(agg.endFrameCount),
                static_cast<unsigned long long>(agg.tagMissing),
                static_cast<unsigned long long>(agg.tagFuture),
                static_cast<unsigned long long>(agg.orderMismatch),
                static_cast<unsigned long long>(agg.duplicateSkip));
  Info("%s", buf);
}

}  // namespace

int RunXrTransfer(const XrTransferOptions& options) {
  Info("XR転送開始: pipe=%s render-scale=%.2f record-ttl=%llums space=%s",
       options.pipeName.c_str(), options.renderScale,
       static_cast<unsigned long long>(options.recordTtlMs), options.space.c_str());
  Info("再提出 on（新規受理が無いフレームは最終受理を同じ pose/FOV で出す。年齢無制限）");
  Info("レコード発行はフロー制御つき（未処理がある間は発行を待ち、500ms 停滞で新しい分を発行）");
  // ADR: 終了時の非決定的クラッシュ（実測: 統計出力後の残り破棄で 0xC0000005・
  // 0xC0000409）は、明示 teardown 後に残る RAII・静的デストラクタのいずれかで
  // 起きる。必要な後始末（セッション終了・パイプ・統計 flush）は統計出力時点で
  // 済んでいるため、以後は破棄を走らせず即時終了する。
  // Info/Error は行ごとに flush するが、_Exit 前に念のため両方を flush する。
  auto quick_exit = [&](int code) {
    Info("終了: 残りの破棄を省略して終了する");
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(code);
  };

  // 転送セッションを先に作る。同一 VkDevice を XR セッションと共有し、
  // 自前イメージから swapchain への blit を同一デバイス内で行う。
  // 別デバイスにするとデバイス間コピーが要るため、この順序を変えないこと。
  TransferSession transfer;
  xr::SpacePreference space_pref = xr::SpacePreference::Auto;
  try {
    space_pref = xr::ParseSpacePreference(options.space);
  } catch (const std::exception& e) {
    Error("%s", e.what());
    return 1;
  }
  // セッション束（セッション・参照空間・swapchain・blit・入力空間）。
  // LOSS_PENDING ではこの束だけを破棄・再生成する。
  // TransferSession（instance・Vulkan device）・ControlBlockReader・PipeClient は保つ。
  // 再生成のたびに connEpoch を進め、旧世代のレコードを失効させる（docs/architecture.md「借用の所有権と release 遅延」「space とセッション復帰」）。
  std::unique_ptr<xr::Session> session;
  std::unique_ptr<gfx::Swapchains> swapchains;
  std::unique_ptr<gfx::BlitPass> blit;
  xr::InputManager input;
  uint32_t conn_epoch = 1;
  RenderLedger ledger(conn_epoch, 1, options.recordTtlMs * 1000u);
  XrStats stats{};
  stats.connEpoch = conn_epoch;
  try {
    session = std::make_unique<xr::Session>(transfer.XrInstance(), transfer.Vulkan().xrGraphicsBinding(),
                                            space_pref);
  } catch (const std::exception& e) {
    Error("XR セッションの生成に失敗: %s", e.what());
    // 生成失敗時も統計（空）とメッセージは残す。以後の RAII 破棄は
    // 飛ばす（終了時クラッシュの回避。正常経路と同一の出口）。
    stats.Print(ledger.stats(), input.stats());
    quick_exit(1);
    return 1;
  }
  try {
    input.Create(transfer.XrInstance().handle());
  } catch (const std::exception& e) {
    Error("入力アクションの生成に失敗: %s", e.what());
    stats.Print(ledger.stats(), input.stats());
    quick_exit(1);
    return 1;
  }
  if (!input.AttachSpaces(session->handle())) {
    Warn("入力アクションの attach に失敗したため入力は inactive のまま続行する");
  }
  try {
    swapchains = std::make_unique<gfx::Swapchains>(transfer.XrInstance().handle(),
                                                   session->handle(), session->views(),
                                                   gfx::XrPreferredSwapchainFormats());
  } catch (const std::exception& e) {
    Error("swapchain の生成に失敗: %s", e.what());
    stats.Print(ledger.stats(), input.stats());
    quick_exit(1);
    return 1;
  }
  if (swapchains->format() != vk::Format::eB8G8R8A8Unorm) {
    Warn("swapchain が %s（受理画像と不一致。二重符号化の疑いが残る。docs/architecture.md「既知の挙動」の未決）",
         gfx::SwapchainFormatName(swapchains->format()));
  } else {
    Info("swapchain が B8G8R8A8_UNORM（受理画像と一致。blit は複写・拡大のみ）");
  }
  blit = std::make_unique<gfx::BlitPass>(transfer.Vulkan(), *swapchains, MakeMarkerLayout());
  ControlBlockReader reader(transfer.Vulkan());
  gfx::HoldImage hold(transfer.Vulkan());
  // 接続失敗は例外で返る。統計とメッセージを残してから投げ直す
  // （呼び出し側は 1 に丸めて通常終了させる）。
  std::unique_ptr<PipeClient> pipe_owned;
  try {
    pipe_owned = std::make_unique<PipeClient>(options.pipeName);
  } catch (const std::exception& e) {
    Error("パイプ接続に失敗: %s", e.what());
    stats.Print(ledger.stats(), input.stats());
    throw;
  }
  // 以後は参照で使う（呼び出し側の記述は変えない。破棄順序も変わらない）。
  PipeClient& pipe = *pipe_owned;
  Info("パイプ接続: main プロセスと接続した");

  XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerProjectionView proj_views[2] = {
      {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
      {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
  };
  layer.space = session->space();
  layer.viewCount = 2;
  layer.views = proj_views;
  std::vector<XrView> views(2, {XR_TYPE_VIEW});

  // 受理済みの最新画像。受理確定時に hold へ GPU 複製し、以後は hold から出し直す。
  // 自前イメージは次の Offer で上書き・棄却画像で汚染されるため、提出の複製元には使わない。
  // 再提出は同じ record の pose/FOV のまま行い、最新 pose への付け替えはしない。
  bool hasFresh = false;
  RenderRecord freshRecord{};
  uint32_t freshW = 0;
  uint32_t freshH = 0;
  // 再提出用の保持。hold イメージと、その画像に対応する受理レコードの複写である。
  // 台帳の掃除・TTL 失効では消さない（黒より古い絵を優先する）。世代進行では消す。
  RenderRecord holdRecord{};
  bool holdHasRecord = false;
  // 要求→初回提出（A）・paint→初回提出（B）の計測状態。提出の挙動は変えず観測だけ行う。
  // ownMeta は自前イメージ（OwnImage）に対応する最後の成功コピーの属性であり、
  // 照合成功時に freshMeta へ、hold 複製成功時に holdMeta へ写す。
  // 世代進行（再生成・loss・space 変更）では 3 つとも seq=0 に無効化する。
  // lastCountedSeq は二重提出の重複判定用であり、世代進行でも保つ。
  uint64_t imageSeq = 0;
  ImageMeta ownMeta{};
  ImageMeta freshMeta{};
  ImageMeta holdMeta{};
  uint64_t lastCountedSeq = 0;
  LatencyAgg latencyWindow{};
  LatencyAgg latencyTotal{};
  uint64_t lastLatencyLogUs = NowUs();
  // 5 秒窓のしきい値（意図的な値であり、仮置きではない。既存の 5 秒計測に合わせる）。
  constexpr uint64_t kLatencyWindowUs = 5000000u;
  // pose 不一致診断の出力済み件数。先頭 5 件だけ詳報し、以後は集計のみにする。
  uint64_t mismatchLogCount = 0;
  // 再提出ログの前回出力時刻。毎フレーム出すと膨大になるため 1 秒に抑える。
  uint64_t lastResubmitLogUs = 0;
  bool session_running = false;
  bool quit = false;
  // セッション再生成の状態。LOSS_PENDING で束を破棄し、1 秒周期で再生成を試みる。
  // 試行上限（30 回≒30 秒）を超えたらランタイム未復帰として安全に終了する（統計出力）。
  // 上限 30・間隔 1 秒はいずれも意図的な値であり、仮置きではない。
  bool needs_rebuild = false;
  int rebuild_attempts_left = 0;
  uint64_t last_rebuild_try_us = 0;
  // REFERENCE_SPACE_CHANGE_PENDING の受付。適用はフレーム側で行う。
  bool pending_space_change = false;
  // locate 確認に使う予測表示時刻（直近の xrWaitFrame の値）。
  // 時刻 0 を渡すと受け付けないランタイムがある（実測: SteamVR は
  // XR_ERROR_TIME_INVALID）ため、running 中のフレーム時刻を使う。
  XrTime last_display_time = 0;
  // 毎秒入力ログの前回出力時刻。0 は未出力である。
  uint64_t last_input_log_us = 0;
  // 入力計測: 押下エッジ用の前フレームと 1 秒ラッチ（左右のボタン bit）。
  // ラッチは 1 秒サマリで 0 へ戻す。製品経路の buttons 欄は変えない。
  xr::InputSnapshot prev_snapshot{};
  bool have_prev_snapshot = false;
  uint8_t input_latch[2] = {0, 0};
  // パイプの生死と終了コード。切断（hard close）は新規受付の停止と統計付き終了の合図
  // であり（docs/architecture.md「借用の所有権と release 遅延」）、未完了の借用へ Done は送らない。プロトコルに終了合図が無い
  // ため、少なくともハングせず統計を残すことを優先し、正常な切断は 0、異常は非ゼロで返す。
  bool pipeAlive = true;
  int exitCode = 0;
  // 送信失敗の扱い。相手先の破棄に伴う切断（BROKEN_PIPE 等）は正常な終了手順の
  // 合図であり、受信側の切断検知と同じく統計付きの正常終了にする（exit は 0 のまま）。
  // アプリ側は未返却の借用を隔離して非ゼロ化するため、失敗の見落としにはならない。
  // それ以外の送信失敗は異常として非ゼロ終了にする。
  // 終了直前の競合（サーバ破棄と次フレーム送信の入れ違い）は切断側に倒す。
  auto note_send_failure = [&](const char* what, const std::exception& e) {
    const PipeError* pipe_error = dynamic_cast<const PipeError*>(&e);
    // 相手先の破棄に伴う切断コード（いずれも「パイプは閉じられた」の変種）。
    // GetOverlappedResult/WriteFile の失敗理由として実測した 232 を含む。
    const bool graceful = pipe_error != nullptr &&
                          (pipe_error->code == ERROR_BROKEN_PIPE ||
                           pipe_error->code == ERROR_NO_DATA ||
                           pipe_error->code == ERROR_PIPE_NOT_CONNECTED);
    if (graceful) {
      Info("%s送信先の切断を検知（Win32=%lu）のため新規受付を止めて終了する", what,
           static_cast<unsigned long>(pipe_error->code));
    } else {
      Error("%s送信に失敗: %s", what, e.what());
      exitCode = 1;
    }
    pipeAlive = false;
    quit = true;
  };
  uint64_t frame_no = 0;
  uint64_t droppedIntermediate = 0;
  // 発行フロー制御の未処理レコードの有無。前回発行分の Offer 処理が済むまで
  // 新規発行を止め、500ms 停滞したら新しい分で解く。
  bool paceOutstanding = false;
  uint64_t paceIssuedUs = 0;
  const uint64_t deadlineUs =
      options.seconds > 0 ? NowUs() + static_cast<uint64_t>(options.seconds) * 1000000u : 0;

  // セッション束の破棄・再生成。LOSS_PENDING で使う。
  // 成功時は新しい connEpoch で ledger を切り替え、旧世代のレコードを失効させる。
  // 入力空間・swapchain・blit は作り直し、instance・device・reader・pipe は保つ。
  // 失敗時は false を返す（呼び出し側は再試行か安全な終了を選ぶ）。
  // 再試行の上限 30 回・間隔 1 秒はいずれも意図的な値であり、仮置きではない。
  static constexpr int kRebuildMaxAttempts = 30;
  static constexpr uint64_t kRebuildRetryIntervalUs = 1000000u;
  auto rebuild_session = [&](const char* why) -> bool {
    ++stats.restartAttempts;
    Info("セッション再生成を開始（%s）。connEpoch=%u→%u", why, conn_epoch, conn_epoch + 1);
    input.DetachSpaces();
    blit.reset();
    swapchains.reset();
    session.reset();
    session_running = false;
    hasFresh = false;
    hold.Invalidate();
    holdHasRecord = false;
    ownMeta.seq = 0;
    freshMeta.seq = 0;
    holdMeta.seq = 0;
    ++conn_epoch;
    ledger.AdvanceConnEpoch(conn_epoch);
    stats.connEpoch = conn_epoch;
    try {
      session = std::make_unique<xr::Session>(transfer.XrInstance(),
                                                 transfer.Vulkan().xrGraphicsBinding(), space_pref);
    } catch (const std::exception& e) {
      Warn("セッション再生成に失敗（%s）: %s", why, e.what());
      return false;
    }
    if (input.hasActionSet()) {
      if (!input.AttachSpaces(session->handle())) {
        Warn("再生成後の attach に失敗したため入力は inactive のまま続行する");
      }
    }
    try {
      swapchains = std::make_unique<gfx::Swapchains>(transfer.XrInstance().handle(),
                                                     session->handle(), session->views(),
                                                     gfx::XrPreferredSwapchainFormats());
      blit = std::make_unique<gfx::BlitPass>(transfer.Vulkan(), *swapchains, MakeMarkerLayout());
    } catch (const std::exception& e) {
      Warn("再生成後の swapchain 生成に失敗（%s）: %s", why, e.what());
      session.reset();
      return false;
    }
    layer.space = session->space();
    ++stats.restarts;
    Info("セッション再生成に成功（%s）。connEpoch=%u space=%s", why, conn_epoch,
         xr::SpaceTypeName(session->spaceType()));
    return true;
  };

  // REFERENCE_SPACE_CHANGE_PENDING の適用。新しい原点で locate できることを確認してから
  // spaceEpoch を進め、旧世代のレコードを失効させる（docs/architecture.md「space とセッション復帰」）。
  auto apply_pending_space = [&]() {
    if (!pending_space_change || !session) {
      return;
    }
    if (session->RecreateSpaceForPendingChange(last_display_time)) {
      ledger.AdvanceSpaceEpoch(ledger.spaceEpoch() + 1);
      hasFresh = false;
      hold.Invalidate();
      holdHasRecord = false;
      ownMeta.seq = 0;
      freshMeta.seq = 0;
      holdMeta.seq = 0;
      ++stats.spaceChanges;
      Info("reference space 変更を適用: spaceEpoch=%u（旧レコードを失効）", ledger.spaceEpoch());
    } else {
      Warn("reference space 変更の適用に失敗したため旧空間・旧世代を保つ");
    }
    pending_space_change = false;
  };

  auto poll_events = [&]() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(transfer.XrInstance().handle(), &event) == XR_SUCCESS) {
      switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
          const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
          if (!session || changed->session != session->handle()) {
            break;
          }
          session->setState(changed->state);
          Info("session 状態: %d (connEpoch=%u)", static_cast<int>(changed->state), conn_epoch);
          switch (changed->state) {
            case XR_SESSION_STATE_READY:
              if (!session_running) {
                session->Begin();
                session_running = true;
                Info("セッション開始");
              }
              break;
            case XR_SESSION_STATE_STOPPING:
              // STOPPING → xrEndSession。脱着は loss とは限らないため束は保つ
              // （focus/visible/shouldRender に従って提出だけ止める）。
              if (session_running) {
                session->End();
                session_running = false;
                Info("セッション終了（STOPPING。束は保持し READY で再開する）");
              }
              break;
            case XR_SESSION_STATE_EXITING:
              // EXITING はランタイムの終了要求であり、再生成せず安全に終わる。
              quit = true;
              break;
            case XR_SESSION_STATE_LOSS_PENDING:
              // loss 時は束を破棄し、新しい connEpoch で再生成する。
              // ランタイムが復帰しなければ再試行の末に安全に終了する。
              Info("セッション loss を検知したため束を破棄し再生成する");
              input.DetachSpaces();
              blit.reset();
              swapchains.reset();
              session.reset();
              session_running = false;
              hasFresh = false;
              hold.Invalidate();
              holdHasRecord = false;
              ownMeta.seq = 0;
              freshMeta.seq = 0;
              holdMeta.seq = 0;
              needs_rebuild = true;
              rebuild_attempts_left = kRebuildMaxAttempts;
              last_rebuild_try_us = 0;
              break;
            default:
              break;
          }
          break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
          quit = true;
          break;
        case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
          // 実 profile の読みは毎フレームの Snapshot 側で行い、変化を記録する。
          Info("interaction profile 変更イベントを受信（次フレームの取得で確認する）");
          break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
          // 適用時刻に従い、新しい原点で locate できるタイミングで世代を進める。
          // ここでは受付だけ行い、適用はフレーム側で行う。
          pending_space_change = true;
          Info("reference space 変更予定を受信（次フレームで適用を試みる）");
          break;
        default:
          break;
      }
      event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
  };

  // Offer の受け皿。最新 1 件だけ照合し、中間は Done だけ返す。
  // 中間画像の受理は行わない（最新優先の方針。破棄数は統計に出る）。
  // shouldRender が false のフレーム（SYNCHRONIZED 等）でも毎フレーム呼ぶこと。
  // 受信・ACK は描画可否と無関係に進め、止めるのは提出だけである。
  // 同じパイプで main → exe の HapticCommand も受ける。Offer とは別扱いにする。
  auto drain_offers = [&]() {
    TransferMessage msg{};
    bool have_last = false;
    for (;;) {
      bool timed_out = false;
      // ポーリング用の期限は「今 + 猶予」とする。現在時刻そのままでは
      // ReceiveWithDeadline 冒頭の期限到達判定が即 true になり ReadFile を
      // 一度も試さずに返るため、現在時刻をそのまま期限に渡さないこと。
      // 期限切れは返却許可ではなく、その回の受信の打ち切りにすぎない。
      // ここでの期限は受信のポーリング間隔である。
      const uint64_t poll_deadline = NowUs() + kOfferPollWaitUs;
      bool received = false;
      try {
        received = pipe.ReceiveWithDeadline(&msg, poll_deadline, &timed_out);
      } catch (const std::exception& e) {
        Error("パイプ受信に失敗: %s", e.what());
        pipeAlive = false;
        exitCode = 1;
        quit = true;
        return;
      }
      if (!received) {
        // 空振り（期限切れ）は正常であり、borrow 資源の返却許可にはならない。
        // 切断時は新規受付を止め、現在の処理を安全に終わらせて統計を出して終了する。
        // 未完了の借用へ Done は送らない（隔離。docs/architecture.md「借用の所有権と release 遅延」）。
        if (!timed_out) {
          Info("パイプ切断を検知したため新規受付を止めて終了する");
          pipeAlive = false;
          quit = true;
          return;
        }
        break;
      }
      // main → exe の HapticCommand は Offer とは別扱いにする。
      // producer 生存の証拠には数えず、借用台帳にも触れない。
      // セッションが無い・実行中でない場合は inactive として記録する（例外にしない）。
      if (msg.magic == kMagic && msg.version == kVersion && msg.type == kTypeHapticCommand) {
        if (!IsValidHapticCommand(msg)) {
          Warn("不正な HapticCommand を破棄（hand/amplitude/duration の不一致）");
          continue;
        }
        ++stats.hapticReceived;
        const uint32_t hand = HapticHand(msg);
        const float amp = HapticAmplitude(msg);
        const uint32_t dur = HapticDurationMs(msg);
        // FOCUSED でなければ送らず inactive に数える（入力の sync と同じ基準）。
        const bool haptic_focused =
            session && session->state() == XR_SESSION_STATE_FOCUSED;
        bool sent = false;
        if (session) {
          sent = input.ApplyHaptic(session->handle(), hand, amp, dur, haptic_focused);
        } else {
          input.ApplyHaptic(XR_NULL_HANDLE, hand, amp, dur, false);
        }
        // 送信結果は XrResult で残す（VkResult ではない。実機で「成功だが振動不明」の
        // 切り分け用）。inactive（未呼出し）と呼出し失敗は区別して出す。
        if (sent) {
          Info("haptics 送信: %s 振幅=%.2f 持続=%ums（XrResult=%s）", hand == 0 ? "左" : "右",
               amp, dur, XrResultToString(input.lastHapticResult()));
        } else if (input.lastHapticCalled()) {
          Info("haptics 未送信: %s 振幅=%.2f 持続=%ums（呼出し失敗 XrResult=%s。統計に残る）",
               hand == 0 ? "左" : "右", amp, dur, XrResultToString(input.lastHapticResult()));
        } else {
          Info("haptics 未送信: %s 振幅=%.2f 持続=%ums（inactive のため送らず。統計に残る）",
               hand == 0 ? "左" : "右", amp, dur);
        }
        continue;
      }
      if (!IsValidOffer(msg)) {
        Warn("不正な Offer を破棄（magic/version/type/format の不一致）");
        continue;
      }
      ++stats.offers;
      // 計測は観測だけであり、受信・ACK の挙動は変えない。
      // msg はバッチ内で上書きされ得るため、タグはこの反復で読み切る。
      // ProcessOffer は成功・失敗を問わず OwnImage を作り直し得るため、
      // 処理の前に pending の freshMeta の対応付けを切る。照合成功時に
      // freshMeta = ownMeta で再関連付けされる。早期 return でも 0 のままなので
      // 誤った標本を作らない。Offer を受信しないフレームでは触らない。
      freshMeta.seq = 0;
      const uint64_t offerRecvQpcUs = NowQpcUs();
      const uint64_t offerPaintTagUs = OfferPaintQpcUs(msg);
      const uint32_t status = transfer.ProcessOffer(msg.handle, msg.width, msg.height);
      if (status == kStatusOk || status == kStatusVerifyFailed) {
        const uint64_t copyDoneQpcUs = NowQpcUs();
        ++imageSeq;
        ownMeta.seq = imageSeq;
        ownMeta.paintTagUs = offerPaintTagUs;
        ownMeta.offerRecvQpcUs = offerRecvQpcUs;
        ownMeta.copyDoneQpcUs = copyDoneQpcUs;
      } else {
        // コピー失敗時は OwnImage と属性の対応を保証できない。
        // 寸法変更時は自前イメージを作り直してからコピーするため、
        // 失敗後の OwnImage が前回成功画像とは限らない。保守的に B の標本だけ落とし、
        // 画像の受理・提出・棄却は変えない。次の成功コピーで再開する。
        ownMeta.seq = 0;
      }
      if (status == kStatusNoAck) {
        Error("GPU 完了未確認のため受付を停止して終了する（borrow への ACK なし）");
        pipeAlive = false;
        exitCode = 2;
        quit = true;
        return;
      }
      if (status == kStatusOk) {
        ++stats.doneOk;
      }
      const TransferMessage done = MakeDone(msg.borrowId, status);
      try {
        pipe.Send(done);
      } catch (const std::exception& e) {
        note_send_failure("ACK", e);
        return;
      }
      if (have_last) {
        ++droppedIntermediate;
      }
      have_last = true;
    }
    if (!have_last) {
      return;
    }
    // 最新 Offer の自前イメージから制御メタだけを読み、台帳と照合する。
    // 画像全体は読まない（docs/architecture.md「経路」の制御メタデータの例外）。
    hasFresh = false;
    const vk::Image own = transfer.OwnImage();
    if (static_cast<VkImage>(own) == VK_NULL_HANDLE) {
      return;
    }
    uint16_t id16 = 0;
    uint16_t tag = 0;
    try {
      reader.Decode(own, &id16, &tag);
    } catch (const std::exception& e) {
      Warn("制御ブロック readback に失敗: %s", e.what());
      return;
    }
    const RenderRecord* hit = nullptr;
    const RejectReason reason = ledger.Accept(id16, tag, ledger.spaceEpoch(), NowUs(), &hit);
    if (reason != RejectReason::Accepted || hit == nullptr) {
      // pose 不一致の切り分け用に先頭 5 件だけ詳報する。復号値と、同じ id16 を持つ
      // 台帳レコードの期待値を並べ、shim 側の付番・pipe 経由の取り違え・画像破損を区別する。
      // 毎回は出さない（72Hz でのログ肥大の防止）。
      if (mismatchLogCount < 5 &&
          (reason == RejectReason::PoseMismatch || reason == RejectReason::UnknownId)) {
        const RenderRecord* cand = nullptr;
        uint16_t want = 0;
        if (ledger.LookupById16(id16, &cand, &want) && cand != nullptr) {
          Info("XR 棄却診断[%llu]: %s decoded id16=0x%04x tag=0x%04x / 台帳 renderId=%llu"
               " wantTag=0x%04x conn=%u space=%u（台帳現世代 conn=%u space=%u）/ ledger最新=%llu",
               static_cast<unsigned long long>(mismatchLogCount),
               RejectReasonName(reason), id16, tag,
               static_cast<unsigned long long>(cand->renderId), want, cand->connEpoch,
               cand->spaceEpoch, ledger.connEpoch(), ledger.spaceEpoch(),
               static_cast<unsigned long long>(ledger.latestRenderId()));
        } else {
          Info("XR 棄却診断[%llu]: %s decoded id16=0x%04x tag=0x%04x / 台帳にid16一致なし"
               "（現世代 conn=%u space=%u）/ ledger最新=%llu",
               static_cast<unsigned long long>(mismatchLogCount),
               RejectReasonName(reason), id16, tag, ledger.connEpoch(), ledger.spaceEpoch(),
               static_cast<unsigned long long>(ledger.latestRenderId()));
        }
        ++mismatchLogCount;
      }
      Info("XR 棄却: %s（id16=0x%04x tag=0x%04x）", RejectReasonName(reason), id16, tag);
      return;
    }
    freshRecord = *hit;
    freshW = transfer.OwnWidth();
    freshH = transfer.OwnHeight();
    hasFresh = true;
    // 照合成功時の画像は ownMeta（最後に成功したコピー）に対応する。
    // TTL 失効等の棄却時は hasFresh=false のままで、ownMeta は保持する
    // （OwnImage と一致しているため）。
    freshMeta = ownMeta;
  };

  // XRループの例外（xrWaitFrame 等の CheckXr 失敗）は統計付きで終了する。
  // 統計欠落と abort（未捕捉例外の即死）を避けるための境界である。
  // なお通常の終了（秒数・切断・途絶）は break で抜け、ここには来ない。
  try {
  while (!quit) {
    if (deadlineUs != 0 && NowUs() >= deadlineUs) {
      Info("指定秒数を経過したため終了する");
      break;
    }
    poll_events();
    if (quit) {
      break;
    }
    // loss 後の再生成の再試行（1 秒周期）。上限を超えたらランタイム未復帰として
    // 安全に終了する（統計出力。非ゼロにはしない。こちらの破損ではないため）。
    if (needs_rebuild && !session && rebuild_attempts_left > 0 &&
        NowUs() - last_rebuild_try_us >= kRebuildRetryIntervalUs) {
      last_rebuild_try_us = NowUs();
      --rebuild_attempts_left;
      if (rebuild_session("loss 後の再試行")) {
        needs_rebuild = false;
      } else if (rebuild_attempts_left == 0) {
        Info("ランタイムが復帰しないため安全に終了する（再試行の上限に到達）");
        break;
      }
    }
    // 参照空間変更の適用は running 中のフレーム時刻が要るため、
    // xrWaitFrame の後（running 側）で行う。実行中でない間は受付だけ保ち、
    // 適用は開始後の最初のフレームまで待つ。
    if (!session_running || !session) {
      // 実行中でない間も Offer の受付・ACK と切断検知は進める。受信した Offer は
      // drain_offers が Done を返すため、消費して返せなくなることはない。
      const uint64_t offersBeforeIdle = stats.offers;
      drain_offers();
      if (stats.offers != offersBeforeIdle) {
        paceOutstanding = false;
      }
      ledger.SweepExpired(NowUs());
      if (quit) {
        break;
      }
      // 実行中でない間は眠らせ、ビジーループにしない。
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    if (stats.frames == 0) {
      stats.firstUs = NowUs();
    }
    ++frame_no;
    ++stats.frames;
    stats.lastUs = NowUs();

    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    CheckXr(xrWaitFrame(session->handle(), &wait_info, &frame_state), "xrWaitFrame");
    last_display_time = frame_state.predictedDisplayTime;
    // 参照空間変更の適用（新しい原点で locate できる確認が取れてから世代を進める）。
    // 当フレームの予測表示時刻を使う（時刻 0 を渡すと受け付けないランタイムがある）。
    apply_pending_space();
    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    CheckXr(xrBeginFrame(session->handle(), &begin_info), "xrBeginFrame");

    bool rendered = false;
    if (frame_state.shouldRender == XR_TRUE) {
      XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
      locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
      locate_info.displayTime = frame_state.predictedDisplayTime;
      locate_info.space = session->space();
      XrViewState view_state{XR_TYPE_VIEW_STATE};
      uint32_t view_count = 0;
      CheckXr(xrLocateViews(session->handle(), &locate_info, &view_state,
                            static_cast<uint32_t>(views.size()), &view_count, views.data()),
              "xrLocateViews");
      const bool pose_valid =
          (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0 &&
          (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
      if (view_count == views.size() && pose_valid) {
        // 要求アトラス寸法は左右各眼の描画倍率から求める。swapchain 自体は
        // XR 推奨サイズのまま作る（VRAM 圧迫を避ける。既定 0.5）。
        const vk::Extent2D eye0 = swapchains->extent(0);
        const vk::Extent2D eye1 = swapchains->extent(1);
        const auto scaled = [](uint32_t v, double s) {
          return static_cast<uint32_t>(static_cast<double>(v) * s + 0.5);
        };
        const uint32_t req_w = scaled(eye0.width, options.renderScale) +
                               scaled(eye1.width, options.renderScale);
        const uint32_t half_h0 = scaled(eye0.height, options.renderScale);
        const uint32_t half_h1 = scaled(eye1.height, options.renderScale);
        const uint32_t req_h = half_h0 > half_h1 ? half_h0 : half_h1;
        const EyePose left = ToEyePose(views[0].pose);
        const EyePose right = ToEyePose(views[1].pose);
        const EyeFov lf = ToEyeFov(views[0].fov);
        const EyeFov rf = ToEyeFov(views[1].fov);
        // 発行フロー制御: 未処理の画像がある間は新しいレコードを発行しない
        // （RenderRequest を送らない）。Offer 処理後の次のフレームで発行する。
        // 応答が無いまま 500ms 経過したら同じレコードの再送ではなく新しい
        // レコードを発行して詰まらないようにする（古い分は失効させる）。
        bool issue_record = true;
        if (paceOutstanding) {
          if (NowUs() - paceIssuedUs > kPaceStallTimeoutUs) {
            Info("応答が %llums 無いため新しいレコードを発行する（旧分は失効させる）",
                 static_cast<unsigned long long>(kPaceStallTimeoutUs / 1000u));
            paceOutstanding = false;
          } else {
            ++stats.pacedWaits;
            issue_record = false;
          }
        }
        if (issue_record) {
          const uint64_t render_id = frame_no;  // 0 を避けるため 1 起点の通番を使う。
          const RenderRecord& rec =
              ledger.Issue(render_id, left, right, lf, rf, req_w, req_h, NowUs());
          TransferMessage req = MakeRenderRequest(rec.renderId, rec.connEpoch, rec.spaceEpoch,
                                                  rec.left, rec.right, rec.leftFov, rec.rightFov,
                                                  req_w, req_h);
          try {
            pipe.Send(req);
            ++stats.requests;
          } catch (const std::exception& e) {
            note_send_failure("RenderRequest", e);
          }
          rendered = true;
          paceOutstanding = true;
          paceIssuedUs = NowUs();
        }
      }
    }

    // Offer の受信・ACK は shouldRender が false のフレームでも毎フレーム行う
    //（SYNCHRONIZED 中でも Offer を受けて ACK できること）。提出だけを
    // shouldRender（rendered）で止める。drain_offers 内の期限切れは返却許可ではなく、
    // ここでの期限は受信のポーリング間隔である。
    const uint64_t offersBefore = stats.offers;
    drain_offers();
    if (stats.offers != offersBefore) {
      // Offer を処理（受理/棄却/中間破棄）し終えたため、次のフレームで
      // 新しいレコードを発行できる（フロー制御の待ちの解除）。
      paceOutstanding = false;
    }
    ledger.SweepExpired(NowUs());
    // 入力の取得と main への送信。FOCUSED 時に sync し、それ以外は inactive の
    // スナップショットとして出す（「0」と「未対応」を区別できる形。毎フレーム送信し、
    // ログは毎秒に抑える）。脱着時は focus を失うため提出だけ止め、受信・送信は続ける。
    if (!quit) {
      const bool focused = session->state() == XR_SESSION_STATE_FOCUSED;
      input.Sync(session->handle(), focused);
      const xr::InputSnapshot snapshot = input.Snapshot(
          session->handle(), session->space(), frame_state.predictedDisplayTime, focused, frame_no);
      // 入力計測: 押下エッジの記録。1 秒周期のスナップショットでは短い押下を
      // 取りこぼすため、状態が変化したフレームで 1 行出す（変化が無ければ出さない）。
      if (have_prev_snapshot) {
        const std::string edge = xr::InputManager::DescribeEdges(prev_snapshot, snapshot);
        if (!edge.empty()) {
          Info("[入力エッジ] frame=%llu %s", static_cast<unsigned long long>(frame_no),
               edge.c_str());
        }
      }
      prev_snapshot = snapshot;
      have_prev_snapshot = true;
      input_latch[0] |= xr::InputManager::PressedBits(snapshot.hands[0]);
      input_latch[1] |= xr::InputManager::PressedBits(snapshot.hands[1]);
      try {
        pipe.Send(MakeInputStateMessage(snapshot, conn_epoch, ledger.spaceEpoch(), input_latch[0],
                                        input_latch[1]));
        ++stats.inputSent;
      } catch (const std::exception& e) {
        note_send_failure("InputState", e);
      }
      if (NowUs() - last_input_log_us >= 1000000u) {
        last_input_log_us = NowUs();
        Info("[入力] %s",
             xr::InputManager::SummarizeWithLatch(snapshot, input_latch[0], input_latch[1])
                 .c_str());
        input_latch[0] = input_latch[1] = 0;
      }
    }
    // 提出は受理画像のレコードの pose/FOV で行う。最新 pose への付け替えはしない。
    // 新規受理が無いフレームでも hold の複製を同じ pose/FOV で出し直す（黒防止）。
    // 最初の 1 枚が来る前だけ空フレームにする。受理から record-ttl 超の新規受理は
    // 失効として棄却するが、hold の再提出は別枠（既定では無制限）で行う。
    XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
    end_info.displayTime = frame_state.predictedDisplayTime;
    end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
    bool submit_layer = false;
    bool is_resubmit = false;
    // src 画像（GENERAL 駐めのアトラス）の左右半分を各眼へ blit し、
    // record の pose/FOV を提出層に載せる。新規受理と再提出で共用する。
    // 失敗時は例外にする（呼び出し側は空フレームで終える）。
    auto submit_image = [&](vk::Image src, uint32_t srcW, uint32_t srcH,
                            const RenderRecord& rec) {
      const gfx::BlitPass::Result r0 = blit->Blit(0, src, srcW, srcH, 0);
      const gfx::BlitPass::Result r1 = blit->Blit(1, src, srcW, srcH, 1);
      const uint64_t blit_us = r0.blitUs + r1.blitUs;
      ++stats.blits;
      stats.blitUsTotal += blit_us;
      if (blit_us > stats.blitUsMax) {
        stats.blitUsMax = blit_us;
      }
      if (r0.markerOk && r1.markerOk) {
        ++stats.verifyOk;
      } else {
        ++stats.verifyNg;
      }
      for (uint32_t eye = 0; eye < 2; ++eye) {
        const EyePose& p = (eye == 0) ? rec.left : rec.right;
        const EyeFov& f = (eye == 0) ? rec.leftFov : rec.rightFov;
        proj_views[eye].pose.position.x = p.position[0];
        proj_views[eye].pose.position.y = p.position[1];
        proj_views[eye].pose.position.z = p.position[2];
        proj_views[eye].pose.orientation.x = p.orientation[0];
        proj_views[eye].pose.orientation.y = p.orientation[1];
        proj_views[eye].pose.orientation.z = p.orientation[2];
        proj_views[eye].pose.orientation.w = p.orientation[3];
        proj_views[eye].fov.angleLeft = f.left;
        proj_views[eye].fov.angleRight = f.right;
        proj_views[eye].fov.angleUp = f.up;
        proj_views[eye].fov.angleDown = f.down;
        proj_views[eye].subImage.swapchain = swapchains->handle(eye);
        proj_views[eye].subImage.imageRect.offset = {0, 0};
        proj_views[eye].subImage.imageRect.extent.width =
            static_cast<int32_t>(swapchains->extent(eye).width);
        proj_views[eye].subImage.imageRect.extent.height =
            static_cast<int32_t>(swapchains->extent(eye).height);
        proj_views[eye].subImage.imageArrayIndex = 0;
      }
    };
    // 新規受理があれば hold へ GPU 複製してから提出する。TTL 超の受理でも複製は残す
    // （再提出は黒より古い絵を優先するため、年齢の上限は付けない）。
    if (hasFresh) {
      try {
        hold.SaveFrom(transfer.OwnImage(), freshW, freshH);
        holdRecord = freshRecord;
        holdHasRecord = true;
        holdMeta = freshMeta;
      } catch (const std::exception& e) {
        Warn("hold 複製に失敗したため再提出はできない: %s", e.what());
        hold.Invalidate();
        holdHasRecord = false;
        holdMeta.seq = 0;
      }
    }
    bool fresh_failed = false;
    // 計測は観測だけであり、提出・blit の挙動は変えない。候補の選択だけ行う。
    ImageMeta submitCandidate{};
    RenderRecord submitRec{};
    bool haveSubmitCandidate = false;
    uint64_t submitSteadyUs = 0;
    uint64_t submitQpcUs = 0;
    if (rendered && hasFresh) {
      const uint64_t age = NowUs() - freshRecord.issuedUs;
      if (age <= ledger.maxAgeUs()) {
        try {
          submit_image(transfer.OwnImage(), freshW, freshH, freshRecord);
          Info("XR 提出: renderId=%llu 年齢=%lluus（画像対応レコードの pose/FOV）",
               static_cast<unsigned long long>(freshRecord.renderId),
               static_cast<unsigned long long>(age));
          submit_layer = true;
          submitCandidate = freshMeta;
          submitRec = freshRecord;
          haveSubmitCandidate = true;
          hasFresh = false;
        } catch (const std::exception& e) {
          Warn("blit に失敗したため空フレームで終える: %s", e.what());
          fresh_failed = true;
        }
      } else {
        hasFresh = false;
      }
    }
    // 再提出。新規受理が無い・使えないフレームでも最後の受理を同じ pose/FOV で出し直す。
    // 新規 blit の失敗直後は出さない（同じ原因で落ちる重複を避ける）。
    // shouldRender が偽のフレームでも層を出す。最初の 1 枚が来る前だけ空にする。
    if (!submit_layer && !fresh_failed && blit && hold.Valid() && holdHasRecord) {
      const uint64_t age = NowUs() - holdRecord.issuedUs;
      try {
        submit_image(hold.image(), hold.width(), hold.height(), holdRecord);
        ++stats.resubmits;
        const uint64_t now = NowUs();
        if (stats.resubmits <= 3 || now - lastResubmitLogUs >= 1000000u) {
          lastResubmitLogUs = now;
          Info("XR 再提出: renderId=%llu 年齢=%lluus（最終受理の pose/FOV のまま）",
               static_cast<unsigned long long>(holdRecord.renderId),
               static_cast<unsigned long long>(age));
        }
        submit_layer = true;
        is_resubmit = true;
        submitCandidate = holdMeta;
        submitRec = holdRecord;
        haveSubmitCandidate = true;
      } catch (const std::exception& e) {
        Warn("再提出の blit に失敗したため空フレームで終える: %s", e.what());
      }
    }
    if (submit_layer) {
      end_info.layerCount = 1;
      end_info.layers = layers;
      ++stats.rendered;
      if (!is_resubmit) {
        ++stats.submitLayers;
      }
    } else {
      end_info.layerCount = 0;
      ++stats.emptyFrames;
    }
    // 計測終点は xrEndFrame 直前で取る（blit の 2 眼分コピーとフェンス待ちを含む）。
    // 層の提出が確定した後・xrEndFrame 呼出しの直前であり、A/B の終点を一致させる。
    // xrEndFrame の呼出し時間計測（efBegin/efEnd）はそのまま行う。
    if (submit_layer && haveSubmitCandidate) {
      submitSteadyUs = NowUs();
      submitQpcUs = NowQpcUs();
    }
    // xrEndFrame の呼出し時間（QPC 前後差）を全フレームで別集計する。
    // 失敗（例外）時は記録しない。計測のために画像を棄却・遅延しない。
    const uint64_t efBeginQpcUs = NowQpcUs();
    CheckXr(xrEndFrame(session->handle(), &end_info), "xrEndFrame");
    const uint64_t efEndQpcUs = NowQpcUs();
    const uint64_t efUs = efEndQpcUs >= efBeginQpcUs ? efEndQpcUs - efBeginQpcUs : 0;
    latencyWindow.endFrameCount += 1;
    latencyWindow.endFrameSumUs += efUs;
    if (efUs > latencyWindow.endFrameMaxUs) {
      latencyWindow.endFrameMaxUs = efUs;
    }
    latencyTotal.endFrameCount += 1;
    latencyTotal.endFrameSumUs += efUs;
    if (efUs > latencyTotal.endFrameMaxUs) {
      latencyTotal.endFrameMaxUs = efUs;
    }
    // 初回提出だけを標本にする。同じ seq の再提出は重複スキップに数える。
    if (submit_layer && haveSubmitCandidate && submitCandidate.seq != 0) {
      if (lastCountedSeq == submitCandidate.seq) {
        latencyWindow.duplicateSkip += 1;
        latencyTotal.duplicateSkip += 1;
      } else {
        lastCountedSeq = submitCandidate.seq;
        const uint64_t reqToSubmitUs =
            submitSteadyUs >= submitRec.issuedUs ? submitSteadyUs - submitRec.issuedUs : 0;
        latencyWindow.aCount += 1;
        latencyWindow.aSumUs += reqToSubmitUs;
        if (reqToSubmitUs > latencyWindow.aMaxUs) {
          latencyWindow.aMaxUs = reqToSubmitUs;
        }
        latencyTotal.aCount += 1;
        latencyTotal.aSumUs += reqToSubmitUs;
        if (reqToSubmitUs > latencyTotal.aMaxUs) {
          latencyTotal.aMaxUs = reqToSubmitUs;
        }
        // QPC 標本は順序を検証し、欠測・未来・順序不正は B から外す（A には含める）。
        // 負値の丸めはしない（順序違反として数える）。
        const uint64_t tag = submitCandidate.paintTagUs;
        const uint64_t recv = submitCandidate.offerRecvQpcUs;
        const uint64_t copy = submitCandidate.copyDoneQpcUs;
        if (tag == 0) {
          latencyWindow.tagMissing += 1;
          latencyTotal.tagMissing += 1;
        } else if (tag > recv) {
          latencyWindow.tagFuture += 1;
          latencyTotal.tagFuture += 1;
        } else if (!(recv <= copy && copy <= submitQpcUs && tag <= submitQpcUs)) {
          latencyWindow.orderMismatch += 1;
          latencyTotal.orderMismatch += 1;
        } else {
          const uint64_t paintToSubmitUs = submitQpcUs - tag;
          const uint64_t paintToOfferUs = recv - tag;
          const uint64_t offerToCopyUs = copy - recv;
          const uint64_t copyToSubmitUs = submitQpcUs - copy;
          latencyWindow.bCount += 1;
          latencyWindow.bSumUs += paintToSubmitUs;
          if (paintToSubmitUs > latencyWindow.bMaxUs) {
            latencyWindow.bMaxUs = paintToSubmitUs;
          }
          latencyWindow.bPaintToOfferSumUs += paintToOfferUs;
          latencyWindow.bOfferToCopySumUs += offerToCopyUs;
          latencyWindow.bCopyToSubmitSumUs += copyToSubmitUs;
          latencyTotal.bCount += 1;
          latencyTotal.bSumUs += paintToSubmitUs;
          if (paintToSubmitUs > latencyTotal.bMaxUs) {
            latencyTotal.bMaxUs = paintToSubmitUs;
          }
          latencyTotal.bPaintToOfferSumUs += paintToOfferUs;
          latencyTotal.bOfferToCopySumUs += offerToCopyUs;
          latencyTotal.bCopyToSubmitSumUs += copyToSubmitUs;
        }
      }
    }
    // 5 秒窓の出力。初回提出が無くても xrEndFrame の窓は出す。
    // 新規画像が止まり hold 再提出だけが続く間も、xrEndFrame 時間や重複 skip の
    // 蓄積が見えないと停止時の切り分けに困るため、endFrame の標本でも出力する。
    // セッション非実行中はフレーム自体が回らないため、従来どおりログは出ない。
    {
      const uint64_t nowLatUs = NowUs();
      if (nowLatUs - lastLatencyLogUs >= kLatencyWindowUs) {
        lastLatencyLogUs = nowLatUs;
        if (latencyWindow.aCount > 0 || latencyWindow.endFrameCount > 0) {
          PrintLatencyLine("", latencyWindow);
        }
        latencyWindow.Reset();
      }
    }
  }
  } catch (const std::exception& e) {
    Error("XRループの異常終了: %s（統計を出して終了する）", e.what());
    pipeAlive = false;
    exitCode = 1;
  }

  stats.Print(ledger.stats(), input.stats());
  PrintLatencyLine("累計 ", latencyTotal);
  Info("XR 中間破棄: %llu（最新優先のため Done のみ返した件数）",
       static_cast<unsigned long long>(droppedIntermediate));
  if (stats.submitLayers == 0 && stats.resubmits == 0) {
    Warn("提出層 0 のまま終了した（受理画像が無い。ページ側の描画を確認すること）");
  }
  if (!pipeAlive) {
    Info("パイプ終了に伴う終了（exit=%d）", exitCode);
  }
  // セッション束は明示の順序で破棄する（blit → swapchain → session）。
  // なお実行中のまま xrDestroySession しても SteamVR は受け付けることを確認済み
  // （終了時 exit 0）。STOPPING 未満での xrEndSession は NOT_STOPPING で失敗する
  // ため、ここであえて End は呼ばない（イベント経路の End に任せる）。
  blit.reset();
  swapchains.reset();
  session.reset();
  // 残りの RAII（TransferSession・reader・pipe 等）と静的デストラクタは
  // 走らせずに終了する（終了時クラッシュの回避）。
  quick_exit(exitCode);
  return exitCode;
}

}  // namespace xrift::transfer

#else

#include <string>

#include "common/log.h"
#include "app/xr_transfer_app.h"

namespace xrift::transfer {

int RunXrTransfer(const XrTransferOptions&) {
  Error("--xr-transfer は Windows のみ（Linux は将来対応）");
  return 1;
}

}  // namespace xrift::transfer

#endif
