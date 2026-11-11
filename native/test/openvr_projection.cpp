#include "openvr_projection.h"

#include <cstdio>

int main() {
  // 非対称な視錐台。期待する傾きは左 -1.5、右 +0.5、上 +0.5、下 -1.5。
  vr::HmdMatrix44_t projection{};
  projection.m[0][0] = 1.0f;
  projection.m[0][2] = -0.5f;
  projection.m[1][1] = 1.0f;
  projection.m[1][2] = -0.5f;
  projection.m[3][2] = -1.0f;
  const auto fov = OpenVrFovFromProjection(projection);
  const double expected[] = {-1.5, 0.5, 0.5, -1.5};
  for (int i = 0; i < 4; ++i) {
    const double tangent = std::tan(fov[i]);
    if (!(std::abs(tangent - expected[i]) < 1e-12)) {
      std::fprintf(stderr, "FOV edge %d: got %g, expected %g\n", i, tangent, expected[i]);
      return 1;
    }
  }
  return 0;
}
