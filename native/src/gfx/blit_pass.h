#pragma once

#include <cstdint>
#include <vector>

#include "common/xr_vulkan.h"

// 受理画像の提出パスを担う層（gpu 側の blit と左右分離の確認専用）。
// acquire → blit → マーカー確認 → release を 1 眼単位で行う。
// マーカーの座標と期待色は ipc の配置に依存させず、呼び出し側が値で渡す。

namespace xrift::gfx {

class VulkanContext;
class Swapchains;

// 1 画素の期待色。各成分の許容範囲（含む）で判定する。
struct MarkerColor {
  uint8_t minR, maxR, minG, maxG, minB, maxB;
};

// 画像内のマーカー位置（中心画素を読む）と期待色。
struct MarkerSpec {
  uint32_t x = 0;  // 左眼はアトラス座標、右眼は右半分起点からの x
  uint32_t y = 0;
  uint32_t size = 0;
  MarkerColor color{};
};

struct MarkerLayout {
  MarkerSpec left;
  MarkerSpec right;
};

class BlitPass {
 public:
  BlitPass(const VulkanContext& vk, const Swapchains& swapchains,
           const MarkerLayout& markers);

  struct Result {
    uint64_t blitUs = 0;
    bool markerOk = false;  // 左眼=マゼンタ、右眼=シアンの確認結果。
  };

  // 受理画像の片半分を各眼へ blit する。srcHalf: 0=左半分、1=右半分。
  // 色の扱い: 同一形式間の blit は複写・拡大であり、色空間の変換は行わない。
  // swapchain が sRGB 形式の場合、二重符号化の疑いが残る（docs/architecture.md「既知の挙動」の未決事項。
  // 左右分離と pose 対応を優先し、色の最終確認は持ち越す）。
  // blit 後に提出画像からマーカー 1 画素だけを読む（画像全体は読まない）。
  // 失敗時は例外にする（呼び出し側は layerCount=0 で終える）。
  Result Blit(uint32_t eye, vk::Image own, uint32_t ownW, uint32_t ownH, uint32_t srcHalf);

 private:
  struct EyeRes {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    vk::Extent2D extent{0, 0};
    std::vector<vk::Image> images;  // XR 所有の非所有ビュー。破棄しない。
  };

  static constexpr vk::DeviceSize kStageSize = 4;  // マーカー 1 画素。

  const vk::raii::Device* device_;
  const vk::raii::Queue* queue_;
  vk::raii::CommandPool pool_{nullptr};
  std::vector<vk::raii::CommandBuffer> cmds_;
  vk::raii::Fence fence_{nullptr};
  vk::raii::Buffer stage_{nullptr};
  vk::raii::DeviceMemory stageMem_{nullptr};
  std::vector<EyeRes> eyes_;
  MarkerLayout markers_;
};

}  // namespace xrift::gfx
