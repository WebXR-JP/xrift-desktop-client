#include "transfer/m2_selftest.h"

#include <cstdint>
#include <cstdio>

#include "transfer/m2_blocks.h"
#include "transfer/protocol.h"
#include "transfer/render_record.h"

namespace xrift::transfer {
namespace {

// 失敗数の計数。失敗しても続行し、最後に件数で判定する（全内訳を出す）。
int g_failures = 0;

void Check(bool cond, const char* name) {
  if (cond) {
    std::printf("[m2-selftest] ok: %s\n", name);
  } else {
    std::printf("[m2-selftest] FAIL: %s\n", name);
    ++g_failures;
  }
}

EyePose Pose(float px, float py, float pz, float ox, float oy, float oz, float ow) {
  EyePose p{};
  p.position[0] = px;
  p.position[1] = py;
  p.position[2] = pz;
  p.orientation[0] = ox;
  p.orientation[1] = oy;
  p.orientation[2] = oz;
  p.orientation[3] = ow;
  return p;
}

}  // namespace

int RunM2SelfTest() {
  // 1. 配置の実行時確認（static_assert の相手側。JS との一致は protocol-test.js で行う）。
  Check(sizeof(TransferMessage) == 192, "メッセージ 192B");
  Check(offsetof(TransferMessage, renderId) == 40, "renderId オフセット 40");
  Check(offsetof(TransferMessage, spaceEpoch) == 52, "spaceEpoch オフセット 52");
  Check(offsetof(TransferMessage, leftPos) == 56, "leftPos オフセット 56");
  Check(offsetof(TransferMessage, leftFov) == 112, "leftFov オフセット 112");
  Check(offsetof(TransferMessage, rightFov) == 128, "rightFov オフセット 128");
  Check(offsetof(TransferMessage, reqWidth) == 144, "reqWidth オフセット 144");

  // 2. RenderRequest の往復。
  const EyePose left = Pose(0.1f, 1.6f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
  const EyePose right = Pose(0.164f, 1.6f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
  const EyeFov lf{0.7f, 0.7f, 0.7f, 0.7f};
  const EyeFov rf{0.7f, 0.7f, 0.7f, 0.7f};
  const TransferMessage req = MakeRenderRequest(42, 1, 7, left, right, lf, rf, 2048, 1024);
  Check(IsValidRenderRequest(req), "RenderRequest 有効");
  Check(req.leftPos[0] == 0.1f && req.rightPos[0] == 0.164f, "pose 値の保持");
  Check(req.leftFov[0] == 0.7f && req.spaceEpoch == 7, "fov/世代の保持");
  TransferMessage bad = req;
  bad.renderId = 0;
  Check(!IsValidRenderRequest(bad), "renderId 0 の要求は無効");

  // 3. 量子化の両端と範囲外の寄せ。
  Check(QuantizePos(-4.0f) == 0 && QuantizePos(4.0f) == 255, "pos 量子化の両端");
  Check(QuantizePos(-99.0f) == 0 && QuantizePos(99.0f) == 255, "pos 範囲外は端に寄せる");
  Check(QuantizeOri(-1.0f) == 0 && QuantizeOri(1.0f) == 255, "ori 量子化の両端");

  // 4. pose タグの決定性と感度。値は JS 側（m2-codec-test.js）と共有する。
  // 変更時は JS 側の期待値も同時に更新する。
  const uint16_t tagA = PoseTag(42, 1, 7, left, right);
  const uint16_t tagA2 = PoseTag(42, 1, 7, left, right);
  Check(tagA == tagA2, "pose タグの決定性");
  std::printf("[m2-selftest] tag vectors: id=42 tag=0x%04x\n", tagA);
  const EyePose leftMoved = Pose(0.2f, 1.6f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
  Check(PoseTag(42, 1, 7, leftMoved, right) != tagA, "pose 変化でタグが変わる");
  Check(PoseTag(43, 1, 7, left, right) != tagA, "ID 変化でタグが変わる");
  Check(PoseTag(42, 1, 8, left, right) != tagA, "space 世代変化でタグが変わる");
  const uint16_t tagB = PoseTag(0x123456789ABCDEF0ull, 3, 11, leftMoved, left);
  std::printf("[m2-selftest] tag vectors: id=0x123456789ABCDEF0 tag=0x%04x\n", tagB);

  // 4b. 制御ブロックの画素往復（m2_blocks.h。GPU コピー経路の CPU 側再現）。
  // JS 側（m2-codec-test.js）の符号化とセル配置が一致することの相手側である。
  {
    uint8_t pixels[kM2BlockBytes];
    const uint16_t samples[5] = {0, 1, tagA, tagB, 0xFFFF};
    bool roundtrip_ok = true;
    for (uint16_t s : samples) {
      EncodeBlock16(pixels, s);
      if (DecodeBlock16(pixels) != s) {
        roundtrip_ok = false;
      }
    }
    Check(roundtrip_ok, "制御ブロックの画素往復");
  }

  // 5. 受理の幸福経路。
  RenderLedger ledger(1, 7);
  const uint64_t t0 = 1000000u;
  const RenderRecord& rec =
      ledger.Issue(42, left, right, lf, rf, 2048, 1024, t0);
  const uint16_t id16 = static_cast<uint16_t>(rec.renderId & 0xFFFFu);
  const uint16_t tag = PoseTag(rec.renderId, rec.connEpoch, rec.spaceEpoch, rec.left, rec.right);
  const RenderRecord* out = nullptr;
  Check(ledger.Accept(id16, tag, 7, t0 + 1000, &out) == RejectReason::Accepted, "一致は受理");
  Check(out != nullptr && out->renderId == 42, "受理レコードの ID");
  // 最新 pose への付け替えをしない。返すのは画像対応のレコード自体である。
  Check(out->left.position[0] == 0.1f, "受理レコードは発行時の pose を保つ");

  // 6. 拒否系の 3 種（古い ID・旧世代・期限超過）と pose 不一致。
  Check(ledger.Accept(0x1234, tag, 7, t0 + 2000) == RejectReason::UnknownId, "未知 ID は棄却");
  const uint16_t staleTag = PoseTag(42, 1, 7, leftMoved, right);
  Check(ledger.Accept(id16, staleTag, 7, t0 + 3000) == RejectReason::PoseMismatch,
        "古い pose の埋め込みは棄却");
  Check(ledger.Accept(id16, tag, 6, t0 + 4000) == RejectReason::SpaceMismatch,
        "旧 space 世代は棄却");
  Check(ledger.Accept(id16, tag, 7, t0 + kRenderRecordMaxAgeUs + 1) == RejectReason::Expired,
        "500ms 超の画像は棄却");
  const RenderLedger::Stats& st = ledger.stats();
  Check(st.accepted == 1 && st.unknownId == 1 && st.poseMismatch == 1 && st.spaceMismatch == 1 &&
            st.expired == 1,
        "理由別カウントの内訳");
  std::printf("[m2-selftest] stats: accepted=%llu unknown=%llu poseNg=%llu spaceNg=%llu expired=%llu\n",
              static_cast<unsigned long long>(st.accepted),
              static_cast<unsigned long long>(st.unknownId),
              static_cast<unsigned long long>(st.poseMismatch),
              static_cast<unsigned long long>(st.spaceMismatch),
              static_cast<unsigned long long>(st.expired));

  // 7. 世代進行で旧レコードは失効する。
  ledger.AdvanceSpaceEpoch(8);
  Check(ledger.size() == 0, "世代進行で台帳は空になる");
  Check(ledger.Accept(id16, tag, 7, t0 + 5000) == RejectReason::SpaceMismatch,
        "旧世代の画像は進行後も棄却");

  if (g_failures > 0) {
    std::printf("[m2-selftest] FAIL %d 件\n", g_failures);
    return 1;
  }
  std::printf("[m2-selftest] all ok\n");
  return 0;
}

}  // namespace xrift::transfer
