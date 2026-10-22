#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "ipc/protocol.h"

// 制御ブロック（32x32・4x4 セル・16bit 値）の符号化・復号。
// 画像配置は tools/pages/xr-smoke.html の刻み（ブロック A=(64,64)、B=(128,64)、
// マーカーは左 (160,8)・右（右半分起点+160,8）、各 32x32）が正典である。
// BGRA バイト順・白黒二値・中心画素判定。JS 側（client/src/control-codec.ts の
// encodeBlock16/decodeBlock16、tools/pages/xr-smoke.html の刻み）と一致させること。

namespace xrift::ipc {

constexpr uint32_t kControlBlockPixels = 32;
constexpr uint32_t kControlBlockCell = 4;
constexpr size_t kControlBlockBytes = kControlBlockPixels * kControlBlockPixels * 4;

// 画像配置（tools/pages/xr-smoke.html の刻みが正典）。
// 制御ブロック A（renderId 下位 16bit）の左上 x。
constexpr uint32_t kControlBlockOrgAX = 64;
// 制御ブロック B（pose タグ 16bit）の左上 x。
constexpr uint32_t kControlBlockOrgBX = 128;
// 両ブロックの左上 y。
constexpr uint32_t kControlBlockOrgY = 64;
// 眼マーカーの一辺。
constexpr uint32_t kMarkerSize = 32;
// 左マーカー（マゼンタ）の左上 x（アトラス座標）。
constexpr uint32_t kMarkerLeftX = 160;
// 右マーカー（シアン）の左上 x（右半分起点から）。
constexpr uint32_t kMarkerRightDx = 160;
// 両マーカーの左上 y（予約ブロックと重ねない）。
constexpr uint32_t kMarkerY = 8;

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

// 16bit 値を 32x32 BGRA バッファへ書く。呼び出し側が kControlBlockBytes を用意すること。
inline void EncodeBlock16(uint8_t* rgba, uint16_t value) {
  for (uint32_t b = 0; b < 16; ++b) {
    const bool on = ((value >> b) & 1u) != 0u;
    const uint32_t x0 = (b % 8u) * kControlBlockCell;
    const uint32_t y0 = (b / 8u) * kControlBlockCell;
    const uint8_t c = on ? 255 : 0;
    for (uint32_t dy = 0; dy < kControlBlockCell; ++dy) {
      for (uint32_t dx = 0; dx < kControlBlockCell; ++dx) {
        uint8_t* p = rgba + (static_cast<size_t>(y0 + dy) * kControlBlockPixels + (x0 + dx)) * 4;
        p[0] = c;
        p[1] = c;
        p[2] = c;
        p[3] = 255;
      }
    }
  }
}

// 32x32 BGRA バッファから 16bit 値を復号する。各セルの中心画素だけを見る
// （ティアリング等の中間値は二値化の境界で吸収する。セル単色の厳密検査は
// 全画面 GPU 検査の責務であり、ここでは ID 照合に足る復号だけを行う）。
inline uint16_t DecodeBlock16(const uint8_t* rgba) {
  uint16_t v = 0;
  for (uint32_t b = 0; b < 16; ++b) {
    const uint32_t cx = (b % 8u) * kControlBlockCell + 2;
    const uint32_t cy = (b / 8u) * kControlBlockCell + 2;
    const uint8_t* p = rgba + (static_cast<size_t>(cy) * kControlBlockPixels + cx) * 4;
    const int avg = (static_cast<int>(p[0]) + static_cast<int>(p[1]) + static_cast<int>(p[2])) / 3;
    if (avg > 128) {
      v |= static_cast<uint16_t>(1u << b);
    }
  }
  return v;
}

}  // namespace xrift::ipc
