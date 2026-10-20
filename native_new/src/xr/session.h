#pragma once

#include <vector>

#include "common/xr_vulkan.h"
#include "gfx/vulkan_context.h"
#include "xr/instance.h"

// Vk バインディング付き XrSession、LOCAL 参照空間、view 構成、状態遷移を担う。
// イベントの受付自体は app 側が行い、状態変化は setState で受け取る。

namespace xrift::xr {

class Session {
 public:
  Session(const Instance& xr, const gfx::VulkanContext& vk);
  ~Session();

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  XrSession handle() const { return session_; }
  XrSpace space() const { return space_; }
  const std::vector<XrViewConfigurationView>& views() const { return views_; }
  XrSessionState state() const { return state_; }
  void setState(XrSessionState state) { state_ = state; }
  bool isRunning() const { return running_; }

  // READY 時に呼ぶ。成功すると実行中になる。
  void Begin();
  // STOPPING 時に呼ぶ。成功すると実行中でなくなる。
  void End();
  // 明示破棄。多重呼び出しと未生成呼び出しは無視する。
  void Destroy();

 private:
  XrInstance instance_ = XR_NULL_HANDLE;
  XrSession session_ = XR_NULL_HANDLE;
  XrSpace space_ = XR_NULL_HANDLE;
  std::vector<XrViewConfigurationView> views_;
  XrSessionState state_ = XR_SESSION_STATE_UNKNOWN;
  bool running_ = false;
};

}  // namespace xrift::xr
