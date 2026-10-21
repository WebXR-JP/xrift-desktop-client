#pragma once

#include <cstdint>
#include <string>

#include "common/xr_vulkan.h"

// 入力アクション（docs/architecture.md「入力と haptics」）。アクションセット `xrift` を作り、
// 左右のサブアクションパスで pose/float/vector2/bool/haptic を持つ。
// suggest 表は native/src/addon.cpp と同じ顔ぶれに PICO 系（bytedance）を足す。
// 取得・無効化・haptics 停止はここに残し、意味付け・UI・デッドゾーンは TS 側の責務である。

namespace xrift::xr {

// 1 項目の有効状態。「0」と「未対応」を区別するための 3 値である。
// Unsupported は profile が束縛を持たない、Inactive は束縛はあるが現在無効である。
enum class FieldState : uint8_t {
  Unsupported = 0,
  Inactive = 1,
  Active = 2,
};

struct InputButton {
  bool pressed = false;
  FieldState state = FieldState::Unsupported;
};

struct HandInput {
  bool connected = false;  // grip/aim のいずれかが locate できた。
  std::string profile;     // 現在の interaction profile。空は未取得・未装着。
  XrPosef grip{};
  XrPosef aim{};
  bool gripValid = false;
  bool aimValid = false;
  float trigger = 0.0f;
  float squeeze = 0.0f;
  FieldState triggerState = FieldState::Unsupported;
  FieldState squeezeState = FieldState::Unsupported;
  float thumbstick[2] = {};
  float trackpad[2] = {};
  FieldState thumbstickState = FieldState::Unsupported;
  FieldState trackpadState = FieldState::Unsupported;
  InputButton thumbstickClick;
  InputButton trackpadClick;
  InputButton primary;
  InputButton secondary;
  InputButton menu;
};

struct InputSnapshot {
  HandInput hands[2];
  bool focused = false;  // xrSyncActions を行ったか（FOCUSED 相当の取得か）。
  uint64_t frameNo = 0;
};

struct InputStats {
  uint64_t syncs = 0;           // xrSyncActions を呼んだ回数。
  uint64_t syncFailed = 0;      // xrSyncActions が失敗した回数。
  uint64_t snapshots = 0;       // Snapshot を取った回数。
  uint64_t hapticOk = 0;        // xrApplyHapticFeedback が成功した回数。
  uint64_t hapticInactive = 0;  // 非 FOCUSED・未接続のため送らなかった回数。
  uint64_t hapticFailed = 0;    // 呼び出しが失敗した回数（例外にはしない）。
  uint64_t profileChanges = 0;  // 検出した profile 変化の回数。
};

// 押下エッジ検出の閾値。短い押下の取りこぼし診断用であり、製品の判定には使わない。
// trigger/squeeze は 0 と正の出入り、stick は絶対値 0.2 超の出入りを見る。
constexpr float kInputEdgeAnalogEps = 0.0005f;  // 0 判定の不感帯（取得ノイズの跳ね抑え）。
constexpr float kInputEdgeStickThreshold = 0.2f;  // stick 有効域の境目（仕様値）。

// 入力アクションの一式。XrInstance に束縛される（action set・action）。
// grip/aim の空間だけが XrSession に束縛されるため、セッション再生成時は
// DetachSpaces→AttachSpaces で作り直す（action 自体は作り直さない）。
class InputManager {
 public:
  InputManager();
  ~InputManager();

  InputManager(const InputManager&) = delete;
  InputManager& operator=(const InputManager&) = delete;

  // アクションセット・アクションの生成と profile 別の suggest を行う。
  // suggest の成否は profile ごとにログに出し、1 件の失敗で全体を止めない。
  void Create(XrInstance instance);
  // セッションへの attach と grip/aim 空間の生成。成功すると true。
  // 失敗時は理由をログに出して false を返す（例外にしない）。
  bool AttachSpaces(XrSession session);
  // セッション破棄に先立つ空間の破棄。冪等である。
  void DetachSpaces();
  bool hasActionSet() const { return action_set_ != XR_NULL_HANDLE; }
  const InputStats& stats() const { return stats_; }

  // FOCUSED 相当の取得が可能か。session_running_ ではなく呼び出し側の実行状態で決める。
  // focused が真のときだけ xrSyncActions を呼び、偽のときは inactive の Snapshot を返す。
  bool Sync(XrSession session, bool focused);
  // 現在の入力を取得する。Sync の直後に呼ぶこと。例外を投げない（失敗は inactive 側に倒す）。
  // focused が偽のときは全項目を Inactive（profile と接続は分かる範囲で埋める）。
  InputSnapshot Snapshot(XrSession session, XrSpace appSpace, XrTime displayTime,
                         bool focused, uint64_t frameNo);

  // haptics の適用。非 FOCUSED・未接続時は送らず inactive に数える。
  // 失敗は統計に残し、例外で呼び出し側のループを止めない。
  // 戻り値は true=送信、false=未送信（inactive・失敗のいずれか）。
  // 実際に呼び出したか・結果は lastHapticCalled/lastHapticResult で残る
  // （実機で「送ったが振動不明」の切り分け用。VkResult ではなく XrResult である）。
  bool ApplyHaptic(XrSession session, uint32_t hand, float amplitude, uint32_t durationMs,
                    bool focused);
  XrResult lastHapticResult() const { return last_haptic_result_; }
  bool lastHapticCalled() const { return last_haptic_called_; }

  // 毎秒ログ用の 1 行要約。押下中のボタンは名前で並べる（無ければ `-`）。
  static std::string Summarize(const InputSnapshot& snapshot);
  // 毎秒サマリの拡張版。Summarize に「その 1 秒間に押されたボタンの集合
  // （ラッチ）」を足す。短い押下の取りこぼし診断用であり、呼び出し側が
  // 1 秒ごとにラッチを 0 へ戻す（戻し忘れると押下済みのまま残る）。
  static std::string SummarizeWithLatch(const InputSnapshot& snapshot, uint8_t latch0,
                                        uint8_t latch1);
  // 前フレームとの差分を 1 行にする。変化が無ければ空を返す（ログを出さない）。
  // 見るのはボタン bit（押下中かつ Active の変化。InputState の buttons 欄と同じ）、
  // trigger/squeeze の 0/正の出入り、stick の絶対値 0.2 超の出入り、
  // pose 有効/無効の変化である。状態のみの遷移（Active→Inactive 等）は見ない。
  static std::string DescribeEdges(const InputSnapshot& prev, const InputSnapshot& cur);
  // 現在押下中のボタン bit（InputState の buttons 欄と同じ割付け）。
  static uint8_t PressedBits(const HandInput& hand);
  // ラッチ bit の人名（空は `-`）。
  static std::string FormatLatch(uint8_t bits);

 private:
  bool QueryProfile(XrSession session, int hand, std::string* out);
  float QueryFloat(XrSession session, XrAction action, XrPath sub, FieldState* state, bool focused,
                   uint8_t supported);
  void QueryVec2(XrSession session, XrAction action, XrPath sub, float out[2], FieldState* state,
                 bool focused, uint8_t supported);
  bool QueryBool(XrSession session, XrAction action, XrPath sub, InputButton* out, bool focused,
                 uint8_t supported);
  // 現在の profile に対する束縛の有無。suggest 表と一致させること。
  // profileId は transfer::ProfileIdFromName の値域（0=未取得、255=未知）を使う。
  uint8_t SupportMask(uint8_t profileId, XrAction action) const;

  XrInstance instance_ = XR_NULL_HANDLE;
  XrActionSet action_set_ = XR_NULL_HANDLE;
  XrPath hand_paths_[2] = {XR_NULL_PATH, XR_NULL_PATH};
  XrAction grip_pose_ = XR_NULL_HANDLE;
  XrAction aim_pose_ = XR_NULL_HANDLE;
  XrAction trigger_ = XR_NULL_HANDLE;
  XrAction squeeze_ = XR_NULL_HANDLE;
  XrAction thumbstick_ = XR_NULL_HANDLE;
  XrAction thumbstick_click_ = XR_NULL_HANDLE;
  XrAction trackpad_ = XR_NULL_HANDLE;
  XrAction trackpad_click_ = XR_NULL_HANDLE;
  XrAction primary_ = XR_NULL_HANDLE;
  XrAction secondary_ = XR_NULL_HANDLE;
  XrAction menu_ = XR_NULL_HANDLE;
  XrAction haptic_ = XR_NULL_HANDLE;
  XrSpace grip_spaces_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  XrSpace aim_spaces_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  std::string last_profiles_[2];
  InputStats stats_{};
  XrResult last_haptic_result_ = XR_SUCCESS;  // 最後に呼んだ xrApplyHapticFeedback の結果。
  bool last_haptic_called_ = false;           // 最後の ApplyHaptic で実際に呼び出したか。
};

}  // namespace xrift::xr
