// XRift VR — OpenVR（SteamVR ネイティブ API）バックエンド
//
// OpenXR 版（addon.cpp の OpenXRSession）と同じメソッド名・戻り値の形で使えるようにしたもの。
// 一部の環境（ALVR など）では OpenXR アプリが SYNCHRONIZED から先に進まないため、
// VRChat などと同じ OpenVR の経路で SteamVR に出力する。
//
//   pollEvents() → await waitFrame()（WaitGetPoses を別スレッドで）→ beginFrame()
//   → locateViews() → syncInput() → submit / submitLatched → endFrame()
//
// 画像は「それを描いたときの HMD 姿勢」と一緒に Submit_TextureWithPose で提出するので、
// 非同期に 1 フレーム遅れて提出してもコンポジタが正しく再投影する。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>

#include <napi.h>
#include <openvr.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

template <typename T>
void Release(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

struct Pose {
  double p[3];
  double q[4];  // x, y, z, w
};

// OpenVR の 3x4（行優先・右手系・Y 上）→ 位置 + クォータニオン
Pose ToPose(const vr::HmdMatrix34_t& m) {
  Pose r{};
  r.p[0] = m.m[0][3];
  r.p[1] = m.m[1][3];
  r.p[2] = m.m[2][3];
  const double m00 = m.m[0][0], m01 = m.m[0][1], m02 = m.m[0][2];
  const double m10 = m.m[1][0], m11 = m.m[1][1], m12 = m.m[1][2];
  const double m20 = m.m[2][0], m21 = m.m[2][1], m22 = m.m[2][2];
  const double tr = m00 + m11 + m22;
  double x, y, z, w;
  if (tr > 0) {
    double s = 0.5 / std::sqrt(tr + 1.0);
    w = 0.25 / s; x = (m21 - m12) * s; y = (m02 - m20) * s; z = (m10 - m01) * s;
  } else if (m00 > m11 && m00 > m22) {
    double s = 2.0 * std::sqrt(1.0 + m00 - m11 - m22);
    w = (m21 - m12) / s; x = 0.25 * s; y = (m01 + m10) / s; z = (m02 + m20) / s;
  } else if (m11 > m22) {
    double s = 2.0 * std::sqrt(1.0 + m11 - m00 - m22);
    w = (m02 - m20) / s; x = (m01 + m10) / s; y = 0.25 * s; z = (m12 + m21) / s;
  } else {
    double s = 2.0 * std::sqrt(1.0 + m22 - m00 - m11);
    w = (m10 - m01) / s; x = (m02 + m20) / s; y = (m12 + m21) / s; z = 0.25 * s;
  }
  double len = std::sqrt(x * x + y * y + z * z + w * w);
  if (len < 1e-9) len = 1;
  r.q[0] = x / len; r.q[1] = y / len; r.q[2] = z / len; r.q[3] = w / len;
  return r;
}

vr::HmdMatrix34_t Mul(const vr::HmdMatrix34_t& a, const vr::HmdMatrix34_t& b) {
  vr::HmdMatrix34_t r{};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 4; ++j) {
      r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + (j == 3 ? a.m[i][3] : 0.0f);
    }
  }
  return r;
}

void SetPose(Napi::Env env, Napi::Object o, const Pose& p) {
  auto pos = Napi::Float64Array::New(env, 3);
  for (int i = 0; i < 3; ++i) pos[i] = p.p[i];
  auto ori = Napi::Float64Array::New(env, 4);
  for (int i = 0; i < 4; ++i) ori[i] = p.q[i];
  o.Set("position", pos);
  o.Set("orientation", ori);
}

// QPC をミリ秒に変換する。周波数はプロセス内で不変なので一度だけ取得する。
// JS の描画終了タグと Submit 時刻を同じ時計で比較するために使う。
double NowMs() {
  static const double freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<double>(f.QuadPart);
  }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return static_cast<double>(t.QuadPart) * 1000.0 / freq;
}

// スコープを抜けたらフラグを戻す。早期 return や例外でも確実に戻すために使う
struct FlagGuard {
  std::atomic<bool>& flag;
  ~FlagGuard() { flag = false; }
};

const char* ProfileFor(const std::string& controllerType) {
  if (controllerType == "oculus_touch") return "/interaction_profiles/oculus/touch_controller";
  if (controllerType == "knuckles") return "/interaction_profiles/valve/index_controller";
  if (controllerType == "vive_controller") return "/interaction_profiles/htc/vive_controller";
  if (controllerType == "holographic_controller") return "/interaction_profiles/microsoft/motion_controller";
  return "/interaction_profiles/oculus/touch_controller";
}

}  // namespace

class VrSessionWrap : public Napi::ObjectWrap<VrSessionWrap> {
 public:
  static void Init(Napi::Env env, Napi::Object exports) {
    Napi::Function func = DefineClass(env, "OpenVRSession", {
      InstanceMethod("pollEvents", &VrSessionWrap::PollEvents),
      InstanceMethod("waitFrame", &VrSessionWrap::WaitFrame),
      InstanceMethod("beginFrame", &VrSessionWrap::Noop),
      InstanceMethod("locateViews", &VrSessionWrap::LocateViews),
      InstanceMethod("syncInput", &VrSessionWrap::SyncInput),
      InstanceMethod("submit", &VrSessionWrap::Submit),
      InstanceMethod("submitLatched", &VrSessionWrap::SubmitLatched),
      InstanceMethod("submitLatchedAsync", &VrSessionWrap::SubmitLatchedAsync),
      InstanceMethod("setLatencyMode", &VrSessionWrap::SetLatencyMode),
      InstanceMethod("fillFrame", &VrSessionWrap::FillFrame),
      InstanceMethod("markPending", &VrSessionWrap::MarkPending),
      InstanceMethod("endFrame", &VrSessionWrap::EndFrame),
      InstanceMethod("applyHaptics", &VrSessionWrap::ApplyHaptics),
      InstanceMethod("requestExit", &VrSessionWrap::RequestExit),
      InstanceMethod("destroy", &VrSessionWrap::Destroy),
      InstanceMethod("getInfo", &VrSessionWrap::GetInfo),
      InstanceMethod("nowQpcMs", &VrSessionWrap::NowQpcMs),
    });
    exports.Set("OpenVRSession", func);
    exports.Set("probeOpenVR", Napi::Function::New(env, Probe));
  }

  VrSessionWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<VrSessionWrap>(info) {
    Napi::Env env = info.Env();
    double scale = 1.0;
    if (info.Length() > 0 && info[0].IsObject()) {
      auto o = info[0].As<Napi::Object>();
      if (o.Has("resolutionScale")) scale = o.Get("resolutionScale").ToNumber().DoubleValue();
    }
    if (const char* m = std::getenv("XRIFT_SUBMIT_MARGIN_MS")) submitMarginMs_ = std::atof(m);
    std::string err = InitAll(scale);
    if (!err.empty()) {
      Cleanup();
      throw Napi::Error::New(env, "OpenVR init failed: " + err);
    }
  }
  ~VrSessionWrap() override { Cleanup(); }

  static Napi::Value Probe(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto o = Napi::Object::New(env);
    bool runtime = vr::VR_IsRuntimeInstalled();
    bool hmd = runtime && vr::VR_IsHmdPresent();
    o.Set("ok", hmd);
    o.Set("runtimeName", "SteamVR (OpenVR)");
    if (!hmd) o.Set("error", runtime ? "HMD が見つかりません" : "SteamVR がインストールされていません");
    return o;
  }

 private:
  std::string InitAll(double scale) {
    vr::EVRInitError e = vr::VRInitError_None;
    system_ = vr::VR_Init(&e, vr::VRApplication_Scene);
    if (e != vr::VRInitError_None || !system_) return vr::VR_GetVRInitErrorAsEnglishDescription(e);
    initialized_ = true;
    compositor_ = vr::VRCompositor();
    if (!compositor_) return "VRCompositor unavailable";
    compositor_->SetTrackingSpace(vr::TrackingUniverseStanding);

    char buf[256] = {};
    system_->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_ModelNumber_String, buf, sizeof(buf));
    systemName_ = buf;
    memset(buf, 0, sizeof(buf));
    system_->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_TrackingSystemName_String, buf, sizeof(buf));
    if (buf[0]) systemName_ += std::string(" (") + buf + ")";

    // SteamVR が使っている GPU と同じアダプタに D3D11 デバイスを作る
    int32_t adapterIndex = -1;
    system_->GetDXGIOutputInfo(&adapterIndex);
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return "CreateDXGIFactory1 failed";
    IDXGIAdapter1* adapter = nullptr;
    if (adapterIndex >= 0) factory->EnumAdapters1(adapterIndex, &adapter);
    if (adapter) {
      DXGI_ADAPTER_DESC1 d;
      adapter->GetDesc1(&d);
      char name[256] = {};
      WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
      adapterName_ = name;
    }
    factory->Release();
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                   D3D11_SDK_VERSION, &device_, nullptr, &context_);
    Release(adapter);
    if (FAILED(hr)) return "D3D11CreateDevice failed";
    // SteamVR は WaitGetPoses / Submit の中でこのデバイスの即時コンテキストを使う。
    // こちらの転送スレッドと同時に触っても壊れないよう、コンテキストの排他を有効にする
    {
      ID3D11Multithread* mt = nullptr;
      if (SUCCEEDED(context_->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&mt)))) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
      }
    }

    uint32_t rw = 0, rh = 0;
    system_->GetRecommendedRenderTargetSize(&rw, &rh);
    recW_ = rw;
    recH_ = rh;
    {
      vr::ETrackedPropertyError pe = vr::TrackedProp_Success;
      float hz = system_->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float, &pe);
      if (pe == vr::TrackedProp_Success && hz > 30.0f) displayHz_ = hz;
    }
    int w = static_cast<int>(std::lround(rw * scale));
    int h = static_cast<int>(std::lround(rh * scale));
    width_ = std::max(64, ((w + 63) / 64) * 64);
    height_ = std::max(64, h);

    for (int eye = 0; eye < 2; ++eye) {
      D3D11_TEXTURE2D_DESC td{};
      td.Width = width_;
      td.Height = height_;
      td.MipLevels = 1;
      td.ArraySize = 1;
      td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;  // JS は sRGB エンコード済みの値を書く
      td.SampleDesc.Count = 1;
      td.Usage = D3D11_USAGE_DEFAULT;
      td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
      // 共有可能にしておくと、SteamVR は Submit 時にコピーせずこのテクスチャを直接使える
      td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
      for (int i = 0; i < 2; ++i) {
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &eyeTex_[eye][i]))) return "CreateTexture2D(eye) failed";
      }
      D3D11_TEXTURE2D_DESC sd = td;
      sd.Usage = D3D11_USAGE_STAGING;
      sd.BindFlags = 0;
      sd.MiscFlags = 0;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      for (int i = 0; i < 2; ++i) {
        if (FAILED(device_->CreateTexture2D(&sd, nullptr, &staging_[eye][i]))) return "CreateTexture2D(staging) failed";
      }
    }

    for (int eye = 0; eye < 2; ++eye) {
      eyeToHead_[eye] = system_->GetEyeToHeadTransform(static_cast<vr::EVREye>(eye));
      float l, r, t, b;
      system_->GetProjectionRaw(static_cast<vr::EVREye>(eye), &l, &r, &t, &b);
      // OpenVR は tan 値（上が負）。OpenXR 形式の角度（左, 右, 上, 下）にそろえる
      fov_[eye][0] = std::atan(l);
      fov_[eye][1] = std::atan(r);
      fov_[eye][2] = std::atan(-t);
      fov_[eye][3] = std::atan(-b);
    }
    return "";
  }

  void Cleanup() {
    // 転送・フレーム待ちスレッドが compositor やテクスチャを使っている間に解放すると落ちる。
    // 終了を止めないよう最大 2 秒だけ待つ
    const auto waitUntil = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((waitRunning_.load() || uploadRunning_.load()) && std::chrono::steady_clock::now() < waitUntil)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    for (auto& e : eyeTex_) for (auto& t : e) Release(t);
    for (auto& e : staging_) for (auto& t : e) Release(t);
    Release(context_);
    Release(device_);
    if (initialized_) {
      vr::VR_Shutdown();
      initialized_ = false;
    }
    system_ = nullptr;
    compositor_ = nullptr;
  }

  void EnsureAlive(Napi::Env env) {
    if (!initialized_) throw Napi::Error::New(env, "OpenVR session destroyed");
  }

  Napi::Value Noop(const Napi::CallbackInfo& info) { return info.Env().Undefined(); }

  Napi::Value GetInfo(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto o = Napi::Object::New(env);
    o.Set("backend", "openvr");
    o.Set("runtimeName", "SteamVR (OpenVR)");
    o.Set("systemName", systemName_);
    o.Set("adapterName", adapterName_);
    o.Set("referenceSpace", "standing");
    o.Set("width", width_);
    o.Set("height", height_);
    o.Set("colorFormat", "rgba8unorm");
    o.Set("srgbSwapchain", true);
    auto rec = Napi::Array::New(env, 2);
    for (uint32_t i = 0; i < 2; ++i) {
      auto r = Napi::Object::New(env);
      r.Set("width", recW_);
      r.Set("height", recH_);
      rec.Set(i, r);
    }
    o.Set("recommended", rec);
    return o;
  }

  // JS が描画終了のタグを付けるための単調時計（QPC ミリ秒）。差だけを使う
  Napi::Value NowQpcMs(const Napi::CallbackInfo& info) {
    return Napi::Number::New(info.Env(), NowMs());
  }

  // OpenXR 版と同じ形のイベントを返す（最初の呼び出しで ready → begin → focused）
  Napi::Value PollEvents(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto arr = Napi::Array::New(env);
    uint32_t n = 0;
    auto push = [&](const char* type, const char* detail) {
      auto e = Napi::Object::New(env);
      e.Set("type", type);
      if (detail) e.Set("detail", detail);
      arr.Set(n++, e);
    };
    if (!initialized_) {
      push("exit", "destroyed");
      return arr;
    }
    if (!started_) {
      started_ = true;
      push("state", "ready");
      push("begin", nullptr);
      push("state", "focused");
    }
    vr::VREvent_t ev;
    while (system_->PollNextEvent(&ev, sizeof(ev))) {
      if (ev.eventType == vr::VREvent_Quit) {
        system_->AcknowledgeQuit_Exiting();
        push("state", "stopping");
        push("end", nullptr);
        push("exit", "quit");
      } else if (ev.eventType == vr::VREvent_TrackedDeviceActivated || ev.eventType == vr::VREvent_TrackedDeviceRoleChanged) {
        push("profiles", nullptr);
      }
    }
    if (exitRequested_) {
      exitRequested_ = false;
      push("state", "stopping");
      push("end", nullptr);
      push("exit", "requested");
    }
    return arr;
  }

  // SubmitLatest が返す計測値。valid は描画終了タグがあり、その画像を提出した場合だけ true
  struct SubmitTiming {
    bool ran = false;
    bool valid = false;
    double renderToSubmitMs = 0;
    double uploadToSubmitMs = 0;
    double submitCallMs = 0;
    double submitFirstMs = 0;
    double handoffMs = 0;
    double submitAtMs = 0;
    uint64_t seq = 0;
  };

  class WaitWorker : public Napi::AsyncWorker {
   public:
    WaitWorker(Napi::Env env, VrSessionWrap* s) : Napi::AsyncWorker(env), s_(s), d_(Napi::Promise::Deferred::New(env)) {}
    Napi::Promise Promise() { return d_.Promise(); }
    void Execute() override {
      [[maybe_unused]] FlagGuard running{s_->waitRunning_};
      using clock = std::chrono::steady_clock;
      // 1) この枠の提出（枠ごとにちょうど 1 回）。
      //    いま描いているフレームの転送が「次のフレーム開始の少し前」までに終われば、それを出す（遅れ 0）。
      //    間に合わなければ、その時点の最新の画像を出す（fps は落とさない。新しい画像は次の枠で出る）
      {
        std::unique_lock<std::mutex> lk(s_->frameMtx_);
        if (s_->haveTiming_) {
          const auto period = std::chrono::duration<double, std::milli>(1000.0 / s_->displayHz_ - s_->submitMarginMs_);
          const auto deadline = s_->lastWaitReturn_ + std::chrono::duration_cast<clock::duration>(period);
          s_->frameCv_.wait_until(lk, deadline, [&] { return !s_->uploadPending_; });
        }
        if (s_->hasImage_ && s_->initialized_) {
          fresh_ = s_->publishedGen_ != s_->submittedGen_;
          submitTiming_ = s_->SubmitLatest();
        }
      }
      // 2) 次のフレームの開始まで待つ
      const double waitStartMs = NowMs();
      err_ = s_->compositor_->WaitGetPoses(s_->poses_, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
      doneMs_ = NowMs();
      waitMs_ = doneMs_ - waitStartMs;
      {
        std::lock_guard<std::mutex> lk(s_->frameMtx_);
        s_->lastWaitReturn_ = clock::now();
        s_->haveTiming_ = true;
      }
      // 直前に提出したフレームを SteamVR がどう扱ったか。取得できないランタイムでは false
      frameTiming_ = {};
      frameTiming_.m_nSize = static_cast<uint32_t>(sizeof(frameTiming_));
      frameTimingValid_ = s_->compositor_->GetFrameTiming(&frameTiming_, 0);
    }
    void OnOK() override {
      Napi::Env env = Env();
      auto o = Napi::Object::New(env);
      o.Set("shouldRender", err_ == vr::VRCompositorError_None);
      o.Set("predictedDisplayPeriodMs", 1000.0 / s_->displayHz_);
      s_->waitInFlight_ = false;
      o.Set("fresh", fresh_);
      o.Set("nativeWaitMs", waitMs_);
      o.Set("notifyDelayMs", NowMs() - doneMs_);
      auto submit = Napi::Object::New(env);
      submit.Set("ran", submitTiming_.ran);
      submit.Set("valid", fresh_ && submitTiming_.valid);
      submit.Set("renderToSubmitMs", submitTiming_.renderToSubmitMs);
      submit.Set("uploadToSubmitMs", submitTiming_.uploadToSubmitMs);
      submit.Set("submitCallMs", submitTiming_.submitCallMs);
      submit.Set("submitFirstMs", submitTiming_.submitFirstMs);
      submit.Set("handoffMs", submitTiming_.handoffMs);
      submit.Set("seq", static_cast<double>(submitTiming_.seq));
      o.Set("submitTiming", submit);
      auto frame = Napi::Object::New(env);
      frame.Set("valid", frameTimingValid_);
      if (frameTimingValid_) {
        frame.Set("frameIndex", static_cast<double>(frameTiming_.m_nFrameIndex));
        frame.Set("appFrameMs", frameTiming_.m_flNewFrameReadyMs - frameTiming_.m_flWaitGetPosesCalledMs);
        frame.Set("preSubmitGpuMs", frameTiming_.m_flPreSubmitGpuMs);
        frame.Set("submitFrameMs", frameTiming_.m_flSubmitFrameMs);
        frame.Set("compositorIdleCpuMs", frameTiming_.m_flCompositorIdleCpuMs);
        frame.Set("compositorRenderGpuMs", frameTiming_.m_flCompositorRenderGpuMs);
        frame.Set("transferLatencyMs", frameTiming_.m_flTransferLatencyMs);
        frame.Set("clientFrameIntervalMs", frameTiming_.m_flClientFrameIntervalMs);
        frame.Set("droppedFrames", static_cast<double>(frameTiming_.m_nNumDroppedFrames));
        frame.Set("misPresented", static_cast<double>(frameTiming_.m_nNumMisPresented));
      }
      o.Set("frameTiming", frame);
      d_.Resolve(o);
    }
    void OnError(const Napi::Error& e) override { d_.Reject(e.Value()); }

   private:
    VrSessionWrap* s_;
    Napi::Promise::Deferred d_;
    vr::EVRCompositorError err_ = vr::VRCompositorError_None;
    double waitMs_ = 0;
    double doneMs_ = 0;
    SubmitTiming submitTiming_;
    vr::Compositor_FrameTiming frameTiming_{};
    bool frameTimingValid_ = false;
    bool fresh_ = false;
  };

  Napi::Value WaitFrame(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    EnsureAlive(env);
    waitInFlight_ = true;
    auto* w = new WaitWorker(env, this);
    auto p = w->Promise();
    waitRunning_ = true;
    w->Queue();
    return p;
  }

  Napi::Value LocateViews(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    EnsureAlive(env);
    const auto& hmd = poses_[vr::k_unTrackedDeviceIndex_Hmd];
    const bool valid = hmd.bPoseIsValid;
    ++seq_;
    hmdRing_[seq_ % kRing] = hmd.mDeviceToAbsoluteTracking;
    auto out = Napi::Object::New(env);
    out.Set("valid", valid);
    out.Set("positionTracked", valid && hmd.eTrackingResult == vr::TrackingResult_Running_OK);
    out.Set("seq", static_cast<double>(seq_));
    auto arr = Napi::Array::New(env, 2);
    for (uint32_t eye = 0; eye < 2; ++eye) {
      auto v = Napi::Object::New(env);
      SetPose(env, v, ToPose(Mul(hmd.mDeviceToAbsoluteTracking, eyeToHead_[eye])));
      auto fov = Napi::Float64Array::New(env, 4);
      for (int i = 0; i < 4; ++i) fov[i] = fov_[eye][i];
      v.Set("fov", fov);
      arr.Set(eye, v);
    }
    out.Set("views", arr);
    return out;
  }

  Napi::Value SyncInput(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    EnsureAlive(env);
    auto out = Napi::Array::New(env, 2);
    const vr::ETrackedControllerRole roles[2] = {vr::TrackedControllerRole_LeftHand, vr::TrackedControllerRole_RightHand};
    for (uint32_t i = 0; i < 2; ++i) {
      auto h = Napi::Object::New(env);
      h.Set("handedness", i == 0 ? "left" : "right");
      vr::TrackedDeviceIndex_t idx = system_->GetTrackedDeviceIndexForControllerRole(roles[i]);
      hand_[i] = idx;
      bool ok = idx != vr::k_unTrackedDeviceIndexInvalid && idx < vr::k_unMaxTrackedDeviceCount && poses_[idx].bPoseIsValid;
      double trigger = 0, squeeze = 0;
      double stick[2] = {0, 0}, pad[2] = {0, 0};
      bool hasStick = false, hasPad = false, stickClick = false, padClick = false, primary = false, secondary = false, menu = false;
      std::string type;
      if (ok) {
        char buf[128] = {};
        system_->GetStringTrackedDeviceProperty(idx, vr::Prop_ControllerType_String, buf, sizeof(buf));
        type = buf;
        vr::VRControllerState_t st{};
        const bool hasState = system_->GetControllerState(idx, &st, sizeof(st));
        // ビームの向き（WebXR の targetRaySpace）はレンダーモデルの "tip"、握り（gripSpace）は "handgrip" の姿勢。
        // 部品が取れないコントローラはデバイスの姿勢をそのまま使う
        const auto& dev = poses_[idx].mDeviceToAbsoluteTracking;
        auto aimM = ComponentPose(idx, vr::k_pch_Controller_Component_Tip, hasState ? &st : nullptr, dev);
        auto gripM = ComponentPose(idx, vr::k_pch_Controller_Component_HandGrip, hasState ? &st : nullptr, dev);
        auto grip = Napi::Object::New(env);
        SetPose(env, grip, ToPose(gripM));
        auto aim = Napi::Object::New(env);
        SetPose(env, aim, ToPose(aimM));
        h.Set("grip", grip);
        h.Set("aim", aim);
        if (hasState) {
          int triggers = 0;
          for (int a = 0; a < static_cast<int>(vr::k_unControllerStateAxisCount); ++a) {
            int at = system_->GetInt32TrackedDeviceProperty(idx, static_cast<vr::ETrackedDeviceProperty>(vr::Prop_Axis0Type_Int32 + a));
            const auto& ax = st.rAxis[a];
            const bool pressed = (st.ulButtonPressed & vr::ButtonMaskFromId(static_cast<vr::EVRButtonId>(vr::k_EButton_Axis0 + a))) != 0;
            if (at == vr::k_eControllerAxis_Trigger) {
              if (triggers++ == 0) trigger = ax.x;
              else squeeze = ax.x;
            } else if (at == vr::k_eControllerAxis_Joystick && !hasStick) {
              stick[0] = ax.x;
              stick[1] = ax.y;
              hasStick = true;
              stickClick = pressed;
            } else if (at == vr::k_eControllerAxis_TrackPad && !hasPad) {
              pad[0] = ax.x;
              pad[1] = ax.y;
              hasPad = true;
              padClick = pressed;
            }
          }
          auto down = [&](int id) { return (st.ulButtonPressed & vr::ButtonMaskFromId(static_cast<vr::EVRButtonId>(id))) != 0; };
          if (type == "knuckles") {
            primary = down(vr::k_EButton_IndexController_A);
            secondary = down(vr::k_EButton_IndexController_B);
          } else {
            primary = down(vr::k_EButton_A);
            secondary = down(vr::k_EButton_ApplicationMenu);
            if (type == "vive_controller" || type == "holographic_controller") {
              menu = secondary;
              secondary = false;
            }
            if (triggers < 2 && down(vr::k_EButton_Grip)) squeeze = 1.0;
          }
        }
      } else {
        h.Set("grip", env.Null());
        h.Set("aim", env.Null());
      }
      auto toArr = [&](double* v) {
        auto a = Napi::Float64Array::New(env, 2);
        a[0] = v[0];
        a[1] = v[1];
        return a;
      };
      h.Set("connected", ok);
      h.Set("profile", ok ? ProfileFor(type) : "");
      h.Set("trigger", trigger);
      h.Set("squeeze", squeeze);
      h.Set("thumbstick", toArr(stick));
      h.Set("trackpad", toArr(pad));
      h.Set("hasThumbstick", hasStick);
      h.Set("hasTrackpad", hasPad);
      h.Set("thumbstickClick", stickClick);
      h.Set("trackpadClick", padClick);
      h.Set("primary", primary);
      h.Set("secondary", secondary);
      h.Set("menu", menu);
      out.Set(i, h);
    }
    return out;
  }

  // デバイスの姿勢 × レンダーモデルの部品（tip / handgrip）の局所変換。取れなければデバイスの姿勢
  vr::HmdMatrix34_t ComponentPose(vr::TrackedDeviceIndex_t idx, const char* component, const vr::VRControllerState_t* st,
                                  const vr::HmdMatrix34_t& device) {
    auto* models = vr::VRRenderModels();
    if (!models || !st) return device;
    char name[256] = {};
    system_->GetStringTrackedDeviceProperty(idx, vr::Prop_RenderModelName_String, name, sizeof(name));
    if (!name[0]) return device;
    vr::RenderModel_ControllerMode_State_t mode{};
    vr::RenderModel_ComponentState_t cs{};
    if (!models->GetComponentState(name, component, st, &mode, &cs)) return device;
    return Mul(device, cs.mTrackingToComponentLocal);
  }

  // 描画終了タグを持たない同期パスの画像で、前回のタグを誤用しないようにする
  void ForgetUploadTiming() {
    lastUploadTagMs_ = 0;
    lastUploadDoneMs_ = 0;
    lastUploadSeq_ = 0;
  }

  // 両眼の画素をステージングへ並列コピー → 目のテクスチャへ GPU コピー → 姿勢付きで Submit
  void Upload(Napi::Env env, const uint8_t* data, const size_t offs[2], uint32_t pitch, const vr::HmdMatrix34_t& pose) {
    const int slot = slot_;
    slot_ ^= 1;
    D3D11_MAPPED_SUBRESOURCE mapped[2] = {};
    for (int eye = 0; eye < 2; ++eye) {
      if (FAILED(context_->Map(staging_[eye][slot], 0, D3D11_MAP_WRITE, 0, &mapped[eye]))) {
        if (eye == 1) context_->Unmap(staging_[0][slot], 0);
        throw Napi::Error::New(env, "Map(staging) failed");
      }
    }
    auto copyEye = [&](int eye) {
      const uint8_t* src = data + offs[eye];
      auto* dst = static_cast<uint8_t*>(mapped[eye].pData);
      const size_t row = static_cast<size_t>(width_) * 4;
      for (int y = 0; y < height_; ++y) memcpy(dst + y * mapped[eye].RowPitch, src + static_cast<size_t>(y) * pitch, row);
    };
    std::thread other(copyEye, 1);
    copyEye(0);
    other.join();
    for (int eye = 0; eye < 2; ++eye) {
      context_->Unmap(staging_[eye][slot], 0);
      context_->CopyResource(eyeTex_[eye][slot], staging_[eye][slot]);
    }
    for (int eye = 0; eye < 2; ++eye) {
      vr::VRTextureWithPose_t tex{};
      tex.handle = eyeTex_[eye][slot];
      tex.eType = vr::TextureType_DirectX;
      tex.eColorSpace = vr::ColorSpace_Auto;
      tex.mDeviceToAbsoluteTracking = pose;
      vr::VRTextureBounds_t bounds{0.0f, 0.0f, 1.0f, 1.0f};
      compositor_->Submit(static_cast<vr::EVREye>(eye), &tex, &bounds, vr::Submit_TextureWithPose);
    }
    lastSlot_ = slot;
    ForgetUploadTiming();
    hasImage_ = true;
    submittedThisFrame_ = true;
  }

  // 別スレッドでステージングへのコピーと GPU コピーまで行い、Submit だけをメインスレッドで行う。
  // ArrayBuffer は Promise が解決するまで有効であること（WebGPU の mapped range をそのまま渡せる）
  class UploadWorker : public Napi::AsyncWorker {
   public:
    UploadWorker(Napi::Env env, VrSessionWrap* s, Napi::ArrayBuffer ab, size_t offL, size_t offR, uint32_t pitch, uint64_t seq, double tagMs)
        : Napi::AsyncWorker(env), s_(s), d_(Napi::Promise::Deferred::New(env)), offs_{offL, offR}, pitch_(pitch), seq_(seq), tagMs_(tagMs) {
      ref_ = Napi::Persistent(ab);
      data_ = static_cast<const uint8_t*>(ab.Data());
      pose_ = s->hmdRing_[seq % kRing];
      // 提出中の（最新の）スロットには書かない
      std::lock_guard<std::mutex> lk(s->frameMtx_);
      slot_ = s->hasImage_ ? (s->lastSlot_ ^ 1) : 0;
    }
    Napi::Promise Promise() { return d_.Promise(); }
    void Execute() override {
      [[maybe_unused]] FlagGuard running{s_->uploadRunning_};
      t0_ = NowMs();
      auto* ctx = s_->context_;
      D3D11_MAPPED_SUBRESOURCE mapped[2] = {};
      for (int eye = 0; eye < 2; ++eye) {
        if (FAILED(ctx->Map(s_->staging_[eye][slot_], 0, D3D11_MAP_WRITE, 0, &mapped[eye]))) {
          if (eye == 1) ctx->Unmap(s_->staging_[0][slot_], 0);
          SetError("Map(staging) failed");
          return;
        }
      }
      const int w = s_->width_, h = s_->height_;
      auto copyEye = [&](int eye) {
        const uint8_t* src = data_ + offs_[eye];
        auto* dst = static_cast<uint8_t*>(mapped[eye].pData);
        for (int y = 0; y < h; ++y) memcpy(dst + y * mapped[eye].RowPitch, src + static_cast<size_t>(y) * pitch_, static_cast<size_t>(w) * 4);
      };
      t1_ = NowMs();
      std::thread other(copyEye, 1);
      copyEye(0);
      other.join();
      t2_ = NowMs();
      for (int eye = 0; eye < 2; ++eye) {
        ctx->Unmap(s_->staging_[eye][slot_], 0);
        ctx->CopyResource(s_->eyeTex_[eye][slot_], s_->staging_[eye][slot_]);
      }
      ctx->Flush();
      t3_ = NowMs();
      // 転送できたら「最新の画像」として公開し、提出を待っているフレーム待ちスレッドを起こす
      {
        std::lock_guard<std::mutex> lk(s_->frameMtx_);
        s_->lastSlot_ = slot_;
        s_->lastPose_ = pose_;
        s_->lastUploadTagMs_ = tagMs_;
        s_->lastUploadDoneMs_ = NowMs();
        s_->lastUploadSeq_ = seq_;
        s_->hasImage_ = true;
        s_->latchedMode_ = true;
        s_->publishedGen_++;
        s_->uploadPending_ = false;
      }
      s_->frameCv_.notify_all();
    }
    void OnOK() override {
      s_->uploadBusy_ = false;
      auto r = Napi::Object::New(Env());
      r.Set("mapWaitMs", t1_ - t0_);
      r.Set("memcpyMs", t2_ - t1_);
      // CopyResource と Flush の CPU 時間。GPU コピーの完了時間ではない
      r.Set("copyResourceCpuMs", t3_ - t2_);
      d_.Resolve(r);
    }
    void OnError(const Napi::Error& e) override {
      s_->uploadBusy_ = false;
      {
        std::lock_guard<std::mutex> lk(s_->frameMtx_);
        s_->uploadPending_ = false;
      }
      s_->frameCv_.notify_all();
      d_.Reject(e.Value());
    }

   private:
    VrSessionWrap* s_;
    Napi::Promise::Deferred d_;
    Napi::Reference<Napi::ArrayBuffer> ref_;
    const uint8_t* data_ = nullptr;
    size_t offs_[2];
    uint32_t pitch_;
    uint64_t seq_;
    double tagMs_;
    vr::HmdMatrix34_t pose_{};
    int slot_ = 0;
    double t0_ = 0, t1_ = 0, t2_ = 0, t3_ = 0;
  };

  // submitLatchedAsync(pixels, offsetLeft, offsetRight, rowPitch, seq, tagMs)
  // tagMs は描画終了時刻（QPC ミリ秒）。計測だけに使い、省略時は 0（計測なし）
  Napi::Value SubmitLatchedAsync(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    EnsureAlive(env);
    if (!info[0].IsArrayBuffer()) throw Napi::TypeError::New(env, "pixels must be an ArrayBuffer");
    auto ab = info[0].As<Napi::ArrayBuffer>();
    size_t offs[2] = {static_cast<size_t>(info[1].ToNumber().Int64Value()), static_cast<size_t>(info[2].ToNumber().Int64Value())};
    uint32_t pitch = info[3].ToNumber().Uint32Value();
    uint64_t seq = static_cast<uint64_t>(info[4].ToNumber().Int64Value());
    double tagMs = info.Length() > 5 ? info[5].ToNumber().DoubleValue() : 0.0;
    CheckBuffer(env, ab.ByteLength(), offs, pitch, width_, height_);
    if (uploadBusy_) throw Napi::Error::New(env, "an upload is already in flight");
    uploadBusy_ = true;
    auto* w = new UploadWorker(env, this, ab, offs[0], offs[1], pitch, seq, tagMs);
    auto p = w->Promise();
    uploadRunning_ = true;
    w->Queue();
    return p;
  }

  // 直近に転送し終えた両眼の画像を、それを描いたときの姿勢で提出する（毎フレーム WaitGetPoses 直後に呼ぶ）
  // 2 回目の Submit が SteamVR の frame ready なので、描画終了タグからの経過をそこで記録する
  SubmitTiming SubmitLatest() {
    SubmitTiming timing;
    if (!hasImage_ || !initialized_) return timing;
    timing.ran = true;
    timing.seq = lastUploadSeq_;
    const double submitStartMs = NowMs();
    for (int eye = 0; eye < 2; ++eye) {
      if (eye == 1) timing.submitAtMs = NowMs();
      vr::VRTextureWithPose_t tex{};
      tex.handle = eyeTex_[eye][lastSlot_];
      tex.eType = vr::TextureType_DirectX;
      tex.eColorSpace = vr::ColorSpace_Auto;
      tex.mDeviceToAbsoluteTracking = lastPose_;
      vr::VRTextureBounds_t bounds{0.0f, 0.0f, 1.0f, 1.0f};
      const double submitMs = NowMs();
      compositor_->Submit(static_cast<vr::EVREye>(eye), &tex, &bounds, vr::Submit_TextureWithPose);
      if (eye == 0) timing.submitFirstMs = NowMs() - submitMs;
    }
    const double handoffStartMs = NowMs();
    compositor_->PostPresentHandoff();
    timing.handoffMs = NowMs() - handoffStartMs;
    timing.submitCallMs = NowMs() - submitStartMs;
    if (lastUploadTagMs_ > 0) {
      timing.valid = true;
      timing.renderToSubmitMs = timing.submitAtMs - lastUploadTagMs_;
      timing.uploadToSubmitMs = lastUploadDoneMs_ > 0 ? timing.submitAtMs - lastUploadDoneMs_ : 0;
    }
    submittedThisFrame_ = true;
    submittedSinceWait_ = true;
    submittedGen_ = publishedGen_;
    return timing;
  }

  Napi::Value FillFrame(const Napi::CallbackInfo& info) { return info.Env().Undefined(); }

  // markPending(true): このフレームの画像を読み戻し中（提出はその転送完了を締め切りまで待つ）
  // markPending(false): 読み戻しを諦めた（待たずに最新の画像を出してよい）
  Napi::Value MarkPending(const Napi::CallbackInfo& info) {
    {
      std::lock_guard<std::mutex> lk(frameMtx_);
      uploadPending_ = info[0].ToBoolean().Value();
    }
    frameCv_.notify_all();
    return info.Env().Undefined();
  }

  // setLatencyMode(immediate: boolean)
  Napi::Value SetLatencyMode(const Napi::CallbackInfo& info) {
    immediate_ = info[0].ToBoolean().Value();
    return info.Env().Undefined();
  }

  static void CheckBuffer(Napi::Env env, size_t len, const size_t offs[2], uint32_t pitch, int w, int h) {
    size_t need = static_cast<size_t>(pitch) * (h - 1) + static_cast<size_t>(w) * 4;
    if (pitch < static_cast<uint32_t>(w) * 4 || offs[0] + need > len || offs[1] + need > len) throw Napi::RangeError::New(env, "pixel buffer too small");
  }

  static const uint8_t* BufferData(Napi::Env env, const Napi::Value& v, size_t* len) {
    if (v.IsArrayBuffer()) {
      auto ab = v.As<Napi::ArrayBuffer>();
      *len = ab.ByteLength();
      return static_cast<const uint8_t*>(ab.Data());
    }
    if (v.IsTypedArray()) {
      auto ta = v.As<Napi::TypedArray>();
      *len = ta.ByteLength();
      return static_cast<const uint8_t*>(ta.ArrayBuffer().Data()) + ta.ByteOffset();
    }
    throw Napi::TypeError::New(env, "pixels must be ArrayBuffer or TypedArray");
  }

  // submitLatched(pixels, offsetLeft, offsetRight, rowPitch, seq)
  Napi::Value SubmitLatched(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    EnsureAlive(env);
    size_t len = 0;
    const uint8_t* data = BufferData(env, info[0], &len);
    size_t offs[2] = {static_cast<size_t>(info[1].ToNumber().Int64Value()), static_cast<size_t>(info[2].ToNumber().Int64Value())};
    uint32_t pitch = info[3].ToNumber().Uint32Value();
    uint64_t seq = static_cast<uint64_t>(info[4].ToNumber().Int64Value());
    CheckBuffer(env, len, offs, pitch, width_, height_);
    Upload(env, data, offs, pitch, hmdRing_[seq % kRing]);
    return env.Undefined();
  }

  // submit(eye, pixels, offset, rowPitch): 片眼ずつ（Node クライアント用）。両眼そろったら提出する
  Napi::Value Submit(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    EnsureAlive(env);
    int eye = info[0].ToNumber().Int32Value();
    size_t len = 0;
    const uint8_t* data = BufferData(env, info[1], &len);
    size_t off = info.Length() > 2 ? static_cast<size_t>(info[2].ToNumber().Int64Value()) : 0;
    uint32_t pitch = info.Length() > 3 ? info[3].ToNumber().Uint32Value() : width_ * 4;
    if (eye < 0 || eye > 1) throw Napi::RangeError::New(env, "eye must be 0 or 1");
    pending_[eye] = data;
    pendingOff_[eye] = off;
    pendingPitch_ = pitch;
    pendingLen_[eye] = len;
    if (pending_[0] && pending_[1]) {
      // 片眼ずつ別のバッファで来ても良いように、目ごとにコピーする
      for (int e = 0; e < 2; ++e) {
        size_t offs[2] = {pendingOff_[e], pendingOff_[e]};
        CheckBuffer(env, pendingLen_[e], offs, pitch, width_, height_);
      }
      const int slot = slot_;
      slot_ ^= 1;
      for (int e = 0; e < 2; ++e) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(context_->Map(staging_[e][slot], 0, D3D11_MAP_WRITE, 0, &m))) throw Napi::Error::New(env, "Map(staging) failed");
        for (int y = 0; y < height_; ++y)
          memcpy(static_cast<uint8_t*>(m.pData) + y * m.RowPitch, pending_[e] + pendingOff_[e] + static_cast<size_t>(y) * pitch, width_ * 4);
        context_->Unmap(staging_[e][slot], 0);
        context_->CopyResource(eyeTex_[e][slot], staging_[e][slot]);
      }
      for (int e = 0; e < 2; ++e) {
        vr::VRTextureWithPose_t tex{};
        tex.handle = eyeTex_[e][slot];
        tex.eType = vr::TextureType_DirectX;
        tex.eColorSpace = vr::ColorSpace_Auto;
        tex.mDeviceToAbsoluteTracking = hmdRing_[seq_ % kRing];
        vr::VRTextureBounds_t bounds{0.0f, 0.0f, 1.0f, 1.0f};
        compositor_->Submit(static_cast<vr::EVREye>(e), &tex, &bounds, vr::Submit_TextureWithPose);
      }
      pending_[0] = pending_[1] = nullptr;
      ForgetUploadTiming();
      hasImage_ = true;
      submittedThisFrame_ = true;
    }
    return env.Undefined();
  }

  Napi::Value EndFrame(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    EnsureAlive(env);
    bool submitted = submittedThisFrame_;
    submittedThisFrame_ = false;
    if (submitted && !latchedMode_) compositor_->PostPresentHandoff();
    return Napi::Boolean::New(env, submitted || hasImage_);
  }

  Napi::Value ApplyHaptics(const Napi::CallbackInfo& info) {
    if (!initialized_) return info.Env().Undefined();
    int hand = info[0].ToNumber().Int32Value();
    if (hand < 0 || hand > 1 || hand_[hand] == vr::k_unTrackedDeviceIndexInvalid) return info.Env().Undefined();
    double amp = std::clamp(info[1].ToNumber().DoubleValue(), 0.0, 1.0);
    double ms = info[2].ToNumber().DoubleValue();
    // レガシー API は最大 3999μs のパルス。強さは長さで近似する
    auto us = static_cast<unsigned short>(std::clamp(amp * std::min(ms, 4.0) * 1000.0, 0.0, 3999.0));
    system_->TriggerHapticPulse(hand_[hand], 0, us);
    return info.Env().Undefined();
  }

  Napi::Value RequestExit(const Napi::CallbackInfo& info) {
    exitRequested_ = true;
    return info.Env().Undefined();
  }

  Napi::Value Destroy(const Napi::CallbackInfo& info) {
    Cleanup();
    return info.Env().Undefined();
  }

  static constexpr int kRing = 8;
  vr::IVRSystem* system_ = nullptr;
  vr::IVRCompositor* compositor_ = nullptr;
  bool initialized_ = false;
  bool started_ = false;
  bool exitRequested_ = false;
  bool hasImage_ = false;
  bool submittedThisFrame_ = false;
  bool uploadBusy_ = false;
  bool latchedMode_ = false;
  bool immediate_ = false;
  bool submittedSinceWait_ = false;
  bool waitInFlight_ = false;
  std::atomic<bool> waitRunning_{false};
  std::atomic<bool> uploadRunning_{false};
  uint64_t onTimeSubmits_ = 0, lateSubmits_ = 0;
  uint64_t publishedGen_ = 0, submittedGen_ = 0;
  double lastUploadTagMs_ = 0;   // JS が付けた描画終了時刻（QPC ミリ秒）
  double lastUploadDoneMs_ = 0;  // 転送が完了して公開した時刻（QPC ミリ秒）
  uint64_t lastUploadSeq_ = 0;
  // フレーム待ちスレッドと転送スレッドの受け渡し
  double submitMarginMs_ = 4.0;  // 次のフレーム開始の何 ms 前を提出の締め切りにするか（実測で決定。XRIFT_SUBMIT_MARGIN_MS で変更可）
  std::mutex frameMtx_;
  std::condition_variable frameCv_;
  bool uploadPending_ = false;
  bool haveTiming_ = false;
  std::chrono::steady_clock::time_point lastWaitReturn_{};
  float displayHz_ = 90.0f;
  vr::HmdMatrix34_t lastPose_{};
  vr::TrackedDevicePose_t poses_[vr::k_unMaxTrackedDeviceCount] = {};
  vr::HmdMatrix34_t hmdRing_[kRing] = {};
  vr::HmdMatrix34_t eyeToHead_[2] = {};
  double fov_[2][4] = {};
  vr::TrackedDeviceIndex_t hand_[2] = {vr::k_unTrackedDeviceIndexInvalid, vr::k_unTrackedDeviceIndexInvalid};
  uint64_t seq_ = 0;
  ID3D11Device* device_ = nullptr;
  ID3D11DeviceContext* context_ = nullptr;
  ID3D11Texture2D* eyeTex_[2][2] = {};
  ID3D11Texture2D* staging_[2][2] = {};
  int slot_ = 0;
  int lastSlot_ = 0;
  int width_ = 0, height_ = 0;
  uint32_t recW_ = 0, recH_ = 0;
  const uint8_t* pending_[2] = {nullptr, nullptr};
  size_t pendingOff_[2] = {0, 0};
  size_t pendingLen_[2] = {0, 0};
  uint32_t pendingPitch_ = 0;
  std::string systemName_, adapterName_;
};

void InitOpenVR(Napi::Env env, Napi::Object exports) { VrSessionWrap::Init(env, exports); }
