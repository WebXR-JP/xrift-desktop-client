#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "common/xr_vulkan.h"
#include "gfx/clear_pass.h"
#include "gfx/swapchains.h"
#include "gfx/vulkan_context.h"
#include "xr/instance.h"
#include "xr/session.h"

// フレームループと --probe を束ねる。各初期化の糊付けだけを行い、
// 描画や状態遷移の実体は gfx/xr 側のクラスに置く。

namespace xrift {

// --probe の実体。instance と Vulkan 初期化だけ行い環境情報を出す。成功時 0。
// セッションは作らない。
int Probe();

class XrApp {
 public:
  XrApp();
  ~XrApp();

  XrApp(const XrApp&) = delete;
  XrApp& operator=(const XrApp&) = delete;

  // 通常起動のフレームループ。終了要求まで戻らない。成功時 0。
  int Run();

 private:
  void PollEvents();
  void Frame();
  void Shutdown();

  std::unique_ptr<xr::Instance> instance_;
  std::unique_ptr<gfx::VulkanContext> vulkan_;
  std::unique_ptr<xr::Session> session_;
  std::unique_ptr<gfx::Swapchains> swapchains_;
  std::unique_ptr<gfx::ClearPass> clear_pass_;

  XrCompositionLayerProjection layer_{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerProjectionView proj_views_[2] = {
      {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
      {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
  };
  std::vector<XrView> views_;
  bool session_running_ = false;
  bool quit_ = false;
};

}  // namespace xrift
