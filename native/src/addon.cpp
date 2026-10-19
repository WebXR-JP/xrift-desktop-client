// XRift OpenXR backend — Node.js native addon
//
// OpenXR (D3D11 graphics binding) のインスタンス / セッション / スワップチェーン / フレームループ /
// コントローラ入力を JS に公開する。描画は JS 側（Dawn WebGPU + three.js）で行い、
// 読み戻した RGBA/BGRA ピクセルを submit() で各眼のスワップチェーンに書き込む。
//
// JS からの呼び出し順（1 フレーム）:
//   pollEvents() → await waitFrame() → beginFrame() → locateViews() → syncInput()
//   → (描画・読み戻し) → submit(0, ...) / submit(1, ...) → endFrame()

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <napi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string XrResultString(XrInstance instance, XrResult result) {
  char buffer[XR_MAX_RESULT_STRING_SIZE] = {};
  if (instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(instance, result, buffer))) {
    return buffer;
  }
  return "XrResult(" + std::to_string(static_cast<int>(result)) + ")";
}

const char* SessionStateName(XrSessionState s) {
  switch (s) {
    case XR_SESSION_STATE_IDLE: return "idle";
    case XR_SESSION_STATE_READY: return "ready";
    case XR_SESSION_STATE_SYNCHRONIZED: return "synchronized";
    case XR_SESSION_STATE_VISIBLE: return "visible";
    case XR_SESSION_STATE_FOCUSED: return "focused";
    case XR_SESSION_STATE_STOPPING: return "stopping";
    case XR_SESSION_STATE_LOSS_PENDING: return "loss_pending";
    case XR_SESSION_STATE_EXITING: return "exiting";
    default: return "unknown";
  }
}

template <typename T>
void SafeRelease(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

struct EyeSwapchain {
  XrSwapchain handle = XR_NULL_HANDLE;
  ID3D11Texture2D* staging[2] = {nullptr, nullptr};  // CPU 書き込み用（交互に使う）
  int32_t width = 0;
  int32_t height = 0;
  std::vector<XrSwapchainImageD3D11KHR> images;
  bool submitted = false;
};

struct HandActions {
  XrPath path = XR_NULL_PATH;
  XrSpace gripSpace = XR_NULL_HANDLE;
  XrSpace aimSpace = XR_NULL_HANDLE;
};

}  // namespace

class XrSessionWrap : public Napi::ObjectWrap<XrSessionWrap> {
 public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports) {
    Napi::Function func = DefineClass(env, "OpenXRSession", {
      InstanceMethod("pollEvents", &XrSessionWrap::PollEvents),
      InstanceMethod("waitFrame", &XrSessionWrap::WaitFrame),
      InstanceMethod("beginFrame", &XrSessionWrap::BeginFrame),
      InstanceMethod("locateViews", &XrSessionWrap::LocateViews),
      InstanceMethod("syncInput", &XrSessionWrap::SyncInput),
      InstanceMethod("submit", &XrSessionWrap::Submit),
      InstanceMethod("submitStereoAsync", &XrSessionWrap::SubmitStereoAsync),
      InstanceMethod("submitLatched", &XrSessionWrap::SubmitLatched),
      InstanceMethod("endFrame", &XrSessionWrap::EndFrame),
      InstanceMethod("applyHaptics", &XrSessionWrap::ApplyHaptics),
      InstanceMethod("requestExit", &XrSessionWrap::RequestExit),
      InstanceMethod("destroy", &XrSessionWrap::Destroy),
      InstanceMethod("getInfo", &XrSessionWrap::GetInfo),
    });
    auto* constructor = new Napi::FunctionReference();
    *constructor = Napi::Persistent(func);
    env.SetInstanceData(constructor);
    exports.Set("OpenXRSession", func);
    return exports;
  }

  XrSessionWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<XrSessionWrap>(info) {
    Napi::Env env = info.Env();
    std::string appName = "XRift OpenXR Client";
    double scale = 1.0;
    std::string refSpace = "local-floor";
    if (info.Length() > 0 && info[0].IsObject()) {
      auto opts = info[0].As<Napi::Object>();
      if (opts.Has("appName")) appName = opts.Get("appName").ToString().Utf8Value();
      if (opts.Has("resolutionScale")) scale = opts.Get("resolutionScale").ToNumber().DoubleValue();
      if (opts.Has("referenceSpace")) refSpace = opts.Get("referenceSpace").ToString().Utf8Value();
    }
    try {
      InitInstance(appName);
      InitDevice();
      InitSession();
      InitSpaces(refSpace);
      InitSwapchains(scale);
      InitActions();
    } catch (const std::exception& e) {
      Cleanup();
      throw Napi::Error::New(env, std::string("OpenXR init failed: ") + e.what());
    }
  }

  ~XrSessionWrap() override { Cleanup(); }

 private:
  // ------------------------------------------------------------------ 初期化

  void Check(XrResult r, const char* what) {
    if (XR_FAILED(r)) {
      throw std::runtime_error(std::string(what) + " -> " + XrResultString(instance_, r));
    }
  }

  void InitInstance(const std::string& appName) {
    uint32_t extCount = 0;
    Check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr), "xrEnumerateInstanceExtensionProperties");
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    Check(xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data()), "xrEnumerateInstanceExtensionProperties");

    bool hasD3D11 = false;
    for (auto& e : exts) {
      if (strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) hasD3D11 = true;
      if (strcmp(e.extensionName, XR_EXT_LOCAL_FLOOR_EXTENSION_NAME) == 0) hasLocalFloorExt_ = true;
    }
    if (!hasD3D11) throw std::runtime_error("runtime does not support XR_KHR_D3D11_enable");

    std::vector<const char*> enabled = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    if (hasLocalFloorExt_) enabled.push_back(XR_EXT_LOCAL_FLOOR_EXTENSION_NAME);

    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy_s(ci.applicationInfo.applicationName, appName.c_str(), XR_MAX_APPLICATION_NAME_SIZE - 1);
    ci.applicationInfo.applicationVersion = 1;
    strncpy_s(ci.applicationInfo.engineName, "three.js (WebGPU/Dawn)", XR_MAX_ENGINE_NAME_SIZE - 1);
    ci.applicationInfo.engineVersion = 186;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    ci.enabledExtensionNames = enabled.data();
    Check(xrCreateInstance(&ci, &instance_), "xrCreateInstance");

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    Check(xrGetInstanceProperties(instance_, &ip), "xrGetInstanceProperties");
    runtimeName_ = ip.runtimeName;

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    Check(xrGetSystem(instance_, &sgi, &systemId_), "xrGetSystem (HMD が接続され SteamVR が起動していますか?)");

    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    Check(xrGetSystemProperties(instance_, systemId_, &sp), "xrGetSystemProperties");
    systemName_ = sp.systemName;
  }

  void InitDevice() {
    PFN_xrGetD3D11GraphicsRequirementsKHR getReq = nullptr;
    Check(xrGetInstanceProcAddr(instance_, "xrGetD3D11GraphicsRequirementsKHR",
                                reinterpret_cast<PFN_xrVoidFunction*>(&getReq)),
          "xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)");
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    Check(getReq(instance_, systemId_, &req), "xrGetD3D11GraphicsRequirementsKHR");

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
      throw std::runtime_error("CreateDXGIFactory1 failed");
    }
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0;; ++i) {
      IDXGIAdapter1* a = nullptr;
      if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_ADAPTER_DESC1 desc;
      a->GetDesc1(&desc);
      if (memcmp(&desc.AdapterLuid, &req.adapterLuid, sizeof(LUID)) == 0) {
        adapter = a;
        char name[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
        adapterName_ = name;
        break;
      }
      a->Release();
    }
    factory->Release();

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
                                  D3D_FEATURE_LEVEL_11_0};
    UINT levelCount = 0;
    for (auto l : levels) {
      if (l >= req.minFeatureLevel) ++levelCount;
    }
    HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, levelCount ? levelCount : 1,
                                   D3D11_SDK_VERSION, &device_, nullptr, &context_);
    SafeRelease(adapter);
    if (FAILED(hr)) throw std::runtime_error("D3D11CreateDevice failed");
    // SteamVR は WaitGetPoses / Submit の中でこのデバイスの即時コンテキストを使う。
    // こちらの転送スレッドと同時に触っても壊れないよう、コンテキストの排他を有効にする
    {
      ID3D11Multithread* mt = nullptr;
      if (SUCCEEDED(context_->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&mt)))) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
      }
    }
  }

  void InitSession() {
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device_;
    XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO};
    ci.next = &binding;
    ci.systemId = systemId_;
    Check(xrCreateSession(instance_, &ci, &session_), "xrCreateSession");
  }

  void InitSpaces(const std::string& requested) {
    uint32_t count = 0;
    Check(xrEnumerateReferenceSpaces(session_, 0, &count, nullptr), "xrEnumerateReferenceSpaces");
    std::vector<XrReferenceSpaceType> types(count);
    Check(xrEnumerateReferenceSpaces(session_, count, &count, types.data()), "xrEnumerateReferenceSpaces");
    auto has = [&](XrReferenceSpaceType t) { return std::find(types.begin(), types.end(), t) != types.end(); };

    XrReferenceSpaceType type = XR_REFERENCE_SPACE_TYPE_LOCAL;
    referenceSpaceName_ = "local";
    if (requested == "local-floor" || requested == "bounded-floor" || requested == "stage") {
      if (requested == "local-floor" && hasLocalFloorExt_ && has(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT)) {
        type = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT;
        referenceSpaceName_ = "local-floor";
      } else if (has(XR_REFERENCE_SPACE_TYPE_STAGE)) {
        type = XR_REFERENCE_SPACE_TYPE_STAGE;
        referenceSpaceName_ = "stage";
      }
    }

    XrReferenceSpaceCreateInfo ci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    ci.poseInReferenceSpace.orientation.w = 1.0f;
    ci.referenceSpaceType = type;
    Check(xrCreateReferenceSpace(session_, &ci, &appSpace_), "xrCreateReferenceSpace(app)");
    ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    Check(xrCreateReferenceSpace(session_, &ci, &viewSpace_), "xrCreateReferenceSpace(view)");
  }

  void InitSwapchains(double scale) {
    uint32_t count = 0;
    Check(xrEnumerateViewConfigurationViews(instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count,
                                            nullptr),
          "xrEnumerateViewConfigurationViews");
    viewConfigs_.assign(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    Check(xrEnumerateViewConfigurationViews(instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count,
                                            &count, viewConfigs_.data()),
          "xrEnumerateViewConfigurationViews");
    if (count != 2) throw std::runtime_error("expected 2 stereo views");
    views_.assign(count, {XR_TYPE_VIEW});

    // フォーマット: JS 側は sRGB エンコード済みの 8bit 値を書くので *_SRGB を選ぶ（コンポジタがデコードする）
    uint32_t fmtCount = 0;
    Check(xrEnumerateSwapchainFormats(session_, 0, &fmtCount, nullptr), "xrEnumerateSwapchainFormats");
    std::vector<int64_t> formats(fmtCount);
    Check(xrEnumerateSwapchainFormats(session_, fmtCount, &fmtCount, formats.data()), "xrEnumerateSwapchainFormats");
    const int64_t preferred[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                 DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};
    colorFormat_ = 0;
    for (int64_t p : preferred) {
      if (std::find(formats.begin(), formats.end(), p) != formats.end()) {
        colorFormat_ = p;
        break;
      }
    }
    if (!colorFormat_) throw std::runtime_error("no supported 8bit RGBA/BGRA swapchain format");
    isBGRA_ = colorFormat_ == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || colorFormat_ == DXGI_FORMAT_B8G8R8A8_UNORM;

    // 両眼同じサイズ。幅は 64 の倍数（= 1 行 256 バイト境界、WebGPU の copyTextureToBuffer 要件）
    int32_t w = 0, h = 0, maxW = INT32_MAX, maxH = INT32_MAX;
    for (auto& v : viewConfigs_) {
      w = std::max<int32_t>(w, static_cast<int32_t>(std::lround(v.recommendedImageRectWidth * scale)));
      h = std::max<int32_t>(h, static_cast<int32_t>(std::lround(v.recommendedImageRectHeight * scale)));
      maxW = std::min<int32_t>(maxW, static_cast<int32_t>(v.maxImageRectWidth));
      maxH = std::min<int32_t>(maxH, static_cast<int32_t>(v.maxImageRectHeight));
    }
    w = std::min(((w + 63) / 64) * 64, (maxW / 64) * 64);
    h = std::min(std::max(h, 64), maxH);

    for (int eye = 0; eye < 2; ++eye) {
      XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
      ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
      ci.format = colorFormat_;
      ci.sampleCount = 1;
      ci.width = w;
      ci.height = h;
      ci.faceCount = 1;
      ci.arraySize = 1;
      ci.mipCount = 1;
      auto& sc = eyes_[eye];
      Check(xrCreateSwapchain(session_, &ci, &sc.handle), "xrCreateSwapchain");
      sc.width = w;
      sc.height = h;
      uint32_t imgCount = 0;
      Check(xrEnumerateSwapchainImages(sc.handle, 0, &imgCount, nullptr), "xrEnumerateSwapchainImages");
      sc.images.assign(imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
      Check(xrEnumerateSwapchainImages(sc.handle, imgCount, &imgCount,
                                       reinterpret_cast<XrSwapchainImageBaseHeader*>(sc.images.data())),
            "xrEnumerateSwapchainImages");

      D3D11_TEXTURE2D_DESC td{};
      td.Width = w;
      td.Height = h;
      td.MipLevels = 1;
      td.ArraySize = 1;
      td.Format = static_cast<DXGI_FORMAT>(colorFormat_);
      td.SampleDesc.Count = 1;
      td.Usage = D3D11_USAGE_STAGING;
      td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      for (auto& st : sc.staging) {
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &st))) throw std::runtime_error("CreateTexture2D(staging) failed");
      }
    }
  }

  XrPath Path(const char* s) {
    XrPath p = XR_NULL_PATH;
    Check(xrStringToPath(instance_, s, &p), s);
    return p;
  }

  XrAction CreateAction(const char* name, const char* localized, XrActionType type) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    strcpy_s(ci.actionName, name);
    strcpy_s(ci.localizedActionName, localized);
    ci.actionType = type;
    XrPath subs[2] = {hands_[0].path, hands_[1].path};
    ci.countSubactionPaths = 2;
    ci.subactionPaths = subs;
    XrAction a = XR_NULL_HANDLE;
    Check(xrCreateAction(actionSet_, &ci, &a), name);
    return a;
  }

  void Suggest(const char* profile, const std::vector<std::pair<XrAction, const char*>>& bindings) {
    std::vector<XrActionSuggestedBinding> b;
    for (auto& [action, path] : bindings) b.push_back({action, Path(path)});
    XrInteractionProfileSuggestedBinding s{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    s.interactionProfile = Path(profile);
    s.suggestedBindings = b.data();
    s.countSuggestedBindings = static_cast<uint32_t>(b.size());
    XrResult r = xrSuggestInteractionProfileBindings(instance_, &s);
    if (XR_FAILED(r)) {
      // 1 つのプロファイルが失敗しても他は使えるので続行
      fprintf(stderr, "[openxr] suggest bindings failed for %s: %s\n", profile, XrResultString(instance_, r).c_str());
    }
  }

  void InitActions() {
    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(asci.actionSetName, "xrift");
    strcpy_s(asci.localizedActionSetName, "XRift");
    Check(xrCreateActionSet(instance_, &asci, &actionSet_), "xrCreateActionSet");

    hands_[0].path = Path("/user/hand/left");
    hands_[1].path = Path("/user/hand/right");

    gripPose_ = CreateAction("grip_pose", "Grip Pose", XR_ACTION_TYPE_POSE_INPUT);
    aimPose_ = CreateAction("aim_pose", "Aim Pose", XR_ACTION_TYPE_POSE_INPUT);
    trigger_ = CreateAction("trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT);
    squeeze_ = CreateAction("squeeze", "Squeeze", XR_ACTION_TYPE_FLOAT_INPUT);
    thumbstick_ = CreateAction("thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT);
    thumbstickClick_ = CreateAction("thumbstick_click", "Thumbstick Click", XR_ACTION_TYPE_BOOLEAN_INPUT);
    trackpad_ = CreateAction("trackpad", "Trackpad", XR_ACTION_TYPE_VECTOR2F_INPUT);
    trackpadClick_ = CreateAction("trackpad_click", "Trackpad Click", XR_ACTION_TYPE_BOOLEAN_INPUT);
    primary_ = CreateAction("primary_button", "A / X Button", XR_ACTION_TYPE_BOOLEAN_INPUT);
    secondary_ = CreateAction("secondary_button", "B / Y Button", XR_ACTION_TYPE_BOOLEAN_INPUT);
    menu_ = CreateAction("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
    haptic_ = CreateAction("haptic", "Haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT);

    const char* L = "/user/hand/left";
    const char* R = "/user/hand/right";
    auto p = [](const char* hand, const char* sub) { return std::string(hand) + sub; };
    std::vector<std::string> keep;  // パス文字列の寿命を保つ
    keep.reserve(128);
    auto bind = [&](XrAction a, const char* hand, const char* sub) {
      keep.push_back(p(hand, sub));
      return std::pair<XrAction, const char*>(a, keep.back().c_str());
    };
    auto common = [&](std::vector<std::pair<XrAction, const char*>>& v) {
      for (const char* h : {L, R}) {
        v.push_back(bind(gripPose_, h, "/input/grip/pose"));
        v.push_back(bind(aimPose_, h, "/input/aim/pose"));
        v.push_back(bind(haptic_, h, "/output/haptic"));
      }
    };

    {  // Khronos simple controller
      std::vector<std::pair<XrAction, const char*>> v;
      common(v);
      for (const char* h : {L, R}) {
        v.push_back(bind(trigger_, h, "/input/select/click"));
        v.push_back(bind(menu_, h, "/input/menu/click"));
      }
      Suggest("/interaction_profiles/khr/simple_controller", v);
    }
    {  // Oculus Touch
      std::vector<std::pair<XrAction, const char*>> v;
      common(v);
      for (const char* h : {L, R}) {
        v.push_back(bind(trigger_, h, "/input/trigger/value"));
        v.push_back(bind(squeeze_, h, "/input/squeeze/value"));
        v.push_back(bind(thumbstick_, h, "/input/thumbstick"));
        v.push_back(bind(thumbstickClick_, h, "/input/thumbstick/click"));
      }
      v.push_back(bind(primary_, L, "/input/x/click"));
      v.push_back(bind(secondary_, L, "/input/y/click"));
      v.push_back(bind(menu_, L, "/input/menu/click"));
      v.push_back(bind(primary_, R, "/input/a/click"));
      v.push_back(bind(secondary_, R, "/input/b/click"));
      Suggest("/interaction_profiles/oculus/touch_controller", v);
    }
    {  // Valve Index
      std::vector<std::pair<XrAction, const char*>> v;
      common(v);
      for (const char* h : {L, R}) {
        v.push_back(bind(trigger_, h, "/input/trigger/value"));
        v.push_back(bind(squeeze_, h, "/input/squeeze/value"));
        v.push_back(bind(thumbstick_, h, "/input/thumbstick"));
        v.push_back(bind(thumbstickClick_, h, "/input/thumbstick/click"));
        v.push_back(bind(trackpad_, h, "/input/trackpad"));
        v.push_back(bind(primary_, h, "/input/a/click"));
        v.push_back(bind(secondary_, h, "/input/b/click"));
      }
      Suggest("/interaction_profiles/valve/index_controller", v);
    }
    {  // HTC Vive
      std::vector<std::pair<XrAction, const char*>> v;
      common(v);
      for (const char* h : {L, R}) {
        v.push_back(bind(trigger_, h, "/input/trigger/value"));
        v.push_back(bind(squeeze_, h, "/input/squeeze/click"));
        v.push_back(bind(trackpad_, h, "/input/trackpad"));
        v.push_back(bind(trackpadClick_, h, "/input/trackpad/click"));
        v.push_back(bind(menu_, h, "/input/menu/click"));
      }
      Suggest("/interaction_profiles/htc/vive_controller", v);
    }
    {  // Windows Mixed Reality
      std::vector<std::pair<XrAction, const char*>> v;
      common(v);
      for (const char* h : {L, R}) {
        v.push_back(bind(trigger_, h, "/input/trigger/value"));
        v.push_back(bind(squeeze_, h, "/input/squeeze/click"));
        v.push_back(bind(thumbstick_, h, "/input/thumbstick"));
        v.push_back(bind(thumbstickClick_, h, "/input/thumbstick/click"));
        v.push_back(bind(trackpad_, h, "/input/trackpad"));
        v.push_back(bind(trackpadClick_, h, "/input/trackpad/click"));
        v.push_back(bind(menu_, h, "/input/menu/click"));
      }
      Suggest("/interaction_profiles/microsoft/motion_controller", v);
    }

    for (int i = 0; i < 2; ++i) {
      XrActionSpaceCreateInfo sci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
      sci.poseInActionSpace.orientation.w = 1.0f;
      sci.subactionPath = hands_[i].path;
      sci.action = gripPose_;
      Check(xrCreateActionSpace(session_, &sci, &hands_[i].gripSpace), "xrCreateActionSpace(grip)");
      sci.action = aimPose_;
      Check(xrCreateActionSpace(session_, &sci, &hands_[i].aimSpace), "xrCreateActionSpace(aim)");
    }

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &actionSet_;
    Check(xrAttachSessionActionSets(session_, &attach), "xrAttachSessionActionSets");
  }

  void Cleanup() {
    for (auto& h : hands_) {
      if (h.gripSpace) xrDestroySpace(h.gripSpace);
      if (h.aimSpace) xrDestroySpace(h.aimSpace);
      h.gripSpace = h.aimSpace = XR_NULL_HANDLE;
    }
    if (actionSet_) xrDestroyActionSet(actionSet_);
    actionSet_ = XR_NULL_HANDLE;
    for (auto& e : eyes_) {
      for (auto& st : e.staging) SafeRelease(st);
      if (e.handle) xrDestroySwapchain(e.handle);
      e.handle = XR_NULL_HANDLE;
      e.images.clear();
    }
    if (appSpace_) xrDestroySpace(appSpace_);
    if (viewSpace_) xrDestroySpace(viewSpace_);
    appSpace_ = viewSpace_ = XR_NULL_HANDLE;
    if (session_) xrDestroySession(session_);
    session_ = XR_NULL_HANDLE;
    if (instance_) xrDestroyInstance(instance_);
    instance_ = XR_NULL_HANDLE;
    SafeRelease(context_);
    SafeRelease(device_);
  }

  void EnsureAlive(Napi::Env env) {
    if (!session_) throw Napi::Error::New(env, "OpenXR session destroyed");
  }

  // ------------------------------------------------------------------ JS API

  Napi::Value GetInfo(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto o = Napi::Object::New(env);
    o.Set("runtimeName", runtimeName_);
    o.Set("systemName", systemName_);
    o.Set("adapterName", adapterName_);
    o.Set("referenceSpace", referenceSpaceName_);
    o.Set("width", eyes_[0].width);
    o.Set("height", eyes_[0].height);
    o.Set("colorFormat", isBGRA_ ? "bgra8unorm" : "rgba8unorm");
    o.Set("srgbSwapchain", colorFormat_ == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                               colorFormat_ == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    auto rec = Napi::Array::New(env, viewConfigs_.size());
    for (uint32_t i = 0; i < viewConfigs_.size(); ++i) {
      auto r = Napi::Object::New(env);
      r.Set("width", viewConfigs_[i].recommendedImageRectWidth);
      r.Set("height", viewConfigs_[i].recommendedImageRectHeight);
      rec.Set(i, r);
    }
    o.Set("recommended", rec);
    return o;
  }

  // セッション状態遷移も内部で処理する（READY → begin, STOPPING → end）
  Napi::Value PollEvents(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto events = Napi::Array::New(env);
    if (!instance_) return events;
    uint32_t n = 0;
    auto push = [&](const char* type, const char* detail) {
      auto e = Napi::Object::New(env);
      e.Set("type", type);
      if (detail) e.Set("detail", detail);
      events.Set(n++, e);
    };
    for (;;) {
      XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
      XrResult r = xrPollEvent(instance_, &ev);
      if (r != XR_SUCCESS) break;
      switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
          auto* sc = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
          state_ = sc->state;
          push("state", SessionStateName(state_));
          if (state_ == XR_SESSION_STATE_READY) {
            XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
            bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            XrResult br = xrBeginSession(session_, &bi);
            running_ = XR_SUCCEEDED(br);
            push(running_ ? "begin" : "error", running_ ? nullptr : "xrBeginSession failed");
          } else if (state_ == XR_SESSION_STATE_STOPPING) {
            xrEndSession(session_);
            running_ = false;
            push("end", nullptr);
          } else if (state_ == XR_SESSION_STATE_EXITING || state_ == XR_SESSION_STATE_LOSS_PENDING) {
            exiting_ = true;
            push("exit", SessionStateName(state_));
          }
          break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
          exiting_ = true;
          push("exit", "instance_loss_pending");
          break;
        case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
          push("profiles", nullptr);
          break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
          push("reset", nullptr);
          break;
        default:
          break;
      }
    }
    return events;
  }

  class WaitFrameWorker : public Napi::AsyncWorker {
   public:
    WaitFrameWorker(Napi::Env env, XrSessionWrap* self)
        : Napi::AsyncWorker(env), self_(self), deferred_(Napi::Promise::Deferred::New(env)) {}
    Napi::Promise Promise() { return deferred_.Promise(); }
    void Execute() override {
      XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
      state_ = {XR_TYPE_FRAME_STATE};
      result_ = xrWaitFrame(self_->session_, &wi, &state_);
    }
    void OnOK() override {
      Napi::Env env = Env();
      if (XR_FAILED(result_)) {
        deferred_.Reject(Napi::Error::New(env, "xrWaitFrame -> " + XrResultString(self_->instance_, result_)).Value());
        return;
      }
      self_->frameState_ = state_;
      auto o = Napi::Object::New(env);
      o.Set("predictedDisplayTime", Napi::BigInt::New(env, static_cast<int64_t>(state_.predictedDisplayTime)));
      o.Set("predictedDisplayPeriodMs", static_cast<double>(state_.predictedDisplayPeriod) / 1e6);
      o.Set("shouldRender", state_.shouldRender == XR_TRUE);
      deferred_.Resolve(o);
    }
    void OnError(const Napi::Error& e) override { deferred_.Reject(e.Value()); }

   private:
    XrSessionWrap* self_;
    Napi::Promise::Deferred deferred_;
    XrFrameState state_{XR_TYPE_FRAME_STATE};
    XrResult result_ = XR_SUCCESS;
  };

  Napi::Value WaitFrame(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    EnsureAlive(env);
    if (!running_) throw Napi::Error::New(env, "session is not running");
    auto* w = new WaitFrameWorker(env, this);
    auto promise = w->Promise();
    w->Queue();
    return promise;
  }

  Napi::Value BeginFrame(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    EnsureAlive(env);
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    XrResult r = xrBeginFrame(session_, &bi);
    if (XR_FAILED(r)) throw Napi::Error::New(env, "xrBeginFrame -> " + XrResultString(instance_, r));
    viewsValid_ = false;
    return env.Undefined();
  }

  static void SetPose(Napi::Env env, Napi::Object o, const XrPosef& p) {
    auto pos = Napi::Float64Array::New(env, 3);
    pos[0] = p.position.x;
    pos[1] = p.position.y;
    pos[2] = p.position.z;
    auto ori = Napi::Float64Array::New(env, 4);
    ori[0] = p.orientation.x;
    ori[1] = p.orientation.y;
    ori[2] = p.orientation.z;
    ori[3] = p.orientation.w;
    o.Set("position", pos);
    o.Set("orientation", ori);
  }

  Napi::Value LocateViews(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    EnsureAlive(env);
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = frameState_.predictedDisplayTime;
    li.space = appSpace_;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t count = 0;
    XrResult r = xrLocateViews(session_, &li, &vs, static_cast<uint32_t>(views_.size()), &count, views_.data());
    if (XR_FAILED(r)) throw Napi::Error::New(env, "xrLocateViews -> " + XrResultString(instance_, r));
    viewsValid_ = (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                  (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);

    auto out = Napi::Object::New(env);
    out.Set("valid", viewsValid_);
    out.Set("positionTracked", (vs.viewStateFlags & XR_VIEW_STATE_POSITION_TRACKED_BIT) != 0);
    auto arr = Napi::Array::New(env, count);
    for (uint32_t i = 0; i < count; ++i) {
      auto v = Napi::Object::New(env);
      SetPose(env, v, views_[i].pose);
      auto fov = Napi::Float64Array::New(env, 4);
      fov[0] = views_[i].fov.angleLeft;
      fov[1] = views_[i].fov.angleRight;
      fov[2] = views_[i].fov.angleUp;
      fov[3] = views_[i].fov.angleDown;
      v.Set("fov", fov);
      arr.Set(i, v);
    }
    out.Set("views", arr);
    // この姿勢で描いた画像を後から（非同期に）提出するときの識別子
    ++locateSeq_;
    for (int i = 0; i < 2; ++i) viewRing_[locateSeq_ % kViewRing][i] = views_[i];
    out.Set("seq", static_cast<double>(locateSeq_));
    return out;
  }

  std::string ProfileName(XrPath hand) {
    XrInteractionProfileState st{XR_TYPE_INTERACTION_PROFILE_STATE};
    if (XR_FAILED(xrGetCurrentInteractionProfile(session_, hand, &st)) || st.interactionProfile == XR_NULL_PATH) {
      return "";
    }
    char buf[XR_MAX_PATH_LENGTH] = {};
    uint32_t len = 0;
    xrPathToString(instance_, st.interactionProfile, sizeof(buf), &len, buf);
    return buf;
  }

  Napi::Value SyncInput(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    EnsureAlive(env);
    auto out = Napi::Array::New(env, 2);

    bool focused = state_ == XR_SESSION_STATE_FOCUSED;
    if (focused) {
      XrActiveActionSet active{actionSet_, XR_NULL_PATH};
      XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
      si.countActiveActionSets = 1;
      si.activeActionSets = &active;
      xrSyncActions(session_, &si);
    }

    for (int i = 0; i < 2; ++i) {
      auto h = Napi::Object::New(env);
      h.Set("handedness", i == 0 ? "left" : "right");
      XrPath sub = hands_[i].path;

      auto getFloat = [&](XrAction a) -> double {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        gi.subactionPath = sub;
        XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
        if (!focused || XR_FAILED(xrGetActionStateFloat(session_, &gi, &s)) || !s.isActive) return 0.0;
        return s.currentState;
      };
      auto getBool = [&](XrAction a) -> bool {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        gi.subactionPath = sub;
        XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (!focused || XR_FAILED(xrGetActionStateBoolean(session_, &gi, &s)) || !s.isActive) return false;
        return s.currentState == XR_TRUE;
      };
      auto getVec2 = [&](XrAction a, bool* active) -> Napi::Float64Array {
        auto v = Napi::Float64Array::New(env, 2);
        v[0] = v[1] = 0;
        *active = false;
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        gi.subactionPath = sub;
        XrActionStateVector2f s{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (focused && XR_SUCCEEDED(xrGetActionStateVector2f(session_, &gi, &s)) && s.isActive) {
          v[0] = s.currentState.x;
          v[1] = s.currentState.y;
          *active = true;
        }
        return v;
      };
      auto locate = [&](XrSpace space, const char* key) {
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        XrResult r = xrLocateSpace(space, appSpace_, frameState_.predictedDisplayTime, &loc);
        bool ok = XR_SUCCEEDED(r) && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                  (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        if (ok) {
          auto p = Napi::Object::New(env);
          SetPose(env, p, loc.pose);
          h.Set(key, p);
        } else {
          h.Set(key, env.Null());
        }
        return ok;
      };

      bool gripOk = locate(hands_[i].gripSpace, "grip");
      bool aimOk = locate(hands_[i].aimSpace, "aim");
      h.Set("connected", gripOk || aimOk);
      h.Set("profile", ProfileName(sub));
      h.Set("trigger", getFloat(trigger_));
      h.Set("squeeze", getFloat(squeeze_));
      bool stickActive = false, padActive = false;
      h.Set("thumbstick", getVec2(thumbstick_, &stickActive));
      h.Set("trackpad", getVec2(trackpad_, &padActive));
      h.Set("hasThumbstick", stickActive);
      h.Set("hasTrackpad", padActive);
      h.Set("thumbstickClick", getBool(thumbstickClick_));
      h.Set("trackpadClick", getBool(trackpadClick_));
      h.Set("primary", getBool(primary_));
      h.Set("secondary", getBool(secondary_));
      h.Set("menu", getBool(menu_));
      out.Set(static_cast<uint32_t>(i), h);
    }
    return out;
  }

  // submit(eye, ArrayBuffer | TypedArray, byteOffset, bytesPerRow)
  Napi::Value Submit(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    EnsureAlive(env);
    int eye = info[0].ToNumber().Int32Value();
    if (eye < 0 || eye > 1) throw Napi::RangeError::New(env, "eye must be 0 or 1");
    uint8_t* data = nullptr;
    size_t length = 0;
    if (info[1].IsArrayBuffer()) {
      auto ab = info[1].As<Napi::ArrayBuffer>();
      data = static_cast<uint8_t*>(ab.Data());
      length = ab.ByteLength();
    } else if (info[1].IsTypedArray()) {
      auto ta = info[1].As<Napi::TypedArray>();
      data = static_cast<uint8_t*>(ta.ArrayBuffer().Data()) + ta.ByteOffset();
      length = ta.ByteLength();
    } else {
      throw Napi::TypeError::New(env, "pixels must be ArrayBuffer or TypedArray");
    }
    size_t offset = info.Length() > 2 ? info[2].ToNumber().Int64Value() : 0;
    uint32_t rowPitch = info.Length() > 3 ? info[3].ToNumber().Uint32Value() : eyes_[eye].width * 4;
    auto& sc = eyes_[eye];
    // 横並び（両眼で 1 枚）のバッファも受けられるよう、最後の行は眼の幅ぶんだけ要求する
    if (rowPitch < static_cast<uint32_t>(sc.width) * 4 ||
        offset + static_cast<size_t>(rowPitch) * (sc.height - 1) + static_cast<size_t>(sc.width) * 4 > length) {
      throw Napi::RangeError::New(env, "pixel buffer too small");
    }

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult r = xrAcquireSwapchainImage(sc.handle, &ai, &index);
    if (XR_FAILED(r)) throw Napi::Error::New(env, "xrAcquireSwapchainImage -> " + XrResultString(instance_, r));
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    r = xrWaitSwapchainImage(sc.handle, &wi);
    if (XR_FAILED(r)) throw Napi::Error::New(env, "xrWaitSwapchainImage -> " + XrResultString(instance_, r));

    context_->UpdateSubresource(sc.images[index].texture, 0, nullptr, data + offset, rowPitch, 0);

    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    r = xrReleaseSwapchainImage(sc.handle, &ri);
    if (XR_FAILED(r)) throw Napi::Error::New(env, "xrReleaseSwapchainImage -> " + XrResultString(instance_, r));
    sc.submitted = true;
    // 同期提出は「今のフレームの姿勢」で描いた画像として扱う
    if (eyes_[0].submitted && eyes_[1].submitted) {
      submittedViews_[0] = views_[0];
      submittedViews_[1] = views_[1];
      hasImage_ = true;
      eyes_[0].submitted = eyes_[1].submitted = false;
    }
    return env.Undefined();
  }

  // 両眼が横並びの 1 枚（上の行から）を別スレッドで左右のスワップチェーンへ書く。
  // submitStereoAsync(ArrayBuffer, offsetLeft, offsetRight, rowPitch, seq) -> Promise
  // ArrayBuffer は Promise が解決するまで書き換え・解放しないこと（WebGPU の mapped range をそのまま渡せる）
  class SubmitWorker : public Napi::AsyncWorker {
   public:
    SubmitWorker(Napi::Env env, XrSessionWrap* self, Napi::ArrayBuffer ab, size_t offL, size_t offR, uint32_t pitch, uint64_t seq)
        : Napi::AsyncWorker(env), self_(self), deferred_(Napi::Promise::Deferred::New(env)), offs_{offL, offR}, pitch_(pitch), seq_(seq) {
      ref_ = Napi::Persistent(ab);
      data_ = static_cast<uint8_t*>(ab.Data());
    }
    Napi::Promise Promise() { return deferred_.Promise(); }
    void Execute() override {
      for (int eye = 0; eye < 2 && XR_SUCCEEDED(result_); ++eye) {
        auto& sc = self_->eyes_[eye];
        uint32_t index = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        result_ = xrAcquireSwapchainImage(sc.handle, &ai, &index);
        if (XR_FAILED(result_)) { what_ = "xrAcquireSwapchainImage"; break; }
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        result_ = xrWaitSwapchainImage(sc.handle, &wi);
        if (XR_FAILED(result_)) { what_ = "xrWaitSwapchainImage"; break; }
        self_->context_->UpdateSubresource(sc.images[index].texture, 0, nullptr, data_ + offs_[eye], pitch_, 0);
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        result_ = xrReleaseSwapchainImage(sc.handle, &ri);
        if (XR_FAILED(result_)) { what_ = "xrReleaseSwapchainImage"; break; }
      }
      self_->context_->Flush();
    }
    void OnOK() override {
      Napi::Env env = Env();
      self_->submitBusy_ = false;
      if (XR_FAILED(result_)) {
        deferred_.Reject(Napi::Error::New(env, what_ + " -> " + XrResultString(self_->instance_, result_)).Value());
        return;
      }
      auto& v = self_->viewRing_[seq_ % kViewRing];
      self_->submittedViews_[0] = v[0];
      self_->submittedViews_[1] = v[1];
      self_->hasImage_ = true;
      deferred_.Resolve(env.Undefined());
    }
    void OnError(const Napi::Error& e) override {
      self_->submitBusy_ = false;
      deferred_.Reject(e.Value());
    }

   private:
    XrSessionWrap* self_;
    Napi::Promise::Deferred deferred_;
    Napi::Reference<Napi::ArrayBuffer> ref_;
    uint8_t* data_ = nullptr;
    size_t offs_[2];
    uint32_t pitch_;
    uint64_t seq_;
    XrResult result_ = XR_SUCCESS;
    std::string what_;
  };

  Napi::Value SubmitStereoAsync(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    EnsureAlive(env);
    if (!info[0].IsArrayBuffer()) throw Napi::TypeError::New(env, "pixels must be an ArrayBuffer");
    auto ab = info[0].As<Napi::ArrayBuffer>();
    size_t offL = info[1].ToNumber().Int64Value();
    size_t offR = info[2].ToNumber().Int64Value();
    uint32_t pitch = info[3].ToNumber().Uint32Value();
    uint64_t seq = static_cast<uint64_t>(info[4].ToNumber().Int64Value());
    size_t need = static_cast<size_t>(pitch) * (eyes_[0].height - 1) + static_cast<size_t>(eyes_[0].width) * 4;
    if (pitch < static_cast<uint32_t>(eyes_[0].width) * 4 || offL + need > ab.ByteLength() || offR + need > ab.ByteLength()) {
      throw Napi::RangeError::New(env, "pixel buffer too small");
    }
    if (submitBusy_) throw Napi::Error::New(env, "a submit is already in flight");
    submitBusy_ = true;
    auto* w = new SubmitWorker(env, this, ab, offL, offR, pitch, seq);
    auto p = w->Promise();
    w->Queue();
    return p;
  }

  // submitLatched(ArrayBuffer, offsetLeft, offsetRight, rowPitch, seq): 同期版（seq の姿勢で描いた画像として提出）
  Napi::Value SubmitLatched(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    EnsureAlive(env);
    if (!info[0].IsArrayBuffer()) throw Napi::TypeError::New(env, "pixels must be an ArrayBuffer");
    auto ab = info[0].As<Napi::ArrayBuffer>();
    auto* data = static_cast<uint8_t*>(ab.Data());
    size_t offs[2] = {static_cast<size_t>(info[1].ToNumber().Int64Value()), static_cast<size_t>(info[2].ToNumber().Int64Value())};
    uint32_t pitch = info[3].ToNumber().Uint32Value();
    uint64_t seq = static_cast<uint64_t>(info[4].ToNumber().Int64Value());
    size_t need = static_cast<size_t>(pitch) * (eyes_[0].height - 1) + static_cast<size_t>(eyes_[0].width) * 4;
    if (pitch < static_cast<uint32_t>(eyes_[0].width) * 4 || offs[0] + need > ab.ByteLength() || offs[1] + need > ab.ByteLength()) {
      throw Napi::RangeError::New(env, "pixel buffer too small");
    }
    // 1) 両眼のステージングテクスチャを Map し、行ごとのコピーを 2 スレッドで並列に行う
    const int slot = stagingSlot_;
    stagingSlot_ ^= 1;
    D3D11_MAPPED_SUBRESOURCE mapped[2] = {};
    for (int eye = 0; eye < 2; ++eye) {
      HRESULT hr = context_->Map(eyes_[eye].staging[slot], 0, D3D11_MAP_WRITE, 0, &mapped[eye]);
      if (FAILED(hr)) {
        if (eye == 1) context_->Unmap(eyes_[0].staging[slot], 0);
        throw Napi::Error::New(env, "Map(staging) failed");
      }
    }
    auto copyEye = [&](int eye) {
      const uint8_t* src = data + offs[eye];
      uint8_t* dst = static_cast<uint8_t*>(mapped[eye].pData);
      const size_t row = static_cast<size_t>(eyes_[eye].width) * 4;
      for (int y = 0; y < eyes_[eye].height; ++y) memcpy(dst + y * mapped[eye].RowPitch, src + static_cast<size_t>(y) * pitch, row);
    };
    std::thread other(copyEye, 1);
    copyEye(0);
    other.join();
    for (int eye = 0; eye < 2; ++eye) context_->Unmap(eyes_[eye].staging[slot], 0);

    // 2) スワップチェーン画像へは GPU 上でコピー
    for (int eye = 0; eye < 2; ++eye) {
      auto& sc = eyes_[eye];
      uint32_t index = 0;
      XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
      XrResult r = xrAcquireSwapchainImage(sc.handle, &ai, &index);
      if (XR_FAILED(r)) throw Napi::Error::New(env, "xrAcquireSwapchainImage -> " + XrResultString(instance_, r));
      XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
      wi.timeout = XR_INFINITE_DURATION;
      r = xrWaitSwapchainImage(sc.handle, &wi);
      if (XR_FAILED(r)) throw Napi::Error::New(env, "xrWaitSwapchainImage -> " + XrResultString(instance_, r));
      context_->CopyResource(sc.images[index].texture, sc.staging[slot]);
      XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      r = xrReleaseSwapchainImage(sc.handle, &ri);
      if (XR_FAILED(r)) throw Napi::Error::New(env, "xrReleaseSwapchainImage -> " + XrResultString(instance_, r));
    }
    auto& v = viewRing_[seq % kViewRing];
    submittedViews_[0] = v[0];
    submittedViews_[1] = v[1];
    hasImage_ = true;
    return env.Undefined();
  }

  Napi::Value EndFrame(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    EnsureAlive(env);
    double nearZ = info.Length() > 0 && info[0].IsNumber() ? info[0].ToNumber().DoubleValue() : 0.0;
    double farZ = info.Length() > 1 && info[1].IsNumber() ? info[1].ToNumber().DoubleValue() : 0.0;
    (void)nearZ;
    (void)farZ;

    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                              {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* layers[1] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};

    // 最後に書き込んだ画像を、それを描いたときの姿勢で出す（非同期提出だと 1 フレーム前の画像になりうる。
    // 姿勢を正しく渡すのでコンポジタが再投影する）
    // shouldRender が false でも（SYNCHRONIZED 中も）画像があれば出す。SteamVR は画像が来るまで VISIBLE にしないため
    bool submit = hasImage_ && (frameState_.shouldRender || alwaysSubmit_);
    if (submit) {
      for (int i = 0; i < 2; ++i) {
        pv[i].pose = submittedViews_[i].pose;
        pv[i].fov = submittedViews_[i].fov;
        pv[i].subImage.swapchain = eyes_[i].handle;
        pv[i].subImage.imageRect.offset = {0, 0};
        pv[i].subImage.imageRect.extent = {eyes_[i].width, eyes_[i].height};
        pv[i].subImage.imageArrayIndex = 0;
      }
      layer.space = appSpace_;
      layer.viewCount = 2;
      layer.views = pv;
    }

    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = frameState_.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = submit ? 1 : 0;
    ei.layers = submit ? layers : nullptr;
    XrResult r = xrEndFrame(session_, &ei);
    if (XR_FAILED(r)) throw Napi::Error::New(env, "xrEndFrame -> " + XrResultString(instance_, r));
    return Napi::Boolean::New(env, submit);
  }

  // applyHaptics(handIndex, amplitude 0..1, durationMs)
  Napi::Value ApplyHaptics(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    EnsureAlive(env);
    int hand = info[0].ToNumber().Int32Value();
    if (hand < 0 || hand > 1) return env.Undefined();
    XrHapticVibration vib{XR_TYPE_HAPTIC_VIBRATION};
    vib.amplitude = static_cast<float>(std::clamp(info[1].ToNumber().DoubleValue(), 0.0, 1.0));
    vib.duration = static_cast<XrDuration>(info[2].ToNumber().DoubleValue() * 1e6);
    vib.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = haptic_;
    hi.subactionPath = hands_[hand].path;
    xrApplyHapticFeedback(session_, &hi, reinterpret_cast<XrHapticBaseHeader*>(&vib));
    return env.Undefined();
  }

  Napi::Value RequestExit(const Napi::CallbackInfo& info) {
    if (session_ && running_) xrRequestExitSession(session_);
    return info.Env().Undefined();
  }

  Napi::Value Destroy(const Napi::CallbackInfo& info) {
    Cleanup();
    return info.Env().Undefined();
  }

  // ------------------------------------------------------------------ 状態

  XrInstance instance_ = XR_NULL_HANDLE;
  XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
  XrSession session_ = XR_NULL_HANDLE;
  XrSpace appSpace_ = XR_NULL_HANDLE;
  XrSpace viewSpace_ = XR_NULL_HANDLE;
  XrSessionState state_ = XR_SESSION_STATE_UNKNOWN;
  XrFrameState frameState_{XR_TYPE_FRAME_STATE};
  bool running_ = false;
  bool exiting_ = false;
  bool viewsValid_ = false;
  bool hasLocalFloorExt_ = false;
  bool isBGRA_ = false;
  int64_t colorFormat_ = 0;

  std::vector<XrViewConfigurationView> viewConfigs_;
  std::vector<XrView> views_;
  static constexpr int kViewRing = 8;
  XrView viewRing_[kViewRing][2] = {};
  XrView submittedViews_[2] = {};
  uint64_t locateSeq_ = 0;
  bool hasImage_ = false;
  bool submitBusy_ = false;
  int stagingSlot_ = 0;
  bool alwaysSubmit_ = true;
  EyeSwapchain eyes_[2];

  XrActionSet actionSet_ = XR_NULL_HANDLE;
  HandActions hands_[2];
  XrAction gripPose_ = XR_NULL_HANDLE, aimPose_ = XR_NULL_HANDLE;
  XrAction trigger_ = XR_NULL_HANDLE, squeeze_ = XR_NULL_HANDLE;
  XrAction thumbstick_ = XR_NULL_HANDLE, thumbstickClick_ = XR_NULL_HANDLE;
  XrAction trackpad_ = XR_NULL_HANDLE, trackpadClick_ = XR_NULL_HANDLE;
  XrAction primary_ = XR_NULL_HANDLE, secondary_ = XR_NULL_HANDLE, menu_ = XR_NULL_HANDLE;
  XrAction haptic_ = XR_NULL_HANDLE;

  ID3D11Device* device_ = nullptr;
  ID3D11DeviceContext* context_ = nullptr;

  std::string runtimeName_, systemName_, adapterName_, referenceSpaceName_;
};

// アクティブなランタイムの情報だけを調べる（セッションは作らない）
static Napi::Value ProbeRuntime(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  auto o = Napi::Object::New(env);
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  strcpy_s(ci.applicationInfo.applicationName, "XRift OpenXR Probe");
  ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
  const char* ext = XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
  ci.enabledExtensionCount = 1;
  ci.enabledExtensionNames = &ext;
  XrInstance inst = XR_NULL_HANDLE;
  XrResult r = xrCreateInstance(&ci, &inst);
  if (XR_FAILED(r)) {
    o.Set("ok", false);
    o.Set("error", "xrCreateInstance -> " + XrResultString(XR_NULL_HANDLE, r));
    return o;
  }
  XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
  xrGetInstanceProperties(inst, &ip);
  o.Set("runtimeName", ip.runtimeName);
  XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
  sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  XrSystemId sys = XR_NULL_SYSTEM_ID;
  r = xrGetSystem(inst, &sgi, &sys);
  o.Set("ok", XR_SUCCEEDED(r));
  if (XR_SUCCEEDED(r)) {
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(inst, sys, &sp);
    o.Set("systemName", sp.systemName);
  } else {
    o.Set("error", "xrGetSystem -> " + XrResultString(inst, r));
  }
  xrDestroyInstance(inst);
  return o;
}

// ---------------------------------------------------------------- SteamVR アプリ登録
//
// SteamVR 付属の openvr_api.dll をユーティリティとして初期化し、IVRApplications の
// フラット関数テーブル（FnTable:IVRApplications_007）で .vrmanifest を登録 / 解除する。
// 登録すると SteamVR のライブラリ（ダッシュボード）から起動できるようになる。

namespace openvr {
typedef uint32_t(__cdecl* PFN_VR_InitInternal2)(int* peError, int eType, const char* pStartupInfo);
typedef void(__cdecl* PFN_VR_ShutdownInternal)();
typedef void*(__cdecl* PFN_VR_GetGenericInterface)(const char* pchInterfaceVersion, int* peError);
typedef int(__stdcall* PFN_AddApplicationManifest)(const char* path, bool temporary);
typedef int(__stdcall* PFN_RemoveApplicationManifest)(const char* path);
typedef bool(__stdcall* PFN_IsApplicationInstalled)(const char* appKey);
struct IVRApplicationsFnTable {
  PFN_AddApplicationManifest AddApplicationManifest;
  PFN_RemoveApplicationManifest RemoveApplicationManifest;
  PFN_IsApplicationInstalled IsApplicationInstalled;
};
constexpr int VRApplication_Utility = 3;
}  // namespace openvr

// steamvrManifest(op: 'add'|'remove'|'check', dllPath, manifestPath | appKey) -> { ok, error? , installed? }
static Napi::Value SteamVRManifest(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  std::string op = info[0].ToString().Utf8Value();
  std::string dll = info[1].ToString().Utf8Value();
  std::string arg = info[2].ToString().Utf8Value();
  auto result = Napi::Object::New(env);

  int wlen = MultiByteToWideChar(CP_UTF8, 0, dll.c_str(), -1, nullptr, 0);
  std::wstring wdll(wlen, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, dll.c_str(), -1, wdll.data(), wlen);
  HMODULE mod = LoadLibraryW(wdll.c_str());
  if (!mod) {
    result.Set("ok", false);
    result.Set("error", "openvr_api.dll を読み込めません: " + dll);
    return result;
  }
  auto init = reinterpret_cast<openvr::PFN_VR_InitInternal2>(GetProcAddress(mod, "VR_InitInternal2"));
  auto shutdown = reinterpret_cast<openvr::PFN_VR_ShutdownInternal>(GetProcAddress(mod, "VR_ShutdownInternal"));
  auto getIface = reinterpret_cast<openvr::PFN_VR_GetGenericInterface>(GetProcAddress(mod, "VR_GetGenericInterface"));
  if (!init || !shutdown || !getIface) {
    FreeLibrary(mod);
    result.Set("ok", false);
    result.Set("error", "openvr_api.dll に必要な関数がありません");
    return result;
  }
  int err = 0;
  init(&err, openvr::VRApplication_Utility, nullptr);
  if (err != 0) {
    FreeLibrary(mod);
    result.Set("ok", false);
    result.Set("error", "VR_Init failed (EVRInitError " + std::to_string(err) + ")。SteamVR を起動してから実行してください");
    return result;
  }
  auto* apps = reinterpret_cast<openvr::IVRApplicationsFnTable*>(getIface("FnTable:IVRApplications_007", &err));
  if (!apps || err != 0) {
    shutdown();
    FreeLibrary(mod);
    result.Set("ok", false);
    result.Set("error", "IVRApplications_007 を取得できません (" + std::to_string(err) + ")");
    return result;
  }
  int appErr = 0;
  if (op == "add") {
    appErr = apps->AddApplicationManifest(arg.c_str(), false);
  } else if (op == "remove") {
    appErr = apps->RemoveApplicationManifest(arg.c_str());
  } else if (op == "check") {
    result.Set("installed", apps->IsApplicationInstalled(arg.c_str()));
  }
  shutdown();
  FreeLibrary(mod);
  result.Set("ok", appErr == 0);
  if (appErr != 0) result.Set("error", "EVRApplicationError " + std::to_string(appErr));
  return result;
}

void InitOpenVR(Napi::Env env, Napi::Object exports);  // openvr_session.cpp

static Napi::Object InitAll(Napi::Env env, Napi::Object exports) {
  XrSessionWrap::Init(env, exports);
  exports.Set("probeRuntime", Napi::Function::New(env, ProbeRuntime));
  exports.Set("steamvrManifest", Napi::Function::New(env, SteamVRManifest));
  InitOpenVR(env, exports);
  return exports;
}

NODE_API_MODULE(xrift_openxr, InitAll)
