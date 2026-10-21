#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>

#include "transfer/protocol.h"

// 描画レコード（exe が発行・保持する変更不能な要求台帳）。
// xrLocateViews の左右 pose/FOV・予測時刻・世代を renderId と結び付けて保持し、
// 画像から復号した ID/pose 要約と照合する（DESIGN §7）。
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

// 位置の量子化範囲。LOCAL 空間の頭部移動（±4m）を 8bit に写像する。
// 範囲外は端に寄せる（破棄しない。照合は両側が同じ関数を通るため一致する）。
constexpr float kPosQuantMin = -4.0f;
constexpr float kPosQuantMax = 4.0f;
// 姿勢の量子化範囲。正規化クォータニオン成分（±1）を 8bit に写像する。
constexpr float kOriQuantMin = -1.0f;
constexpr float kOriQuantMax = 1.0f;

// 0〜255 に丸める。JS 側（control-codec.ts・shim.ts）と同じ式を
// 倍精度で計算することが一致の条件である。単精度で計算すると .5 境界で 1 ずれる
// 値があり、静止 pose のシミュレータでは毎フレーム不一致になり受理 0 になる
// （実測: wantTag と復号 tag が割れ、倍精度の再計算 wantDbl と復号 tag が一致）。
// 入力は float32 由来の正確な double のため、有限値では JS と bit 一致する。
inline uint8_t QuantizeByte(float v, float lo, float hi) {
  const double t = (static_cast<double>(v) - static_cast<double>(lo)) /
                   (static_cast<double>(hi) - static_cast<double>(lo)) * 255.0;
  if (!(t >= 0.0)) {
    return 0;
  }
  if (!(t <= 255.0)) {
    return 255;
  }
  return static_cast<uint8_t>(std::floor(t + 0.5));
}

inline uint8_t QuantizePos(float v) { return QuantizeByte(v, kPosQuantMin, kPosQuantMax); }
inline uint8_t QuantizeOri(float v) { return QuantizeByte(v, kOriQuantMin, kOriQuantMax); }

// pose 要約タグ。量子化した pose + 世代 + renderId 全体を FNV-1a で畳み、下位 16bit を取る。
// 画像のブロック B（32x32・16 セル）に載る値であり、JS 側も同じバイト列で計算する。
// バイト列（計 30B）: renderId LE 8B, connEpoch LE 4B, spaceEpoch LE 4B,
// 左 pos 3B, 左 ori 4B, 右 pos 3B, 右 ori 4B。
inline uint16_t PoseTag(uint64_t renderId, uint32_t connEpoch, uint32_t spaceEpoch,
                        const EyePose& left, const EyePose& right) {
  uint8_t bytes[30];
  for (int i = 0; i < 8; ++i) {
    bytes[i] = static_cast<uint8_t>((renderId >> (i * 8)) & 0xFFu);
  }
  for (int i = 0; i < 4; ++i) {
    bytes[8 + i] = static_cast<uint8_t>((connEpoch >> (i * 8)) & 0xFFu);
    bytes[12 + i] = static_cast<uint8_t>((spaceEpoch >> (i * 8)) & 0xFFu);
  }
  for (int i = 0; i < 3; ++i) {
    bytes[16 + i] = QuantizePos(left.position[i]);
    bytes[23 + i] = QuantizePos(right.position[i]);
  }
  for (int i = 0; i < 4; ++i) {
    bytes[19 + i] = QuantizeOri(left.orientation[i]);
    bytes[26 + i] = QuantizeOri(right.orientation[i]);
  }
  uint32_t hash = 2166136261u;
  for (uint8_t b : bytes) {
    hash ^= b;
    hash *= 16777619u;
  }
  return static_cast<uint16_t>(hash & 0xFFFFu);
}

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
  // 旧世代のレコードは即座に失効させる（DESIGN §10。照合で弾く）。
  void AdvanceSpaceEpoch(uint32_t next) {
    if (next == spaceEpoch_) {
      return;
    }
    spaceEpoch_ = next;
    records_.clear();
  }

  // セッション再生成に伴い接続世代を進める。旧世代のレコードは失効させる
  // （DESIGN §6・§10。旧 ACK・旧画像を受け付けないことと連動する）。
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
