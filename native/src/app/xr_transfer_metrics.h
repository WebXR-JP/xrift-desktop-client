#pragma once

#include <cstdint>

#include "transfer/render_record.h"
#include "xr/input.h"

// 転送ループの計測集約。提出・ACK・画像選択には触らず観測だけ行う。
// xr_transfer_app.cpp から抽出した製品経路固有の定義であり、文面・集計式は変えない。

namespace xrift::transfer {

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

  void Print(const RenderLedger::Stats& ledger, const xr::InputStats& input) const;
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
  void Reset();
};

// ms 1 桁の表示用。us を ms に直す（表示だけであり、集計は us のまま）。
double UsToMs(uint64_t us);

// 集計 1 件分の [XR計測] 行を出す。窓と累計で共用する（prefix は空または「累計 」）。
// B の内訳平均は B 標本があるときだけ意味を持ち、無いときは 0.0 を出す。
void PrintLatencyLine(const char* prefix, const LatencyAgg& agg);

}  // namespace xrift::transfer
