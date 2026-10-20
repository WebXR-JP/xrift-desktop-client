#pragma once

#include <string>
#include <vector>

#include "common/xr_vulkan.h"
#include "gfx/vulkan_context.h"
#include "xr/instance.h"

// Vk バインディング付き XrSession、参照空間、view 構成、状態遷移を担う。
// イベントの受付自体は app 側が行い、状態変化は setState で受け取る。
// M4: 参照空間の選択（local-floor→stage→local の後退と明示指定）と
// REFERENCE_SPACE_CHANGE_PENDING 後の空間作り直しを持つ。

namespace xrift::xr {

// 参照空間の要求。Auto は local-floor→stage→local の順で後退する。
enum class SpacePreference {
  Auto,
  Local,
  Stage,
  LocalFloor,
};

// --space の値を要求に直す。不正時は理由付きの例外にする（黙って既定にしない）。
SpacePreference ParseSpacePreference(const std::string& name);
const char* SpacePreferenceName(SpacePreference pref);
const char* SpaceTypeName(XrReferenceSpaceType type);

class Session {
 public:
  Session(const Instance& xr, const gfx::VulkanContext& vk,
          SpacePreference pref = SpacePreference::Local);
  ~Session();

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  XrSession handle() const { return session_; }
  XrSpace space() const { return space_; }
  const std::vector<XrViewConfigurationView>& views() const { return views_; }
  XrSessionState state() const { return state_; }
  void setState(XrSessionState state) { state_ = state; }
  bool isRunning() const { return running_; }
  bool valid() const { return session_ != XR_NULL_HANDLE; }
  XrReferenceSpaceType spaceType() const { return space_type_; }

  // READY 時に呼ぶ。成功すると実行中になる。
  void Begin();
  // STOPPING 時に呼ぶ。成功すると実行中でなくなる。
  void End();
  // 明示破棄。多重呼び出しと未生成呼び出しは無視する。
  void Destroy();
  // REFERENCE_SPACE_CHANGE_PENDING を受けた後の空間の作り直し。
  // 同種別の空間を作り直し、新しい原点で locate できることを確認する。
  // 成功時は true。失敗時は旧空間を保ち false を返す（世代は進めない）。
  bool RecreateSpaceForPendingChange();

 private:
  bool CreateSpace(XrReferenceSpaceType type);
  std::vector<XrReferenceSpaceType> EnumerateSpaces() const;

  XrInstance instance_ = XR_NULL_HANDLE;
  XrSession session_ = XR_NULL_HANDLE;
  XrSpace space_ = XR_NULL_HANDLE;
  std::vector<XrViewConfigurationView> views_;
  XrSessionState state_ = XR_SESSION_STATE_UNKNOWN;
  bool running_ = false;
  XrReferenceSpaceType space_type_ = XR_REFERENCE_SPACE_TYPE_LOCAL;
  SpacePreference pref_ = SpacePreference::Local;
};

}  // namespace xrift::xr
