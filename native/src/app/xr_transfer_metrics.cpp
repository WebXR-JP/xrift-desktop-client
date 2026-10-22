#include "app/xr_transfer_metrics.h"

#include <cstdio>

#include "common/log.h"

namespace xrift::app {

void XrStats::Print(const transfer::RenderLedger::Stats& ledger, const xr::InputStats& input) const {
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

void LatencyAgg::Reset() { *this = LatencyAgg{}; }

double UsToMs(uint64_t us) { return static_cast<double>(us) / 1000.0; }

void PrintLatencyLine(const char* prefix, const LatencyAgg& agg) {
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

}  // namespace xrift::app
