#include "app/probe.h"

#include <vector>

#include "common/log.h"
#include "gfx/swapchains.h"
#include "gfx/vulkan_context.h"
#include "xr/instance.h"

namespace xrift::app {

int Probe() {
  try {
    xr::Instance xr;
    gfx::VulkanContext vk(xr);

    const XrInstanceProperties& props = xr.instanceProperties();
    Info("runtime: %s (%u.%u.%u)", props.runtimeName,
         static_cast<unsigned>(XR_VERSION_MAJOR(props.runtimeVersion)),
         static_cast<unsigned>(XR_VERSION_MINOR(props.runtimeVersion)),
         static_cast<unsigned>(XR_VERSION_PATCH(props.runtimeVersion)));

    const XrSystemProperties& system = xr.systemProperties();
    Info("system: %s (vendor %u)", system.systemName,
         static_cast<unsigned>(system.vendorId));
    Info("system 上限: 画像 %ux%u, レイヤー %u", system.graphicsProperties.maxSwapchainImageWidth,
         system.graphicsProperties.maxSwapchainImageHeight,
         static_cast<unsigned>(system.graphicsProperties.maxLayerCount));

    Info("GPU: %s", vk.gpuName().c_str());

    const std::vector<XrViewConfigurationView> views = xr.QueryStereoViews();
    for (size_t i = 0; i < views.size(); ++i) {
      Info("view[%u]: 推奨 %ux%u, 最大 %ux%u", static_cast<unsigned>(i),
           static_cast<unsigned>(views[i].recommendedImageRectWidth),
           static_cast<unsigned>(views[i].recommendedImageRectHeight),
           static_cast<unsigned>(views[i].maxImageRectWidth),
           static_cast<unsigned>(views[i].maxImageRectHeight));
    }

    // フォーマット列挙には session が要るため、ここでは優先候補の表示に留める。
    Info("swapchain format 候補（優先順）:");
    for (vk::Format format : gfx::PreferredSwapchainFormats()) {
      Info("  %s", gfx::SwapchainFormatName(format));
    }
    Info("probe 成功: セッションは作成していません");
    return 0;
  } catch (const std::exception& e) {
    Error("%s", e.what());
    return 1;
  }
}

}  // namespace xrift::app
