#include "xr/session.h"

#include "common/log.h"
#include "common/xr_result.h"

namespace xrift::xr {

Session::Session(const Instance& xr, const gfx::VulkanContext& vk) : instance_(xr.handle()) {
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

    // 今回使う参照空間は LOCAL の 1 つのみ。local-floor は将来対応する。
    XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space_info.poseInReferenceSpace.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    space_info.poseInReferenceSpace.position = {0.0f, 0.0f, 0.0f};
    CheckXr(xrCreateReferenceSpace(session_, &space_info, &space_), "xrCreateReferenceSpace(LOCAL)");

    views_ = xr.QueryStereoViews();
    Info("view 構成: %u 眼", static_cast<unsigned>(views_.size()));
  } catch (...) {
    Destroy();
    throw;
  }
}

Session::~Session() { Destroy(); }

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

}  // namespace xrift::xr
