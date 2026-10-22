#pragma once

#include <chrono>
#include <cstdint>
#include <deque>

#include "ipc/control_blocks.h"

// 描画レコード（exe が発行・保持する変更不能な要求台帳）。
// xrLocateViews の左右 pose/FOV・予測時刻・世代を renderId と結び付けて保持し、
// 画像から復号した ID/pose 要約と照合する（docs/architecture.md「経路」）。
// XR/Vulkan に依存しない値層であり、--selftest で HMD 無しに検証できる。

namespace xrift::transfer {

// 画像年齢の上限の既定値。受理から 500ms を超えたレコードは失効として棄却する。
// 実行時は --record-ttl-ms で上書きできる（RenderLedger の保持値が正）。
// 実測: 本番サイトで 350〜500ms の遅延のため、app 経由の通常運用は暫定 2s で渡す。
// 既定値自体は 500ms に保つ（変更時は README と selftest を更新する）。
constexpr uint64_t kRenderRecordMaxAgeUs = 500000u;
// 保持レコード数の上限。超えた古い分は失効扱いで捨てる（VRAM ではなく台帳の上限）。
// 90Hz・2s で 180 件のため余裕を見た 256 である（既定 500ms では 45 件）。
constexpr size_t kRenderRecordCapacity = 256;

// 棄却理由。統計の内訳用であり、線路上には出さない。
enum class RejectReason : uint8_t {
  Accepted = 0,
  UnknownId = 1,      // 保持レコードに存在しない renderId。
  PoseMismatch = 2,   // pose 要約タグが不一致（古い pose の埋め込み等）。
  SpaceMismatch = 3,  // spaceEpoch が不一致（旧世代の画像）。
  Expired = 4,        // 受理時点で年齢上限（500ms）超過。
};

inline const char* RejectReasonName(RejectReason r) {
  switch (r) {
    case RejectReason::Accepted:
      return "受理";
    case RejectReason::UnknownId:
      return "不明ID";
    case RejectReason::PoseMismatch:
      return "pose不一致";
    case RejectReason::SpaceMismatch:
      return "space世代不一致";
    case RejectReason::Expired:
      return "失効";
  }
  return "不明";
}

// 1 件の描画レコード。発行後に書き換えない（const 扱い。台帳側が値で保持する）。
struct RenderRecord {
  uint64_t renderId = 0;
  uint32_t connEpoch = 0;
  uint32_t spaceEpoch = 0;
  EyePose left{};
  EyePose right{};
  EyeFov leftFov{};
  EyeFov rightFov{};
  uint32_t reqWidth = 0;
  uint32_t reqHeight = 0;
  // 発行時刻（steady_clock の us）。年齢判定の起点である。
  uint64_t issuedUs = 0;
};

// 描画レコードの台帳。発行・照合・失効を直列化する（単一スレッド所有を想定する）。
class RenderLedger {
 public:
  struct Stats {
    uint64_t issued = 0;
    uint64_t accepted = 0;
    uint64_t unknownId = 0;
    uint64_t poseMismatch = 0;
    uint64_t spaceMismatch = 0;
    uint64_t expired = 0;
    uint64_t evicted = 0;  // 容量超過で捨てた件数。
  };

  RenderLedger(uint32_t connEpoch, uint32_t spaceEpoch,
               uint64_t maxAgeUs = kRenderRecordMaxAgeUs)
      : connEpoch_(connEpoch), spaceEpoch_(spaceEpoch), maxAgeUs_(maxAgeUs) {}

  uint32_t connEpoch() const { return connEpoch_; }
  uint32_t spaceEpoch() const { return spaceEpoch_; }
  // 失効判定に使う画像年齢の上限（us）。--record-ttl-ms の実行値である。
  uint64_t maxAgeUs() const { return maxAgeUs_; }

  // REFERENCE_SPACE_CHANGE_PENDING の適用時刻に従い世代を進める。
  // 旧世代のレコードは即座に失効させる（docs/architecture.md「space とセッション復帰」。照合で弾く）。
  void AdvanceSpaceEpoch(uint32_t next) {
    if (next == spaceEpoch_) {
      return;
    }
    spaceEpoch_ = next;
    records_.clear();
  }

  // セッション再生成に伴い接続世代を進める。旧世代のレコードは失効させる
  // （docs/architecture.md「借用の所有権と release 遅延」「space とセッション復帰」。旧 ACK・旧画像を受け付けないことと連動する）。
  void AdvanceConnEpoch(uint32_t next) {
    if (next == connEpoch_) {
      return;
    }
    connEpoch_ = next;
    records_.clear();
  }

  // レコードを発行して保持する。renderId 0 は使わない（画像の未初期値と区別する）。
  const RenderRecord& Issue(uint64_t renderId, const EyePose& left, const EyePose& right,
                            const EyeFov& leftFov, const EyeFov& rightFov, uint32_t reqWidth,
                            uint32_t reqHeight, uint64_t nowUs) {
    if (renderId == 0) {
      renderId = 1;
    }
    if (records_.size() >= kRenderRecordCapacity) {
      records_.pop_front();
      ++stats_.evicted;
    }
    RenderRecord r{};
    r.renderId = renderId;
    r.connEpoch = connEpoch_;
    r.spaceEpoch = spaceEpoch_;
    r.left = left;
    r.right = right;
    r.leftFov = leftFov;
    r.rightFov = rightFov;
    r.reqWidth = reqWidth;
    r.reqHeight = reqHeight;
    r.issuedUs = nowUs;
    records_.push_back(r);
    ++stats_.issued;
    return records_.back();
  }

  // 画像から復号した (renderId16, poseTag, spaceEpoch) を照合する。
  // 画像は下位 16bit しか持たないため、保持側も下位 16bit で探し、完全一致は
  // poseTag の再計算で担保する（衝突時は pose 不一致として棄却される）。
  // out が非 null なら一致レコードを指す（失敗時は触らない）。
  // 最新 pose への付け替えはしない。返すのは画像に対応するレコード自体である。
  RejectReason Accept(uint16_t renderId16, uint16_t poseTag, uint32_t spaceEpoch,
                      uint64_t nowUs, const RenderRecord** out = nullptr) {
    if (spaceEpoch != spaceEpoch_) {
      ++stats_.spaceMismatch;
      return RejectReason::SpaceMismatch;
    }
    for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
      if (static_cast<uint16_t>(it->renderId & 0xFFFFu) != renderId16) {
        continue;
      }
      if (nowUs - it->issuedUs > maxAgeUs_) {
        ++stats_.expired;
        return RejectReason::Expired;
      }
      const uint16_t want = PoseTag(it->renderId, it->connEpoch, it->spaceEpoch, it->left,
                                    it->right);
      if (want != poseTag) {
        ++stats_.poseMismatch;
        return RejectReason::PoseMismatch;
      }
      ++stats_.accepted;
      if (out != nullptr) {
        // reverse_iterator の base は次を指すため 1 つ戻す。
        auto fwd = it.base();
        --fwd;
        *out = &(*fwd);
      }
      return RejectReason::Accepted;
    }
    ++stats_.unknownId;
    return RejectReason::UnknownId;
  }

  // 年限切れの掃除。毎フレーム呼ぶことを想定する（期限超過の返却ではなく台帳の掃除）。
  void SweepExpired(uint64_t nowUs) {
    while (!records_.empty() && nowUs - records_.front().issuedUs > maxAgeUs_) {
      records_.pop_front();
    }
  }

  size_t size() const { return records_.size(); }
  const Stats& stats() const { return stats_; }

  // pose 不一致の診断用に、下位 16bit が一致する最新のレコードを探す。
  // 見つかれば真を返し、out に該当レコード（次に台帳へ触れるまで有効）、
  // wantTag に再計算した期待タグを出す。見つからなければ偽（out・wantTag は触らない）。
  // Accept と違い年齢・世代の判定は行わない（不一致の原因切り分け用である）。
  bool LookupById16(uint16_t renderId16, const RenderRecord** out,
                    uint16_t* wantTag) const {
    for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
      if (static_cast<uint16_t>(it->renderId & 0xFFFFu) != renderId16) {
        continue;
      }
      if (wantTag != nullptr) {
        *wantTag = PoseTag(it->renderId, it->connEpoch, it->spaceEpoch, it->left, it->right);
      }
      if (out != nullptr) {
        auto fwd = it.base();
        --fwd;
        *out = &(*fwd);
      }
      return true;
    }
    return false;
  }

  // 診断用に、保持中の最新 renderId を返す。空なら 0 である。
  uint64_t latestRenderId() const { return records_.empty() ? 0 : records_.back().renderId; }

 private:
  uint32_t connEpoch_;
  uint32_t spaceEpoch_;
  // 失効判定の上限（us）。既定は kRenderRecordMaxAgeUs（500ms）。
  uint64_t maxAgeUs_;
  std::deque<RenderRecord> records_;
  Stats stats_{};
};

inline uint64_t SteadyNowUs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()
                                                                .time_since_epoch())
          .count());
}

}  // namespace xrift::transfer
