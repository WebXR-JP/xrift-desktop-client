#pragma once

#include <cstddef>
#include <cstdint>

// M2 制御ブロック（32x32・4x4 セル・16bit 値）の符号化・復号。
// 画像配置は tools/pages/xr-smoke.html の刻み（ブロック A=(64,64)、B=(128,64)、
// マーカーは左 (160,8)・右（右半分起点+160,8）、各 32x32）が正典である。
// BGRA バイト順・白黒二値・中心画素判定。JS 側（client/src/m2codec.ts の
// encodeBlock16/decodeBlock16、tools/pages/xr-smoke.html の刻み）と一致させること。

namespace xrift::transfer {

constexpr uint32_t kM2BlockPixels = 32;
constexpr uint32_t kM2BlockCell = 4;
constexpr size_t kM2BlockBytes = kM2BlockPixels * kM2BlockPixels * 4;

// 16bit 値を 32x32 BGRA バッファへ書く。呼び出し側が kM2BlockBytes を用意すること。
inline void EncodeBlock16(uint8_t* rgba, uint16_t value) {
  for (uint32_t b = 0; b < 16; ++b) {
    const bool on = ((value >> b) & 1u) != 0u;
    const uint32_t x0 = (b % 8u) * kM2BlockCell;
    const uint32_t y0 = (b / 8u) * kM2BlockCell;
    const uint8_t c = on ? 255 : 0;
    for (uint32_t dy = 0; dy < kM2BlockCell; ++dy) {
      for (uint32_t dx = 0; dx < kM2BlockCell; ++dx) {
        uint8_t* p = rgba + (static_cast<size_t>(y0 + dy) * kM2BlockPixels + (x0 + dx)) * 4;
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
// M1 の GPU 検査の責務であり、ここでは ID 照合に足る復号だけを行う）。
inline uint16_t DecodeBlock16(const uint8_t* rgba) {
  uint16_t v = 0;
  for (uint32_t b = 0; b < 16; ++b) {
    const uint32_t cx = (b % 8u) * kM2BlockCell + 2;
    const uint32_t cy = (b / 8u) * kM2BlockCell + 2;
    const uint8_t* p = rgba + (static_cast<size_t>(cy) * kM2BlockPixels + cx) * 4;
    const int avg = (static_cast<int>(p[0]) + static_cast<int>(p[1]) + static_cast<int>(p[2])) / 3;
    if (avg > 128) {
      v |= static_cast<uint16_t>(1u << b);
    }
  }
  return v;
}

}  // namespace xrift::transfer
