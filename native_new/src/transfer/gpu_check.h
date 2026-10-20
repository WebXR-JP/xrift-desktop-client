#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "common/xr_vulkan.h"
#include "gfx/vulkan_context.h"

// M1b 全画面 GPU 検査。自前イメージへのコピー直後に、同じ GPU 上の compute
// シェーダ（shaders/check.comp）で全タイルを検査する。画像全体は CPU に戻さず、
// 176 バイトの結果バッファだけを読む（制御メタデータの例外。DESIGN §1・§7）。
// XR ループを readback 待ちで止めないことが目的であり、M1 では転送ループを
// 検査時間だけ延ばすことを許す（内訳は Timings で計測し、統計で報告する）。

namespace xrift::transfer {

// GPU 完了を確認できない失敗（submit・fence 待ちの失敗）。
// timeout は返却許可ではないため、呼び出し側は Done を送らず受付停止に回す。
// submit 前の失敗とは区別する（shared_texture 側の UnconfirmedCompletion へ載せ替える）。
class UnconfirmedCheck : public std::runtime_error {
 public:
  explicit UnconfirmedCheck(const std::string& message) : std::runtime_error(message) {}
};

class GpuChecker {
 public:
  static constexpr uint32_t kMaxRecords = 8;
  // 結果バッファの大きさ: 先頭 3 ワード + 記録 8 件 x 5 ワード。
  static constexpr size_t kResultSize = 12 + kMaxRecords * 5 * 4;

  // 不一致の種別。シェーダの kind と一致させること。
  static constexpr uint32_t kKindTile = 0;       // タイル中心がハッシュ期待値と不一致。
  static constexpr uint32_t kKindFrameCell = 1;  // フレーム番号セルが単色ではない。
  static constexpr uint32_t kKindCorner = 2;     // 隅ブロックが固定色と不一致。

  struct Record {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t expected = 0;  // R を下位バイトに詰めた 32bit（シェーダの PackColor と同じ並び）。
    uint32_t actual = 0;
    uint32_t kind = 0;
  };

  struct Report {
    uint32_t mismatchCount = 0;
    uint32_t decodedFrame = 0;  // フレーム番号ブロックから復号した 16bit 値。
    uint32_t checkedTiles = 0;  // 範囲内タイルを数えたスレッド数。
    std::array<Record, kMaxRecords> records{};
  };

  explicit GpuChecker(const gfx::VulkanContext& vulkan);
  ~GpuChecker();

  GpuChecker(const GpuChecker&) = delete;
  GpuChecker& operator=(const GpuChecker&) = delete;

  // 検査対象の自前イメージを結び付ける。同じ image・extent なら何もしない。
  // ビューの作り直しを伴うため、EnsureOwnImage の作り直し直後に呼ぶ。
  void BindImage(vk::Image image, uint32_t width, uint32_t height);

  // 1 フレームを検査する。BindImage 済みの extent と一致していること。
  // 戻り値は結果バッファの複写である。
  Report Check(uint32_t width, uint32_t height);

 private:
  const vk::raii::Device& device_;
  const vk::raii::Queue& queue_;
  uint32_t queueFamily_ = 0;

  vk::raii::Sampler sampler_{nullptr};
  vk::raii::DescriptorSetLayout setLayout_{nullptr};
  vk::raii::PipelineLayout pipelineLayout_{nullptr};
  vk::raii::Pipeline pipeline_{nullptr};
  vk::raii::DescriptorPool pool_{nullptr};
  vk::raii::DescriptorSet set_{nullptr};

  vk::raii::Buffer resultBuffer_{nullptr};
  vk::raii::DeviceMemory resultMemory_{nullptr};
  void* mapped_ = nullptr;  // 結果バッファの永続マップ。176 バイトのゼロ埋めと読みに使う。

  vk::raii::CommandPool cmdPool_{nullptr};
  vk::raii::CommandBuffer cmd_{nullptr};
  vk::raii::Fence fence_{nullptr};

  vk::raii::ImageView view_{nullptr};
  VkImage boundRaw_ = VK_NULL_HANDLE;
  uint32_t boundWidth_ = 0;
  uint32_t boundHeight_ = 0;
  bool hasBinding_ = false;
};

// 統計ログ用の種別名。未知値には「不明」を返す（落とさない）。
inline const char* MismatchKindName(uint32_t kind) {
  switch (kind) {
    case GpuChecker::kKindTile:
      return "タイル";
    case GpuChecker::kKindFrameCell:
      return "番号セル";
    case GpuChecker::kKindCorner:
      return "隅";
    default:
      return "不明";
  }
}

}  // namespace xrift::transfer
