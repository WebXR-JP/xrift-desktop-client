#pragma once

#include <cstddef>
#include <cstdint>

#include "transfer/protocol.h"
#include "xr/input.h"

// M4 の入力スナップショットを 192B の InputState（exe → main）に畳む。
// pose は載せない（exe 側のログで確認する。線路上は操作の要約のみ）。
// 「0」と「未対応」の区別は active/supported の 2 段で行う（protocol.h の欄定義を参照）。
// 未接続の手は flags=0 のまま全欄 0 で送る（0 埋めと区別するため flags を見る）。
// latch0/latch1 は「その 1 秒間に押されたボタンの集合（ラッチ）」であり、
// buttons 欄と同じ bit 割付けで予約域の先頭 2B（offset 152,153）に載せる。
// 短い押下を取りこぼさないための計測用であり、旧復号器は予約域を読まないため
// 読み飛ばす（版は上げない）。製品経路の buttons 欄の配置は変えない。

namespace xrift::transfer {

inline TransferMessage MakeInputStateMessage(const xr::InputSnapshot& snapshot, uint32_t connEpoch,
                                             uint32_t spaceEpoch, uint8_t latch0 = 0,
                                             uint8_t latch1 = 0) {
  TransferMessage m{};
  m.magic = kMagic;
  m.version = kVersion;
  m.type = kTypeInputState;
  m.renderId = snapshot.frameNo;
  m.connEpoch = connEpoch;
  m.spaceEpoch = spaceEpoch;
  uint8_t* base = reinterpret_cast<uint8_t*>(&m);
  for (int i = 0; i < 2; ++i) {
    const xr::HandInput& hand = snapshot.hands[i];
    uint8_t* h = base + kInputHand0 + static_cast<size_t>(i) * kInputHandStride;
    uint8_t flags = 0;
    if (hand.connected) {
      flags |= 1u << 0;
    }
    if (hand.gripValid) {
      flags |= 1u << 1;
    }
    if (hand.aimValid) {
      flags |= 1u << 2;
    }
    h[kInputHandFlags] = flags;
    h[kInputHandProfile] = ProfileIdFromName(hand.profile.c_str());
    const auto active_value = [](xr::FieldState s) { return s == xr::FieldState::Active ? 1 : 0; };
    const auto supported_value = [](xr::FieldState s) {
      return s == xr::FieldState::Unsupported ? 0 : 1;
    };
    h[kInputHandTrigger] = QuantizeUnitFloat(hand.trigger);
    h[kInputHandSqueeze] = QuantizeUnitFloat(hand.squeeze);
    h[kInputHandStickX] = QuantizeStick(hand.thumbstick[0]);
    h[kInputHandStickY] = QuantizeStick(hand.thumbstick[1]);
    h[kInputHandPadX] = QuantizeStick(hand.trackpad[0]);
    h[kInputHandPadY] = QuantizeStick(hand.trackpad[1]);
    uint8_t buttons = 0;
    uint8_t btn_active = 0;
    uint8_t btn_supported = 0;
    const xr::InputButton* btns[5] = {&hand.thumbstickClick, &hand.trackpadClick, &hand.primary,
                                      &hand.secondary, &hand.menu};
    for (int b = 0; b < 5; ++b) {
      if (btns[b]->pressed && btns[b]->state == xr::FieldState::Active) {
        buttons |= static_cast<uint8_t>(1u << b);
      }
      if (btns[b]->state == xr::FieldState::Active) {
        btn_active |= static_cast<uint8_t>(1u << b);
      }
      if (btns[b]->state != xr::FieldState::Unsupported) {
        btn_supported |= static_cast<uint8_t>(1u << b);
      }
    }
    h[kInputHandButtons] = buttons;
    h[kInputHandBtnActive] = btn_active;
    h[kInputHandFloatActive] = static_cast<uint8_t>((active_value(hand.triggerState) << 0) |
                                                    (active_value(hand.squeezeState) << 1));
    h[kInputHandVecActive] = static_cast<uint8_t>((active_value(hand.thumbstickState) << 0) |
                                                   (active_value(hand.trackpadState) << 1));
    h[kInputHandBtnSupported] = btn_supported;
    h[kInputHandAnalogSupported] =
        static_cast<uint8_t>((supported_value(hand.triggerState) << 0) |
                             (supported_value(hand.squeezeState) << 1) |
                             (supported_value(hand.thumbstickState) << 2) |
                             (supported_value(hand.trackpadState) << 3));
  }
  // 押下ラッチ（計測用）。buttons 欄と同じ bit 割付けであり、旧復号器は読まない。
  m.reserved[0] = latch0;
  m.reserved[1] = latch1;
  return m;
}

}  // namespace xrift::transfer
