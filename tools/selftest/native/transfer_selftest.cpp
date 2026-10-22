#include "transfer_selftest.h"

#include <cstdint>
#include <cstdio>
#include <string>

#include "app/input_message.h"
#include "ipc/control_blocks.h"
#include "ipc/protocol.h"
#include "transfer/render_record.h"
#include "xr/input.h"

namespace xrift::transfer {
namespace {

// 失敗数の計数。失敗しても続行し、最後に件数で判定する（全内訳を出す）。
int g_failures = 0;

void Check(bool cond, const char* name) {
  if (cond) {
    std::printf("[transfer-selftest] ok: %s\n", name);
  } else {
    std::printf("[transfer-selftest] FAIL: %s\n", name);
    ++g_failures;
  }
}

ipc::EyePose Pose(float px, float py, float pz, float ox, float oy, float oz, float ow) {
  ipc::EyePose p{};
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

int RunTransferSelfTest() {
  // 1. 配置の実行時確認（static_assert の相手側。JS との一致は tools/test/client/protocol.test.cjs で行う）。
  Check(sizeof(ipc::TransferMessage) == 256, "メッセージ 256B");
  Check(offsetof(ipc::TransferMessage, renderId) == 40, "renderId オフセット 40");
  Check(offsetof(ipc::TransferMessage, spaceEpoch) == 52, "spaceEpoch オフセット 52");
  Check(offsetof(ipc::TransferMessage, leftPos) == 56, "leftPos オフセット 56");
  Check(offsetof(ipc::TransferMessage, leftFov) == 112, "leftFov オフセット 112");
  Check(offsetof(ipc::TransferMessage, rightFov) == 128, "rightFov オフセット 128");
  Check(offsetof(ipc::TransferMessage, reqWidth) == 144, "reqWidth オフセット 144");

  // 2. RenderRequest の往復。
  const ipc::EyePose left = Pose(0.1f, 1.6f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
  const ipc::EyePose right = Pose(0.164f, 1.6f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
  const ipc::EyeFov lf{0.7f, 0.7f, 0.7f, 0.7f};
  const ipc::EyeFov rf{0.7f, 0.7f, 0.7f, 0.7f};
  const ipc::TransferMessage req = ipc::MakeRenderRequest(42, 1, 7, left, right, lf, rf, 2048, 1024);
  Check(ipc::IsValidRenderRequest(req), "RenderRequest 有効");
  Check(req.leftPos[0] == 0.1f && req.rightPos[0] == 0.164f, "pose 値の保持");
  Check(req.leftFov[0] == 0.7f && req.spaceEpoch == 7, "fov/世代の保持");
  ipc::TransferMessage bad = req;
  bad.renderId = 0;
  Check(!ipc::IsValidRenderRequest(bad), "renderId 0 の要求は無効");

  // 3. 量子化の両端と範囲外の寄せ。
  Check(ipc::QuantizePos(-4.0f) == 0 && ipc::QuantizePos(4.0f) == 255, "pos 量子化の両端");
  Check(ipc::QuantizePos(-99.0f) == 0 && ipc::QuantizePos(99.0f) == 255, "pos 範囲外は端に寄せる");
  Check(ipc::QuantizeOri(-1.0f) == 0 && ipc::QuantizeOri(1.0f) == 255, "ori 量子化の両端");

  // 4. pose タグの決定性と感度。値は JS 側（control-codec.test.cjs）と共有する。
  // 変更時は JS 側の期待値も同時に更新する。
  const uint16_t tagA = ipc::PoseTag(42, 1, 7, left, right);
  const uint16_t tagA2 = ipc::PoseTag(42, 1, 7, left, right);
  Check(tagA == tagA2, "pose タグの決定性");
  std::printf("[transfer-selftest] tag vectors: id=42 tag=0x%04x\n", tagA);
  const ipc::EyePose leftMoved = Pose(0.2f, 1.6f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
  Check(ipc::PoseTag(42, 1, 7, leftMoved, right) != tagA, "pose 変化でタグが変わる");
  Check(ipc::PoseTag(43, 1, 7, left, right) != tagA, "ID 変化でタグが変わる");
  Check(ipc::PoseTag(42, 1, 8, left, right) != tagA, "space 世代変化でタグが変わる");
  const uint16_t tagB = ipc::PoseTag(0x123456789ABCDEF0ull, 3, 11, leftMoved, left);
  std::printf("[transfer-selftest] tag vectors: id=0x123456789ABCDEF0 tag=0x%04x\n", tagB);

  // 4b. 制御ブロックの画素往復（control_blocks.h。GPU コピー経路の CPU 側再現）。
  // JS 側（control-codec.test.cjs）の符号化とセル配置が一致することの相手側である。
  {
    uint8_t pixels[ipc::kControlBlockBytes];
    const uint16_t samples[5] = {0, 1, tagA, tagB, 0xFFFF};
    bool roundtrip_ok = true;
    for (uint16_t s : samples) {
      ipc::EncodeBlock16(pixels, s);
      if (ipc::DecodeBlock16(pixels) != s) {
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
  const uint16_t tag = ipc::PoseTag(rec.renderId, rec.connEpoch, rec.spaceEpoch, rec.left, rec.right);
  const RenderRecord* out = nullptr;
  Check(ledger.Accept(id16, tag, 7, t0 + 1000, &out) == RejectReason::Accepted, "一致は受理");
  Check(out != nullptr && out->renderId == 42, "受理レコードの ID");
  // 最新 pose への付け替えをしない。返すのは画像対応のレコード自体である。
  Check(out->left.position[0] == 0.1f, "受理レコードは発行時の pose を保つ");

  // 6. 拒否系の 3 種（古い ID・旧世代・期限超過）と pose 不一致。
  Check(ledger.Accept(0x1234, tag, 7, t0 + 2000) == RejectReason::UnknownId, "未知 ID は棄却");
  const uint16_t staleTag = ipc::PoseTag(42, 1, 7, leftMoved, right);
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
  std::printf("[transfer-selftest] stats: accepted=%llu unknown=%llu poseNg=%llu spaceNg=%llu expired=%llu\n",
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

  // 8. 接続世代の進行で旧レコードは失効する（セッション再生成時の切り替え）。
  RenderLedger conn_ledger(1, 7);
  conn_ledger.Issue(42, left, right, lf, rf, 2048, 1024, t0);
  conn_ledger.AdvanceConnEpoch(2);
  Check(conn_ledger.size() == 0, "接続世代の進行で台帳は空になる");
  Check(conn_ledger.connEpoch() == 2, "接続世代の保持");

  // 9. HapticCommand の往復と妥当性（配置は protocol.h の定義どおり）。
  {
    const ipc::TransferMessage haptic = ipc::MakeHapticCommand(1, 0.8f, 300);
    Check(ipc::IsValidHapticCommand(haptic), "HapticCommand 有効");
    Check(ipc::HapticHand(haptic) == 1 && ipc::HapticAmplitude(haptic) == 0.8f &&
              ipc::HapticDurationMs(haptic) == 300,
          "HapticCommand の値の保持");
    ipc::TransferMessage bad_hand = haptic;
    bad_hand.renderId = 2;
    Check(!ipc::IsValidHapticCommand(bad_hand), "hand 2 の指令は無効");
    ipc::TransferMessage bad_amp = haptic;
    bad_amp.leftPos[0] = 1.5f;
    Check(!ipc::IsValidHapticCommand(bad_amp), "振幅 1 超の指令は無効");
    ipc::TransferMessage bad_dur = haptic;
    bad_dur.connEpoch = 0;
    Check(!ipc::IsValidHapticCommand(bad_dur), "持続 0 の指令は無効");
  }

  // 10. profile 識別子と量子化（JS 側 tools/test/client/protocol.test.cjs と同じ表・同じ式）。
  Check(ipc::ProfileIdFromName("") == 0, "空 profile は未取得");
  Check(ipc::ProfileIdFromName("/interaction_profiles/bytedance/pico4_controller") == 6,
        "PICO 4 の識別子");
  Check(ipc::ProfileIdFromName("/interaction_profiles/oculus/touch_controller") == 2,
        "oculus の識別子");
  Check(ipc::ProfileIdFromName("/interaction_profiles/unknown/future_controller") == 255,
        "未知 profile は 255");
  Check(ipc::QuantizeUnitFloat(0.0f) == 0 && ipc::QuantizeUnitFloat(1.0f) == 255, "アナログ量子化の両端");
  Check(ipc::QuantizeStick(-1.0f) == static_cast<uint8_t>(static_cast<int8_t>(-127)) &&
            ipc::QuantizeStick(1.0f) == 127,
        "スティック量子化の両端");
  Check(sizeof(ipc::TransferMessage) == 256, "拡張後もメッセージ 256B");

  // 11. 入力計測: 押下エッジの検出と 1 秒ラッチ（HMD 無しで形式を確認する）。
  // 実機の取りこぼし（短い押下・primary/menu の未記録）の切り分け用である。
  {
    // 合成スナップショットの雛形。ボタンは非押下・Inactive、pose は無効から始める。
    const auto blank = []() {
      xr::InputSnapshot s{};
      s.frameNo = 1;
      return s;
    };
    // 左 primary を Active 押下にする。
    const auto press_primary = [](xr::InputSnapshot s) {
      s.hands[0].primary.pressed = true;
      s.hands[0].primary.state = xr::FieldState::Active;
      return s;
    };
    const xr::InputSnapshot s0 = blank();
    const xr::InputSnapshot s1 = press_primary(s0);
    Check(xr::InputManager::DescribeEdges(s0, s0).empty(), "変化無しはエッジ無し");
    const std::string edge = xr::InputManager::DescribeEdges(s0, s1);
    Check(edge.find("L.primary press") != std::string::npos, "primary 押下のエッジ");
    const std::string release = xr::InputManager::DescribeEdges(s1, s0);
    Check(release.find("L.primary release") != std::string::npos, "primary 解放のエッジ");
    // menu 等の他ボタンも同じ割付けで出る（実機で出なかったボタンの確認用）。
    xr::InputSnapshot s2 = s0;
    s2.hands[1].menu.pressed = true;
    s2.hands[1].menu.state = xr::FieldState::Active;
    Check(xr::InputManager::DescribeEdges(s0, s2).find("R.menu press") != std::string::npos,
          "右 menu 押下のエッジ");
    // Inactive のままの押下は bit が立たないためエッジにしない（InputState と同じ基準）。
    xr::InputSnapshot s3 = s0;
    s3.hands[0].primary.pressed = true;
    s3.hands[0].primary.state = xr::FieldState::Inactive;
    Check(xr::InputManager::DescribeEdges(s0, s3).empty(), "Inactive の押下はエッジにしない");
    // trigger の 0/正の出入り。
    xr::InputSnapshot s4 = s0;
    s4.hands[0].trigger = 0.45f;
    s4.hands[0].triggerState = xr::FieldState::Active;
    Check(xr::InputManager::DescribeEdges(s0, s4).find("L.trigger off->on") != std::string::npos,
          "trigger 立ち上がりのエッジ");
    Check(xr::InputManager::DescribeEdges(s4, s0).find("L.trigger on->off") != std::string::npos,
          "trigger 立ち下がりのエッジ");
    // stick の絶対値 0.2 超の出入り。
    xr::InputSnapshot s5 = s0;
    s5.hands[1].thumbstick[0] = 0.3f;
    s5.hands[1].thumbstick[1] = -0.1f;
    s5.hands[1].thumbstickState = xr::FieldState::Active;
    Check(xr::InputManager::DescribeEdges(s0, s5).find("R.stick idle->touch") !=
              std::string::npos,
          "stick 有効域入りのエッジ");
    // 0.2 以下の倒しはエッジにしない。
    xr::InputSnapshot s6 = s0;
    s6.hands[1].thumbstick[0] = 0.1f;
    Check(xr::InputManager::DescribeEdges(s0, s6).empty(), "stick 微倒しはエッジにしない");
    // pose 有効/無効の変化。
    xr::InputSnapshot s7 = s0;
    s7.hands[0].gripValid = true;
    s7.hands[0].connected = true;
    const std::string pose_edge = xr::InputManager::DescribeEdges(s0, s7);
    Check(pose_edge.find("L.grip invalid->valid") != std::string::npos, "grip 有効化のエッジ");
    Check(pose_edge.find("L.connected no->yes") != std::string::npos, "接続変化のエッジ");
  }

  // 12. 入力計測: 押下ラッチの bit 割付けと InputState への格納（予約先頭 2B）。
  {
    xr::InputSnapshot s{};
    s.frameNo = 77;
    s.hands[0].primary.pressed = true;
    s.hands[0].primary.state = xr::FieldState::Active;
    s.hands[1].secondary.pressed = true;
    s.hands[1].secondary.state = xr::FieldState::Active;
    const uint8_t latch0 = xr::InputManager::PressedBits(s.hands[0]);
    const uint8_t latch1 = xr::InputManager::PressedBits(s.hands[1]);
    Check(latch0 == ipc::kBtnPrimary && latch1 == ipc::kBtnSecondary, "ラッチ bit は buttons 欄と同じ割付け");
    Check(xr::InputManager::FormatLatch(latch0) == "primary", "ラッチの人名（左）");
    Check(xr::InputManager::FormatLatch(0) == "-", "空ラッチは -");
    const ipc::TransferMessage with_latch = app::MakeInputStateMessage(s, 2, 7, latch0, latch1);
    Check(with_latch.reserved[0] == ipc::kBtnPrimary && with_latch.reserved[1] == ipc::kBtnSecondary,
          "ラッチは予約先頭 2B（左・右）");
    // ラッチ無し（既定 0）の配置は従来どおりであり、buttons 欄は変わらない。
    const ipc::TransferMessage plain = app::MakeInputStateMessage(s, 2, 7);
    const uint8_t* base = reinterpret_cast<const uint8_t*>(&plain);
    Check(plain.reserved[0] == 0 && plain.reserved[1] == 0, "既定ラッチは 0");
    Check(base[ipc::kInputHand0 + ipc::kInputHandButtons] == ipc::kBtnPrimary, "buttons 欄は従来どおり");
  }

  // 13b. TTL 設定の上書き（--record-ttl-ms の実行値）。既定 500ms は変えない。
  {
    RenderLedger custom(1, 7, 2000000u);
    const RenderRecord& c =
        custom.Issue(43, left, right, lf, rf, 2048, 1024, t0);
    const uint16_t cid16 = static_cast<uint16_t>(c.renderId & 0xFFFFu);
    const uint16_t ctag = ipc::PoseTag(c.renderId, c.connEpoch, c.spaceEpoch, c.left, c.right);
    Check(custom.maxAgeUs() == 2000000u, "TTL 設定値の保持");
    Check(custom.Accept(cid16, ctag, 7, t0 + 600000) == RejectReason::Accepted,
          "TTL 2s では 600ms の画像も受理");
    Check(custom.Accept(cid16, ctag, 7, t0 + 2000001) == RejectReason::Expired,
          "TTL 2s 超の画像は棄却");
  }
  // 13. InputState のコントローラ pose（左右の grip/aim）の往復。
  // 配置は protocol.h の kInput*（C++ と JS の一致は tools/test/client/protocol.test.cjs で行う）。
  // 有効性は手 flags が表し、無効時の値は 0 のままである。
  {
    xr::InputSnapshot s{};
    s.frameNo = 78;
    s.hands[0].connected = true;
    s.hands[0].gripValid = true;
    s.hands[0].grip.position = {0.11f, 1.21f, -0.31f};
    s.hands[0].grip.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    s.hands[0].aimValid = true;
    s.hands[0].aim.position = {0.12f, 1.22f, -0.32f};
    s.hands[0].aim.orientation = {0.0f, 0.7071f, 0.0f, 0.7071f};
    s.hands[1].connected = true;
    s.hands[1].gripValid = false;
    s.hands[1].aimValid = true;
    s.hands[1].aim.position = {-0.13f, 1.23f, -0.33f};
    s.hands[1].aim.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    const ipc::TransferMessage pose_msg = app::MakeInputStateMessage(s, 2, 7);
    const uint8_t* pbase = reinterpret_cast<const uint8_t*>(&pose_msg);
    Check((pbase[ipc::kInputHand0 + ipc::kInputHandFlags] & 0b110) == 0b110, "左の grip/aim 有効 flags");
    Check((pbase[ipc::kInputHand0 + ipc::kInputHandStride + ipc::kInputHandFlags] & 0b110) == 0b100,
          "右は aim のみ有効 flags");
    const ipc::ControllerPose lg = ipc::ReadInputPose(pose_msg, ipc::kInputGripLeft);
    Check(lg.position[0] == 0.11f && lg.position[1] == 1.21f && lg.position[2] == -0.31f &&
              lg.orientation[3] == 1.0f,
          "左 grip pose の往復");
    const ipc::ControllerPose la = ipc::ReadInputPose(pose_msg, ipc::kInputAimLeft);
    Check(la.position[0] == 0.12f && la.orientation[1] == 0.7071f, "左 aim pose の往復");
    const ipc::ControllerPose ra = ipc::ReadInputPose(pose_msg, ipc::kInputAimRight);
    Check(ra.position[0] == -0.13f && ra.position[1] == 1.23f && ra.orientation[3] == 1.0f,
          "右 aim pose の往復");
    // 無効な右 grip の値は 0 のまま（受信側は flags を見て読まない）。
    const ipc::ControllerPose rg = ipc::ReadInputPose(pose_msg, ipc::kInputGripRight);
    Check(rg.position[0] == 0.0f && rg.position[1] == 0.0f && rg.position[2] == 0.0f &&
              rg.orientation[0] == 0.0f && rg.orientation[3] == 0.0f,
          "無効な右 grip pose は 0 のまま");
    // pose 以外の予約域は 0（144〜159 の req・ラッチ外、216〜255 の拡張末尾）。
    const uint8_t* raw = pbase;
    bool tail_zero = true;
    for (size_t i = 144; i < 152; ++i) {
      tail_zero = tail_zero && (raw[i] == 0);
    }
    for (size_t i = 154; i < 160; ++i) {
      tail_zero = tail_zero && (raw[i] == 0);
    }
    for (size_t i = 216; i < 256; ++i) {
      tail_zero = tail_zero && (raw[i] == 0);
    }
    Check(tail_zero, "pose 以外の予約域は 0");
    // 未接続の両手は flags=0 で pose も 0（0 埋めと区別するため flags を見る）。
    xr::InputSnapshot empty{};
    empty.frameNo = 79;
    const ipc::TransferMessage empty_msg = app::MakeInputStateMessage(empty, 2, 7);
    const uint8_t* ebase = reinterpret_cast<const uint8_t*>(&empty_msg);
    Check(ebase[ipc::kInputHand0 + ipc::kInputHandFlags] == 0, "未接続の左 flags は 0");
    const ipc::ControllerPose eg = ipc::ReadInputPose(empty_msg, ipc::kInputGripLeft);
    Check(eg.position[0] == 0.0f && eg.orientation[3] == 0.0f, "未接続の左 grip pose は 0");
  }

  // 14. Offer の paint タグ（QPC µs）の往復。reserved 先頭 8B であり、版は上げない。
  {
    Check(ipc::kOfferPaintQpcOffset == 152, "paint タグのオフセット 152");
    Check(ipc::kOfferPaintQpcOffset == offsetof(ipc::TransferMessage, reserved),
          "paint タグは reserved の先頭");
    ipc::TransferMessage offer{};
    offer.magic = ipc::kMagic;
    offer.version = ipc::kVersion;
    offer.type = ipc::kTypeOffer;
    ipc::SetOfferPaintQpcUs(offer, 0);
    Check(ipc::OfferPaintQpcUs(offer) == 0, "タグ 0 は欠測として往復");
    ipc::SetOfferPaintQpcUs(offer, 0x1122334455667788ull);
    Check(ipc::OfferPaintQpcUs(offer) == 0x1122334455667788ull, "タグの往復");
    // タグ以外の既存欄は変わらない。
    Check(offer.magic == ipc::kMagic && offer.version == ipc::kVersion && offer.type == ipc::kTypeOffer,
          "タグ設定で既存欄は変わらない");
    const uint8_t* obase = reinterpret_cast<const uint8_t*>(&offer);
    bool tail_zero = true;
    for (size_t i = 160; i < 256; ++i) {
      tail_zero = tail_zero && (obase[i] == 0);
    }
    Check(tail_zero, "タグ外の予約域は 0 のまま");
    // 版は上げない（旧受信側は無視、旧送信側の 0 は欠測）。
    Check(ipc::kVersion == 3, "paint タグ追加で版は上げない");
  }

  if (g_failures > 0) {
    std::printf("[transfer-selftest] FAIL %d 件\n", g_failures);
    return 1;
  }
  std::printf("[transfer-selftest] all ok\n");
  return 0;
}

}  // namespace xrift::transfer
