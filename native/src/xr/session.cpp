#include "xr/session.h"

#include <stdexcept>

#include "common/log.h"
#include "common/xr_result.h"

namespace xrift::xr {

SpacePreference ParseSpacePreference(const std::string& name) {
  if (name == "auto") {
    return SpacePreference::Auto;
  }
  if (name == "local") {
    return SpacePreference::Local;
  }
  if (name == "stage") {
    return SpacePreference::Stage;
  }
  if (name == "local-floor") {
    return SpacePreference::LocalFloor;
  }
  throw std::runtime_error("--space は auto/local/stage/local-floor のいずれかであること（指定=" +
                           name + ")");
}

const char* SpacePreferenceName(SpacePreference pref) {
  switch (pref) {
    case SpacePreference::Auto:
      return "auto";
    case SpacePreference::Local:
      return "local";
    case SpacePreference::Stage:
      return "stage";
    case SpacePreference::LocalFloor:
      return "local-floor";
  }
  return "不明";
}

const char* SpaceTypeName(XrReferenceSpaceType type) {
  switch (type) {
    case XR_REFERENCE_SPACE_TYPE_VIEW:
      return "view";
    case XR_REFERENCE_SPACE_TYPE_LOCAL:
      return "local";
    case XR_REFERENCE_SPACE_TYPE_STAGE:
      return "stage";
    case XR_REFERENCE_SPACE_TYPE_UNBOUNDED_MSFT:
      return "unbounded";
    case XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO:
      return "combined-eye";
    case XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR:
      return "local-floor";
    default:
      return "不明";
  }
}

Session::Session(const Instance& xr, const gfx::VulkanContext& vk, SpacePreference pref)
    : instance_(xr.handle()), pref_(pref) {
  // コンストラクタが例外で抜けるとデストラクタは呼ばれないため、生成済みハンドルは
  // ここで破棄する。Destroy() は冪等であり、部分構築状態でも安全に呼べる。
  try {
    // OpenXR との境界は生ハンドルで行う。vk::raii からの変換は vulkan.hpp の仕様に従い、
    // operator* で取り出した C++ ハンドルを static_cast で C ハンドルに変換する。
    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    binding.instance = static_cast<VkInstance>(*vk.instance());
    binding.physicalDevice = static_cast<VkPhysicalDevice>(*vk.physicalDevice());
    binding.device = static_cast<VkDevice>(*vk.device());
    binding.queueFamilyIndex = vk.queueFamilyIndex();
    binding.queueIndex = 0;

    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    info.next = &binding;
    info.systemId = xr.systemId();
    CheckXr(xrCreateSession(xr.handle(), &info, &session_), "xrCreateSession");

    // 参照空間の選択。availability は xrEnumerateReferenceSpaces で確認する。
    // 要求が不可なら落とさず後退し、理由をログに出す（参照空間選択の仕様）。
    const std::vector<XrReferenceSpaceType> available = EnumerateSpaces();
    std::string available_names;
    for (XrReferenceSpaceType type : available) {
      if (!available_names.empty()) {
        available_names += ",";
      }
      available_names += SpaceTypeName(type);
    }
    Info("参照空間の一覧: %s（要求=%s）",
         available_names.empty() ? "(空)" : available_names.c_str(),
         SpacePreferenceName(pref_));
    auto has = [&](XrReferenceSpaceType type) {
      for (XrReferenceSpaceType t : available) {
        if (t == type) {
          return true;
        }
      }
      return false;
    };
    // 後退順は local-floor→stage→local である（DESIGN §10）。
    const XrReferenceSpaceType kFallback[] = {
        XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR,
        XR_REFERENCE_SPACE_TYPE_STAGE,
        XR_REFERENCE_SPACE_TYPE_LOCAL,
    };
    XrReferenceSpaceType chosen = XR_REFERENCE_SPACE_TYPE_LOCAL;
    bool chosen_explicit = false;
    if (pref_ == SpacePreference::LocalFloor || pref_ == SpacePreference::Stage ||
        pref_ == SpacePreference::Local) {
      const XrReferenceSpaceType want = pref_ == SpacePreference::LocalFloor
                                            ? XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR
                                            : (pref_ == SpacePreference::Stage
                                                   ? XR_REFERENCE_SPACE_TYPE_STAGE
                                                   : XR_REFERENCE_SPACE_TYPE_LOCAL);
      if (has(want)) {
        chosen = want;
        chosen_explicit = true;
      } else {
        Warn("要求の参照空間 %s は利用不可のため後退する", SpaceTypeName(want));
      }
    }
    if (!chosen_explicit) {
      for (XrReferenceSpaceType type : kFallback) {
        if (has(type)) {
          chosen = type;
          break;
        }
      }
      // 一覧が空・未知ばかりの場合は LOCAL に倒す（core 必須のため生成は試みる）。
      if (!has(chosen) && !available.empty()) {
        Warn("local-floor/stage/local が一覧に無いため %s で生成を試みる",
             SpaceTypeName(available[0]));
        chosen = available[0];
      }
    }
    if (!CreateSpace(chosen)) {
      // 選択種別の生成に失敗したら後退順で作り直す。LOCAL まで駄目なら例外にする。
      bool recovered = false;
      for (XrReferenceSpaceType type : kFallback) {
        if (type == chosen) {
          continue;
        }
        Warn("参照空間 %s の生成に失敗したため %s で作り直す", SpaceTypeName(chosen),
             SpaceTypeName(type));
        if (CreateSpace(type)) {
          chosen = type;
          recovered = true;
          break;
        }
      }
      if (!recovered) {
        throw std::runtime_error("参照空間の生成に失敗しました");
      }
    }
    space_type_ = chosen;
    Info("参照空間: %s（要求=%s）", SpaceTypeName(space_type_), SpacePreferenceName(pref_));

    views_ = xr.QueryStereoViews();
    Info("view 構成: %u 眼", static_cast<unsigned>(views_.size()));
  } catch (...) {
    Destroy();
    throw;
  }
}

Session::~Session() { Destroy(); }

bool Session::CreateSpace(XrReferenceSpaceType type) {
  XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  space_info.referenceSpaceType = type;
  space_info.poseInReferenceSpace.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
  space_info.poseInReferenceSpace.position = {0.0f, 0.0f, 0.0f};
  XrSpace space = XR_NULL_HANDLE;
  const XrResult result = xrCreateReferenceSpace(session_, &space_info, &space);
  if (result != XR_SUCCESS) {
    Warn("xrCreateReferenceSpace(%s) に失敗: %s", SpaceTypeName(type), XrResultToString(result));
    return false;
  }
  if (space_ != XR_NULL_HANDLE) {
    xrDestroySpace(space_);
  }
  space_ = space;
  return true;
}

std::vector<XrReferenceSpaceType> Session::EnumerateSpaces() const {
  uint32_t count = 0;
  const XrResult counted = xrEnumerateReferenceSpaces(session_, 0, &count, nullptr);
  if (counted != XR_SUCCESS) {
    Warn("xrEnumerateReferenceSpaces(件数) に失敗: %s", XrResultToString(counted));
    return {};
  }
  std::vector<XrReferenceSpaceType> spaces(count, XR_REFERENCE_SPACE_TYPE_VIEW);
  const XrResult listed = xrEnumerateReferenceSpaces(session_, count, &count, spaces.data());
  if (listed != XR_SUCCESS) {
    Warn("xrEnumerateReferenceSpaces(列挙) に失敗: %s", XrResultToString(listed));
    return {};
  }
  spaces.resize(count);
  return spaces;
}

void Session::Begin() {
  if (running_) {
    return;
  }
  XrSessionBeginInfo info{XR_TYPE_SESSION_BEGIN_INFO};
  info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  CheckXr(xrBeginSession(session_, &info), "xrBeginSession");
  running_ = true;
}

void Session::End() {
  if (!running_) {
    return;
  }
  CheckXr(xrEndSession(session_), "xrEndSession");
  running_ = false;
}

void Session::Destroy() {
  // 破棄は破損を広げない。失敗は警告に留める。
  // space_ は session_ より先に破棄する。session 自体は swapchain より後、
  // Vk device より前に破棄する必要があり、その順序は呼び出し側 (XrApp::Shutdown) が保つ。
  if (space_ != XR_NULL_HANDLE) {
    const XrResult result = xrDestroySpace(space_);
    if (result != XR_SUCCESS) {
      Warn("xrDestroySpace に失敗: %s", XrResultToString(result));
    }
    space_ = XR_NULL_HANDLE;
  }
  if (session_ != XR_NULL_HANDLE) {
    const XrResult result = xrDestroySession(session_);
    if (result != XR_SUCCESS) {
      Warn("xrDestroySession に失敗: %s", XrResultToString(result));
    }
    session_ = XR_NULL_HANDLE;
  }
  running_ = false;
}

bool Session::RecreateSpaceForPendingChange(XrTime displayTime) {
  if (session_ == XR_NULL_HANDLE) {
    return false;
  }
  // 同種別の空間を作り直し、新しい原点で locate できることを確認してから返す。
  // 呼び出し側は成功時に spaceEpoch を進め、旧世代のレコードを失効させる。
  // 失敗時は旧空間を保ち、世代は進めない。
  // 時刻 0 はランタイムに解決を委ねるが、受け付けないランタイム
  // （実測: SteamVR は XR_ERROR_TIME_INVALID を返す）では失敗する。
  // そのため呼び出し側は xrWaitFrame の予測表示時刻を渡すこと。
  XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  space_info.referenceSpaceType = space_type_;
  space_info.poseInReferenceSpace.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
  space_info.poseInReferenceSpace.position = {0.0f, 0.0f, 0.0f};
  XrSpace next = XR_NULL_HANDLE;
  const XrResult created = xrCreateReferenceSpace(session_, &space_info, &next);
  if (created != XR_SUCCESS) {
    Warn("参照空間の作り直しに失敗（%s）: %s。旧空間を保つ", SpaceTypeName(space_type_),
         XrResultToString(created));
    return false;
  }
  // 新しい原点を取得できることの確認。時刻は呼び出し側の予測表示時刻を使う。
  XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
  const XrResult located = xrLocateSpace(next, next, displayTime, &location);
  if (located != XR_SUCCESS) {
    Warn("作り直した参照空間の locate に失敗: %s。旧空間を保つ", XrResultToString(located));
    xrDestroySpace(next);
    return false;
  }
  xrDestroySpace(space_);
  space_ = next;
  Info("参照空間を作り直した（%s。新しい原点で locate 可）", SpaceTypeName(space_type_));
  return true;
}

}  // namespace xrift::xr
