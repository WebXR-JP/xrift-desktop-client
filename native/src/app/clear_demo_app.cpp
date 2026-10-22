#include "app/clear_demo_app.h"

#include <chrono>
#include <thread>

#include "common/log.h"
#include "common/xr_result.h"

namespace xrift {

XrApp::XrApp() { views_.resize(2, {XR_TYPE_VIEW}); }

XrApp::~XrApp() { Shutdown(); }

int XrApp::Run() {
  instance_ = std::make_unique<xr::Instance>();
  vulkan_ = std::make_unique<gfx::VulkanContext>(*instance_);
  session_ = std::make_unique<xr::Session>(*instance_, vulkan_->xrGraphicsBinding());
  swapchains_ = std::make_unique<gfx::Swapchains>(instance_->handle(), session_->handle(),
                                                 session_->views());
  clear_pass_ = std::make_unique<gfx::ClearPass>(vulkan_->device(), vulkan_->queueFamilyIndex(),
                                                vulkan_->queue(), *swapchains_);

  layer_.space = session_->space();
  layer_.viewCount = 2;
  layer_.views = proj_views_;

  Info("起動: HMD に両眼クリア色を表示します");
  while (!quit_) {
    PollEvents();
    if (!session_running_) {
      // 実行中でない間は眠らせ、ビジーループにしない。
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    Frame();
  }
  Shutdown();
  return 0;
}

void XrApp::PollEvents() {
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  while (xrPollEvent(instance_->handle(), &event) == XR_SUCCESS) {
    switch (event.type) {
      case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
        const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
        if (changed->session != session_->handle()) {
          break;
        }
        session_->setState(changed->state);
        Info("session 状態: %d", static_cast<int>(changed->state));
        switch (changed->state) {
          case XR_SESSION_STATE_READY:
            if (!session_running_) {
              session_->Begin();
              session_running_ = true;
              Info("セッション開始");
            }
            break;
          case XR_SESSION_STATE_STOPPING:
            if (session_running_) {
              session_->End();
              session_running_ = false;
              Info("セッション終了");
            }
            break;
          case XR_SESSION_STATE_EXITING:
          case XR_SESSION_STATE_LOSS_PENDING:
            quit_ = true;
            break;
          default:
            break;
        }
        break;
      }
      case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
        quit_ = true;
        break;
      case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
        Info("interaction profile 変更（未対応のため記録のみ）");
        break;
      case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
        Info("reference space 変更予定（未対応のため記録のみ）");
        break;
      default:
        Info("未処理のイベント種別: %d", static_cast<int>(event.type));
        break;
    }
    event = {XR_TYPE_EVENT_DATA_BUFFER};
  }
}

void XrApp::Frame() {
  XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
  XrFrameState frame_state{XR_TYPE_FRAME_STATE};
  CheckXr(xrWaitFrame(session_->handle(), &wait_info, &frame_state), "xrWaitFrame");
  XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
  CheckXr(xrBeginFrame(session_->handle(), &begin_info), "xrBeginFrame");

  bool rendered = false;
  if (frame_state.shouldRender == XR_TRUE) {
    XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
    locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate_info.displayTime = frame_state.predictedDisplayTime;
    locate_info.space = session_->space();
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    uint32_t view_count = 0;
    CheckXr(xrLocateViews(session_->handle(), &locate_info, &view_state,
                          static_cast<uint32_t>(views_.size()), &view_count, views_.data()),
            "xrLocateViews");
    const bool pose_valid =
        (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0 &&
        (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
    if (view_count == views_.size() && pose_valid) {
      for (uint32_t eye = 0; eye < swapchains_->eyeCount(); ++eye) {
        clear_pass_->Draw(eye);
        proj_views_[eye].pose = views_[eye].pose;
        proj_views_[eye].fov = views_[eye].fov;
        proj_views_[eye].subImage.swapchain = swapchains_->handle(eye);
        proj_views_[eye].subImage.imageRect.offset = {0, 0};
        proj_views_[eye].subImage.imageRect.extent.width =
            static_cast<int32_t>(swapchains_->extent(eye).width);
        proj_views_[eye].subImage.imageRect.extent.height =
            static_cast<int32_t>(swapchains_->extent(eye).height);
        proj_views_[eye].subImage.imageArrayIndex = 0;
      }
      rendered = true;
    }
  }

  XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
  end_info.displayTime = frame_state.predictedDisplayTime;
  end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  const XrCompositionLayerBaseHeader* layers[] = {
      reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer_)};
  // 描画しないフレームでも xrEndFrame は必ず呼ぶ。swapchain の acquire は行わない。
  if (rendered) {
    end_info.layerCount = 1;
    end_info.layers = layers;
  } else {
    end_info.layerCount = 0;
  }
  CheckXr(xrEndFrame(session_->handle(), &end_info), "xrEndFrame");
}

void XrApp::Shutdown() {
  // 破棄は子から親へ行う。swapchain は session より先に破棄し、
  // VkDevice/VkQueue を使うものは VkDevice より先に破棄する。
  // unique_ptr の reset と各 Destroy は冪等であるため、二重呼び出しは安全である。
  clear_pass_.reset();
  swapchains_.reset();
  session_.reset();
  vulkan_.reset();
  instance_.reset();
}

}  // namespace xrift
