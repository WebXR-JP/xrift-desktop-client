#include "xr/input.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/log.h"
#include "common/xr_result.h"

namespace xrift::xr {
namespace {

// suggest 対象の profile 一覧。PICO 系（bytedance）の存在は OpenXR 仕様
// （release-1.1.65 の xr.xml）に確認した。無いランタイムへの suggest は
// 失敗として記録し、他を止めない。
constexpr const char* kProfiles[] = {
    "/interaction_profiles/khr/simple_controller",
    "/interaction_profiles/oculus/touch_controller",
    "/interaction_profiles/valve/index_controller",
    "/interaction_profiles/htc/vive_controller",
    "/interaction_profiles/microsoft/motion_controller",
    "/interaction_profiles/bytedance/pico4_controller",
    "/interaction_profiles/bytedance/pico_neo3_controller",
    "/interaction_profiles/bytedance/pico_ultra_controller_bd",
    "/interaction_profiles/bytedance/pico_g3_controller",
};

XrPath ToPath(XrInstance instance, const char* s) {
  XrPath p = XR_NULL_PATH;
  CheckXr(xrStringToPath(instance, s, &p), s);
  return p;
}

XrAction MakeAction(XrActionSet set, const char* name, const char* localized,
                    XrActionType type, const XrPath* subs, uint32_t subCount) {
  XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
  std::snprintf(info.actionName, XR_MAX_ACTION_NAME_SIZE, "%s", name);
  std::snprintf(info.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "%s", localized);
  info.actionType = type;
  info.countSubactionPaths = subCount;
  info.subactionPaths = subs;
  XrAction action = XR_NULL_HANDLE;
  CheckXr(xrCreateAction(set, &info, &action), name);
  return action;
}

// interaction profile 名の分類。wire 側の ProfileIdFromName（ipc 側の protocol.h）と
// 対になる表であり、同じ名前一覧・同じ末尾一致で判定する。片方の顔ぶれを変えたら
// 両方を変えること。wire の数値 ID には触れず、xr 層の SupportMask 判定だけに使う。
ControllerProfile ClassifyProfile(const std::string& path) {
  if (path.empty()) {
    return ControllerProfile::None;
  }
  // 最終節の比較で十分である（suggest 表の顔ぶれは最終節が重複しない）。
  // 将来 profile を足して最終節が衝突したら、ここを完全一致に戻すこと。
  const char* name = path.c_str();
  const char* tail = name;
  for (const char* p = name; *p != '\0'; ++p) {
    if (*p == '/') {
      tail = p + 1;
    }
  }
  auto eq = [](const char* a, const char* b) {
    while (*a != '\0' && *a == *b) {
      ++a;
      ++b;
    }
    return *a == *b;
  };
  if (eq(tail, "simple_controller")) {
    return ControllerProfile::Simple;
  }
  if (eq(tail, "touch_controller")) {
    return ControllerProfile::Oculus;
  }
  if (eq(tail, "index_controller")) {
    return ControllerProfile::Index;
  }
  if (eq(tail, "vive_controller")) {
    return ControllerProfile::Vive;
  }
  if (eq(tail, "motion_controller")) {
    return ControllerProfile::Motion;
  }
  if (eq(tail, "pico4_controller")) {
    return ControllerProfile::Pico4;
  }
  if (eq(tail, "pico_neo3_controller")) {
    return ControllerProfile::PicoNeo3;
  }
  if (eq(tail, "pico_ultra_controller_bd")) {
    return ControllerProfile::PicoUltra;
  }
  if (eq(tail, "pico_g3_controller")) {
    return ControllerProfile::PicoG3;
  }
  return ControllerProfile::Unknown;
}

}  // namespace

InputManager::InputManager() = default;

InputManager::~InputManager() {
  DetachSpaces();
  if (action_set_ != XR_NULL_HANDLE) {
    const XrResult result = xrDestroyActionSet(action_set_);
    if (result != XR_SUCCESS) {
      Warn("xrDestroyActionSet に失敗: %s", XrResultToString(result));
    }
    action_set_ = XR_NULL_HANDLE;
  }
}

void InputManager::Create(XrInstance instance) {
  instance_ = instance;
  XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
  std::snprintf(set_info.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "%s", "xrift");
  std::snprintf(set_info.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "%s",
                "XRift");
  CheckXr(xrCreateActionSet(instance, &set_info, &action_set_), "xrCreateActionSet(xrift)");
  Info("アクションセット xrift を作成した");

  hand_paths_[0] = ToPath(instance, "/user/hand/left");
  hand_paths_[1] = ToPath(instance, "/user/hand/right");

  grip_pose_ = MakeAction(action_set_, "grip_pose", "Grip Pose", XR_ACTION_TYPE_POSE_INPUT,
                          hand_paths_, 2);
  aim_pose_ =
      MakeAction(action_set_, "aim_pose", "Aim Pose", XR_ACTION_TYPE_POSE_INPUT,
                 hand_paths_, 2);
  trigger_ = MakeAction(action_set_, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT,
                        hand_paths_, 2);
  squeeze_ = MakeAction(action_set_, "squeeze", "Squeeze", XR_ACTION_TYPE_FLOAT_INPUT,
                        hand_paths_, 2);
  thumbstick_ = MakeAction(action_set_, "thumbstick", "Thumbstick",
                           XR_ACTION_TYPE_VECTOR2F_INPUT, hand_paths_, 2);
  thumbstick_click_ = MakeAction(action_set_, "thumbstick_click", "Thumbstick Click",
                                 XR_ACTION_TYPE_BOOLEAN_INPUT, hand_paths_, 2);
  trackpad_ = MakeAction(action_set_, "trackpad", "Trackpad",
                         XR_ACTION_TYPE_VECTOR2F_INPUT, hand_paths_, 2);
  trackpad_click_ = MakeAction(action_set_, "trackpad_click", "Trackpad Click",
                               XR_ACTION_TYPE_BOOLEAN_INPUT, hand_paths_, 2);
  primary_ = MakeAction(action_set_, "primary_button", "A / X Button",
                        XR_ACTION_TYPE_BOOLEAN_INPUT, hand_paths_, 2);
  secondary_ = MakeAction(action_set_, "secondary_button", "B / Y Button",
                          XR_ACTION_TYPE_BOOLEAN_INPUT, hand_paths_, 2);
  menu_ = MakeAction(action_set_, "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT,
                     hand_paths_, 2);
  haptic_ = MakeAction(action_set_, "haptic", "Haptic",
                       XR_ACTION_TYPE_VIBRATION_OUTPUT, hand_paths_, 2);
  Info("アクション 12 件を作成した（grip/aim pose、trigger、squeeze、thumbstick/trackpad＋click、"
       "primary/secondary、menu、haptic）");

  // パス文字列の寿命を suggest 呼び出しの間だけ保つ。
  const char* kLeft = "/user/hand/left";
  const char* kRight = "/user/hand/right";
  auto path = [](const char* hand, const std::string& sub) {
    return std::string(hand) + sub;
  };
  // profile 別の束縛表。addon.cpp の suggest 表と同じ顔ぶれに PICO 系を足す。
  // PICO 4/neo3/ultra の部品は仕様の interaction_profile 定義どおり
  // （trigger/value、squeeze/value、thumbstick＋click、x/y/a/b、menu(左)、grip/aim、haptic。
  // trackpad は無い）。g3 は trigger・menu・thumbstick のみを持つ。
  for (const char* profile : kProfiles) {
    std::vector<std::string> owned;
    owned.reserve(32);
    std::vector<XrActionSuggestedBinding> bindings;
    bindings.reserve(32);
    const std::string name = profile;
    const bool is_simple = name == "/interaction_profiles/khr/simple_controller";
    const bool is_oculus = name == "/interaction_profiles/oculus/touch_controller";
    const bool is_pico4 = name == "/interaction_profiles/bytedance/pico4_controller";
    const bool is_neo3 = name == "/interaction_profiles/bytedance/pico_neo3_controller";
    const bool is_ultra = name == "/interaction_profiles/bytedance/pico_ultra_controller_bd";
    const bool is_g3 = name == "/interaction_profiles/bytedance/pico_g3_controller";
    const bool is_index = name == "/interaction_profiles/valve/index_controller";
    const bool is_vive = name == "/interaction_profiles/htc/vive_controller";
    const bool is_motion = name == "/interaction_profiles/microsoft/motion_controller";
    const bool is_pico_full = is_pico4 || is_neo3 || is_ultra;
    // owned の index 記録。path 解決は owned 確定後に行う（再配置で指し先が変わるため）。
    struct Pending {
      size_t index;
    };
    std::vector<Pending> pending;
    auto add = [&](XrAction action, const char* hand, const std::string& sub) {
      owned.push_back(path(hand, sub));
      XrActionSuggestedBinding b{};
      b.action = action;
      b.binding = XR_NULL_PATH;
      bindings.push_back(b);
      pending.push_back({owned.size() - 1});
    };
    for (const char* hand : {kLeft, kRight}) {
      add(grip_pose_, hand, "/input/grip/pose");
      add(aim_pose_, hand, "/input/aim/pose");
      add(haptic_, hand, "/output/haptic");
    }
    if (is_simple) {
      for (const char* hand : {kLeft, kRight}) {
        add(trigger_, hand, "/input/select/click");
        add(menu_, hand, "/input/menu/click");
      }
    } else if (is_oculus || is_pico_full) {
      for (const char* hand : {kLeft, kRight}) {
        add(trigger_, hand, "/input/trigger/value");
        add(squeeze_, hand, "/input/squeeze/value");
        add(thumbstick_, hand, "/input/thumbstick");
        add(thumbstick_click_, hand, "/input/thumbstick/click");
      }
      add(primary_, kLeft, "/input/x/click");
      add(secondary_, kLeft, "/input/y/click");
      add(menu_, kLeft, "/input/menu/click");
      add(primary_, kRight, "/input/a/click");
      add(secondary_, kRight, "/input/b/click");
    } else if (is_index) {
      for (const char* hand : {kLeft, kRight}) {
        add(trigger_, hand, "/input/trigger/value");
        add(squeeze_, hand, "/input/squeeze/value");
        add(thumbstick_, hand, "/input/thumbstick");
        add(thumbstick_click_, hand, "/input/thumbstick/click");
        add(trackpad_, hand, "/input/trackpad");
        add(primary_, hand, "/input/a/click");
        add(secondary_, hand, "/input/b/click");
      }
    } else if (is_vive) {
      for (const char* hand : {kLeft, kRight}) {
        add(trigger_, hand, "/input/trigger/value");
        add(squeeze_, hand, "/input/squeeze/click");
        add(trackpad_, hand, "/input/trackpad");
        add(trackpad_click_, hand, "/input/trackpad/click");
        add(menu_, hand, "/input/menu/click");
      }
    } else if (is_motion) {
      for (const char* hand : {kLeft, kRight}) {
        add(trigger_, hand, "/input/trigger/value");
        add(squeeze_, hand, "/input/squeeze/click");
        add(thumbstick_, hand, "/input/thumbstick");
        add(thumbstick_click_, hand, "/input/thumbstick/click");
        add(trackpad_, hand, "/input/trackpad");
        add(trackpad_click_, hand, "/input/trackpad/click");
        add(menu_, hand, "/input/menu/click");
      }
    } else if (is_g3) {
      for (const char* hand : {kLeft, kRight}) {
        add(trigger_, hand, "/input/trigger/value");
        add(thumbstick_, hand, "/input/thumbstick");
        add(thumbstick_click_, hand, "/input/thumbstick/click");
        add(menu_, hand, "/input/menu/click");
      }
    }
    for (size_t i = 0; i < bindings.size(); ++i) {
      bindings[i].binding = ToPath(instance, owned[pending[i].index].c_str());
    }
    XrInteractionProfileSuggestedBinding suggest{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggest.interactionProfile = ToPath(instance, profile);
    suggest.suggestedBindings = bindings.data();
    suggest.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
    const XrResult result = xrSuggestInteractionProfileBindings(instance, &suggest);
    if (result == XR_SUCCESS) {
      Info("suggest 成功: %s（%u 件）", profile,
           static_cast<unsigned>(bindings.size()));
    } else {
      Warn("suggest 失敗: %s: %s（他 profile は続行する）", profile, XrResultToString(result));
    }
  }
}

bool InputManager::AttachSpaces(XrSession session) {
  if (action_set_ == XR_NULL_HANDLE || session == XR_NULL_HANDLE) {
    return false;
  }
  XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  attach.countActionSets = 1;
  attach.actionSets = &action_set_;
  const XrResult attached = xrAttachSessionActionSets(session, &attach);
  if (attached != XR_SUCCESS) {
    Warn("xrAttachSessionActionSets に失敗: %s", XrResultToString(attached));
    return false;
  }
  Info("アクションセットを attach した");
  for (int i = 0; i < 2; ++i) {
    XrActionSpaceCreateInfo space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    space_info.poseInActionSpace.orientation.w = 1.0f;
    space_info.subactionPath = hand_paths_[i];
    space_info.action = grip_pose_;
    if (xrCreateActionSpace(session, &space_info, &grip_spaces_[i]) != XR_SUCCESS) {
      Warn("grip 空間の生成に失敗（%s）", i == 0 ? "左" : "右");
      return false;
    }
    space_info.action = aim_pose_;
    if (xrCreateActionSpace(session, &space_info, &aim_spaces_[i]) != XR_SUCCESS) {
      Warn("aim 空間の生成に失敗（%s）", i == 0 ? "左" : "右");
      return false;
    }
  }
  Info("grip/aim 空間を生成した（左右）");
  return true;
}

void InputManager::DetachSpaces() {
  for (int i = 0; i < 2; ++i) {
    if (grip_spaces_[i] != XR_NULL_HANDLE) {
      xrDestroySpace(grip_spaces_[i]);
      grip_spaces_[i] = XR_NULL_HANDLE;
    }
    if (aim_spaces_[i] != XR_NULL_HANDLE) {
      xrDestroySpace(aim_spaces_[i]);
      aim_spaces_[i] = XR_NULL_HANDLE;
    }
  }
}

bool InputManager::Sync(XrSession session, bool focused) {
  if (!focused || session == XR_NULL_HANDLE || action_set_ == XR_NULL_HANDLE) {
    return false;
  }
  XrActiveActionSet active{action_set_, XR_NULL_PATH};
  XrActionsSyncInfo sync_info{XR_TYPE_ACTIONS_SYNC_INFO};
  sync_info.countActiveActionSets = 1;
  sync_info.activeActionSets = &active;
  const XrResult result = xrSyncActions(session, &sync_info);
  ++stats_.syncs;
  if (result != XR_SUCCESS) {
    ++stats_.syncFailed;
    Warn("xrSyncActions に失敗: %s", XrResultToString(result));
    return false;
  }
  return true;
}

bool InputManager::QueryProfile(XrSession session, int hand, std::string* out) {
  XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
  if (xrGetCurrentInteractionProfile(session, hand_paths_[hand], &state) != XR_SUCCESS ||
      state.interactionProfile == XR_NULL_PATH) {
    out->clear();
    return false;
  }
  char buffer[XR_MAX_PATH_LENGTH] = {};
  uint32_t length = 0;
  if (xrPathToString(instance_, state.interactionProfile, sizeof(buffer), &length, buffer) !=
      XR_SUCCESS) {
    out->clear();
    return false;
  }
  *out = buffer;
  return true;
}

uint8_t InputManager::SupportMask(ControllerProfile profile, XrAction action) const {
  // suggest 表と一致させる。未知・未取得の profile は束縛の有無が分からないため
  // 取得呼び出しの結果（PATH_UNSUPPORTED か否か）で判断する（呼び出し側で処理）。
  if (profile == ControllerProfile::None || profile == ControllerProfile::Unknown) {
    return 1;
  }
  const bool simple = profile == ControllerProfile::Simple;
  const bool oculus = profile == ControllerProfile::Oculus;
  const bool pico_full = profile == ControllerProfile::Pico4 ||
                         profile == ControllerProfile::PicoNeo3 ||
                         profile == ControllerProfile::PicoUltra;
  const bool pico_g3 = profile == ControllerProfile::PicoG3;
  const bool index = profile == ControllerProfile::Index;
  const bool vive = profile == ControllerProfile::Vive;
  const bool motion = profile == ControllerProfile::Motion;
  if (action == grip_pose_ || action == aim_pose_ || action == haptic_) {
    return 1;
  }
  if (action == trigger_) {
    return (simple || oculus || pico_full || pico_g3 || index || vive || motion) ? 1 : 0;
  }
  if (action == squeeze_) {
    return (oculus || pico_full || index || vive || motion) ? 1 : 0;
  }
  if (action == thumbstick_ || action == thumbstick_click_) {
    return (oculus || pico_full || pico_g3 || index || motion) ? 1 : 0;
  }
  if (action == trackpad_ || action == trackpad_click_) {
    return (index || vive || motion) ? 1 : 0;
  }
  if (action == primary_ || action == secondary_) {
    return (oculus || pico_full || index) ? 1 : 0;
  }
  if (action == menu_) {
    return (simple || oculus || pico_full || pico_g3 || vive || motion) ? 1 : 0;
  }
  return 0;
}

float InputManager::QueryFloat(XrSession session, XrAction action, XrPath sub, FieldState* state,
                               bool focused, uint8_t supported) {
  if (!supported) {
    *state = FieldState::Unsupported;
    return 0.0f;
  }
  if (!focused) {
    *state = FieldState::Inactive;
    return 0.0f;
  }
  XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
  info.action = action;
  info.subactionPath = sub;
  XrActionStateFloat value{XR_TYPE_ACTION_STATE_FLOAT};
  const XrResult result = xrGetActionStateFloat(session, &info, &value);
  if (result == XR_ERROR_PATH_UNSUPPORTED) {
    *state = FieldState::Unsupported;
    return 0.0f;
  }
  if (result != XR_SUCCESS || value.isActive == XR_FALSE) {
    *state = FieldState::Inactive;
    return 0.0f;
  }
  *state = FieldState::Active;
  return value.currentState;
}

void InputManager::QueryVec2(XrSession session, XrAction action, XrPath sub, float out[2],
                             FieldState* state, bool focused, uint8_t supported) {
  out[0] = out[1] = 0.0f;
  if (!supported) {
    *state = FieldState::Unsupported;
    return;
  }
  if (!focused) {
    *state = FieldState::Inactive;
    return;
  }
  XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
  info.action = action;
  info.subactionPath = sub;
  XrActionStateVector2f value{XR_TYPE_ACTION_STATE_VECTOR2F};
  const XrResult result = xrGetActionStateVector2f(session, &info, &value);
  if (result == XR_ERROR_PATH_UNSUPPORTED) {
    *state = FieldState::Unsupported;
    return;
  }
  if (result != XR_SUCCESS || value.isActive == XR_FALSE) {
    *state = FieldState::Inactive;
    return;
  }
  *state = FieldState::Active;
  out[0] = value.currentState.x;
  out[1] = value.currentState.y;
}

bool InputManager::QueryBool(XrSession session, XrAction action, XrPath sub, InputButton* out,
                             bool focused, uint8_t supported) {
  out->pressed = false;
  if (!supported) {
    out->state = FieldState::Unsupported;
    return false;
  }
  if (!focused) {
    out->state = FieldState::Inactive;
    return false;
  }
  XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
  info.action = action;
  info.subactionPath = sub;
  XrActionStateBoolean value{XR_TYPE_ACTION_STATE_BOOLEAN};
  const XrResult result = xrGetActionStateBoolean(session, &info, &value);
  if (result == XR_ERROR_PATH_UNSUPPORTED) {
    out->state = FieldState::Unsupported;
    return false;
  }
  if (result != XR_SUCCESS || value.isActive == XR_FALSE) {
    out->state = FieldState::Inactive;
    return false;
  }
  out->state = FieldState::Active;
  out->pressed = value.currentState == XR_TRUE;
  return out->pressed;
}

InputSnapshot InputManager::Snapshot(XrSession session, XrSpace appSpace, XrTime displayTime,
                                     bool focused, uint64_t frameNo) {
  InputSnapshot snapshot{};
  snapshot.focused = focused;
  snapshot.frameNo = frameNo;
  ++stats_.snapshots;
  if (session == XR_NULL_HANDLE) {
    return snapshot;
  }
  for (int i = 0; i < 2; ++i) {
    HandInput& hand = snapshot.hands[i];
    std::string profile;
    if (QueryProfile(session, i, &profile)) {
      hand.profile = profile;
      if (last_profiles_[i] != profile) {
        Info("interaction profile: %s（%s）", profile.c_str(), i == 0 ? "左" : "右");
        last_profiles_[i] = profile;
        ++stats_.profileChanges;
      }
    } else {
      hand.profile.clear();
      if (!last_profiles_[i].empty()) {
        Info("interaction profile: 未取得（%s。未装着・未束縛のいずれか）", i == 0 ? "左" : "右");
        last_profiles_[i].clear();
        ++stats_.profileChanges;
      }
    }
    const ControllerProfile classified = ClassifyProfile(hand.profile);
    // pose の locate。失敗・無効は未接続側に倒す（例外にしない）。
    auto locate = [&](XrSpace space, XrPosef* pose, bool* valid) {
      *valid = false;
      if (space == XR_NULL_HANDLE) {
        return;
      }
      XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
      if (xrLocateSpace(space, appSpace, displayTime, &location) != XR_SUCCESS) {
        return;
      }
      if ((location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0 ||
          (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0) {
        return;
      }
      *pose = location.pose;
      *valid = true;
    };
    locate(grip_spaces_[i], &hand.grip, &hand.gripValid);
    locate(aim_spaces_[i], &hand.aim, &hand.aimValid);
    hand.connected = hand.gripValid || hand.aimValid;

    hand.trigger = QueryFloat(session, trigger_, hand_paths_[i], &hand.triggerState, focused,
                              SupportMask(classified, trigger_));
    hand.squeeze = QueryFloat(session, squeeze_, hand_paths_[i], &hand.squeezeState, focused,
                              SupportMask(classified, squeeze_));
    QueryVec2(session, thumbstick_, hand_paths_[i], hand.thumbstick, &hand.thumbstickState,
              focused, SupportMask(classified, thumbstick_));
    QueryVec2(session, trackpad_, hand_paths_[i], hand.trackpad, &hand.trackpadState, focused,
              SupportMask(classified, trackpad_));
    QueryBool(session, thumbstick_click_, hand_paths_[i], &hand.thumbstickClick, focused,
              SupportMask(classified, thumbstick_click_));
    QueryBool(session, trackpad_click_, hand_paths_[i], &hand.trackpadClick, focused,
              SupportMask(classified, trackpad_click_));
    QueryBool(session, primary_, hand_paths_[i], &hand.primary, focused,
              SupportMask(classified, primary_));
    QueryBool(session, secondary_, hand_paths_[i], &hand.secondary, focused,
              SupportMask(classified, secondary_));
    QueryBool(session, menu_, hand_paths_[i], &hand.menu, focused,
              SupportMask(classified, menu_));
  }
  return snapshot;
}

bool InputManager::ApplyHaptic(XrSession session, uint32_t hand, float amplitude,
                               uint32_t durationMs, bool focused) {
  if (hand > 1 || session == XR_NULL_HANDLE || haptic_ == XR_NULL_HANDLE || !focused) {
    // 呼び出し自体を行わない。結果は前回のままにし、未呼出しだけを残す。
    last_haptic_called_ = false;
    ++stats_.hapticInactive;
    return false;
  }
  XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
  vibration.amplitude = amplitude < 0.0f ? 0.0f : (amplitude > 1.0f ? 1.0f : amplitude);
  vibration.duration = static_cast<XrDuration>(durationMs) * 1000000;
  vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
  XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
  info.action = haptic_;
  info.subactionPath = hand_paths_[hand];
  // 非対応・inactive の失敗は統計に残し、例外でループを止めない。
  // 呼び出しの有無と XrResult は呼び出し側のログ用に残す。
  const XrResult result =
      xrApplyHapticFeedback(session, &info, reinterpret_cast<XrHapticBaseHeader*>(&vibration));
  last_haptic_called_ = true;
  last_haptic_result_ = result;
  if (result == XR_SUCCESS) {
    ++stats_.hapticOk;
    return true;
  }
  if (result == XR_ERROR_PATH_UNSUPPORTED) {
    ++stats_.hapticInactive;
  } else {
    ++stats_.hapticFailed;
    Warn("xrApplyHapticFeedback に失敗（%s）: %s", hand == 0 ? "左" : "右",
         XrResultToString(result));
  }
  return false;
}

std::string InputManager::Summarize(const InputSnapshot& snapshot) {
  std::string out;
  for (int i = 0; i < 2; ++i) {
    const HandInput& hand = snapshot.hands[i];
    char line[512];
    std::string buttons;
    auto add = [&](const char* name, const InputButton& button) {
      if (button.state == FieldState::Active && button.pressed) {
        if (!buttons.empty()) {
          buttons += ",";
        }
        buttons += name;
      }
    };
    add("stick", hand.thumbstickClick);
    add("pad", hand.trackpadClick);
    add("primary", hand.primary);
    add("secondary", hand.secondary);
    add("menu", hand.menu);
    if (buttons.empty()) {
      buttons = "-";
    }
    std::snprintf(line, sizeof(line), "%s: %s prof=%s trig=%.2f%s sq=%.2f%s stick=(%+.2f,%+.2f)%s",
                  i == 0 ? "左" : "右", hand.connected ? "接続" : "未接続",
                  hand.profile.empty() ? "-" : hand.profile.c_str(), hand.trigger,
                  hand.triggerState == FieldState::Active
                      ? ""
                      : (hand.triggerState == FieldState::Inactive ? "(inactive)" : "(非対応)"),
                  hand.squeeze,
                  hand.squeezeState == FieldState::Active
                      ? ""
                      : (hand.squeezeState == FieldState::Inactive ? "(inactive)" : "(非対応)"),
                  hand.thumbstick[0], hand.thumbstick[1],
                  hand.thumbstickState == FieldState::Active
                      ? ""
                      : (hand.thumbstickState == FieldState::Inactive ? "(inactive)" : "(非対応)"));
    if (i > 0) {
      out += " | ";
    }
    out += line;
    out += " btn=" + buttons;
  }
  return out;
}

std::string InputManager::SummarizeWithLatch(const InputSnapshot& snapshot, uint8_t latch0,
                                             uint8_t latch1) {
  return Summarize(snapshot) + " latch=L:" + FormatLatch(latch0) + " R:" + FormatLatch(latch1);
}

uint8_t InputManager::PressedBits(const HandInput& hand) {
  uint8_t bits = 0;
  const InputButton* btns[5] = {&hand.thumbstickClick, &hand.trackpadClick, &hand.primary,
                                &hand.secondary, &hand.menu};
  for (int b = 0; b < 5; ++b) {
    if (btns[b]->pressed && btns[b]->state == FieldState::Active) {
      bits |= static_cast<uint8_t>(1u << b);
    }
  }
  return bits;
}

std::string InputManager::FormatLatch(uint8_t bits) {
  static constexpr const char* kNames[5] = {"stick", "pad", "primary", "secondary", "menu"};
  std::string out;
  for (int b = 0; b < 5; ++b) {
    if ((bits & static_cast<uint8_t>(1u << b)) != 0) {
      if (!out.empty()) {
        out += ",";
      }
      out += kNames[b];
    }
  }
  if (out.empty()) {
    out = "-";
  }
  return out;
}

std::string InputManager::DescribeEdges(const InputSnapshot& prev, const InputSnapshot& cur) {
  static constexpr const char* kNames[5] = {"stick", "pad", "primary", "secondary", "menu"};
  std::string out;
  auto emit = [&](const std::string& token) {
    if (!out.empty()) {
      out += " ";
    }
    out += token;
  };
  for (int i = 0; i < 2; ++i) {
    const HandInput& a = prev.hands[i];
    const HandInput& b = cur.hands[i];
    const char* hand = (i == 0) ? "L" : "R";
    const InputButton* prev_btns[5] = {&a.thumbstickClick, &a.trackpadClick, &a.primary,
                                       &a.secondary, &a.menu};
    const InputButton* cur_btns[5] = {&b.thumbstickClick, &b.trackpadClick, &b.primary,
                                      &b.secondary, &b.menu};
    for (int k = 0; k < 5; ++k) {
      const bool was = prev_btns[k]->pressed && prev_btns[k]->state == FieldState::Active;
      const bool now = cur_btns[k]->pressed && cur_btns[k]->state == FieldState::Active;
      if (was != now) {
        emit(std::string(hand) + "." + kNames[k] + (now ? " press" : " release"));
      }
    }
    const bool was_trigger = a.trigger > kInputEdgeAnalogEps;
    const bool now_trigger = b.trigger > kInputEdgeAnalogEps;
    if (was_trigger != now_trigger) {
      char token[64];
      std::snprintf(token, sizeof(token), "%s.trigger %s(%.2f)", hand,
                    now_trigger ? "off->on" : "on->off", b.trigger);
      emit(token);
    }
    const bool was_squeeze = a.squeeze > kInputEdgeAnalogEps;
    const bool now_squeeze = b.squeeze > kInputEdgeAnalogEps;
    if (was_squeeze != now_squeeze) {
      char token[64];
      std::snprintf(token, sizeof(token), "%s.squeeze %s(%.2f)", hand,
                    now_squeeze ? "off->on" : "on->off", b.squeeze);
      emit(token);
    }
    const float was_mag =
        std::hypot(a.thumbstick[0], a.thumbstick[1]);
    const float now_mag =
        std::hypot(b.thumbstick[0], b.thumbstick[1]);
    const bool was_touch = was_mag > kInputEdgeStickThreshold;
    const bool now_touch = now_mag > kInputEdgeStickThreshold;
    if (was_touch != now_touch) {
      char token[96];
      std::snprintf(token, sizeof(token), "%s.stick %s(%+.2f,%+.2f)", hand,
                    now_touch ? "idle->touch" : "touch->idle", b.thumbstick[0], b.thumbstick[1]);
      emit(token);
    }
    if (a.gripValid != b.gripValid) {
      emit(std::string(hand) + ".grip " + (b.gripValid ? "invalid->valid" : "valid->invalid"));
    }
    if (a.aimValid != b.aimValid) {
      emit(std::string(hand) + ".aim " + (b.aimValid ? "invalid->valid" : "valid->invalid"));
    }
    if (a.connected != b.connected) {
      emit(std::string(hand) + ".connected " + (b.connected ? "no->yes" : "yes->no"));
    }
  }
  return out;
}

}  // namespace xrift::xr
