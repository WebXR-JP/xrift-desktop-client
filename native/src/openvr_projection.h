#pragma once

#include <array>
#include <cmath>
#include <openvr.h>

// OpenVR の行優先の投影行列から OpenXR 順（左・右・上・下）の角度を復元する。
inline std::array<double, 4> OpenVrFovFromProjection(const vr::HmdMatrix44_t& p) {
  return {
      std::atan((p.m[0][2] - 1.0) / p.m[0][0]),
      std::atan((p.m[0][2] + 1.0) / p.m[0][0]),
      std::atan((p.m[1][2] + 1.0) / p.m[1][1]),
      std::atan((p.m[1][2] - 1.0) / p.m[1][1]),
  };
}
