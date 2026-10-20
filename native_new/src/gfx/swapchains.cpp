#include "gfx/swapchains.h"

#include <algorithm>
#include <stdexcept>

#include "common/log.h"
#include "common/xr_result.h"

namespace xrift::gfx {

std::vector<vk::Format> PreferredSwapchainFormats() {
  return {
      vk::Format::eR8G8B8A8Srgb,
      vk::Format::eB8G8R8A8Srgb,
      vk::Format::eR8G8B8A8Unorm,
      vk::Format::eB8G8R8A8Unorm,
  };
}

const char* SwapchainFormatName(vk::Format format) {
  switch (format) {
    case vk::Format::eR8G8B8A8Srgb:
      return "VK_FORMAT_R8G8B8A8_SRGB";
    case vk::Format::eB8G8R8A8Srgb:
      return "VK_FORMAT_B8G8R8A8_SRGB";
    case vk::Format::eR8G8B8A8Unorm:
      return "VK_FORMAT_R8G8B8A8_UNORM";
    case vk::Format::eB8G8R8A8Unorm:
      return "VK_FORMAT_B8G8R8A8_UNORM";
    // ランタイムは深度形式も列挙するため、診断用に名前を用意する。
    case vk::Format::eD16Unorm:
      return "VK_FORMAT_D16_UNORM";
    case vk::Format::eX8D24UnormPack32:
      return "VK_FORMAT_X8_D24_UNORM_PACK32";
    case vk::Format::eD32Sfloat:
      return "VK_FORMAT_D32_SFLOAT";
    case vk::Format::eS8Uint:
      return "VK_FORMAT_S8_UINT";
    case vk::Format::eD16UnormS8Uint:
      return "VK_FORMAT_D16_UNORM_S8_UINT";
    case vk::Format::eD24UnormS8Uint:
      return "VK_FORMAT_D24_UNORM_S8_UINT";
    case vk::Format::eD32SfloatS8Uint:
      return "VK_FORMAT_D32_SFLOAT_S8_UINT";
    default:
      return "不明なフォーマット";
  }
}

Swapchains::Swapchains(XrInstance instance, XrSession session,
                       const std::vector<XrViewConfigurationView>& views)
    : instance_(instance) {
  // コンストラクタが例外で抜けるとデストラクタは呼ばれないため、生成済み swapchain は
  // ここで破棄する。Destroy() は冪等であり、部分構築状態でも安全に呼べる。
  try {
    // ランタイム対応のフォーマットを列挙し、優先順で 1 つ選ぶ。
    // XrSwapchainCreateInfo::format は int64_t のため、比較と代入は境界でのみ変換する。
    uint32_t format_count = 0;
    CheckXr(xrEnumerateSwapchainFormats(session, 0, &format_count, nullptr),
            "xrEnumerateSwapchainFormats(件数)");
  std::vector<int64_t> formats(format_count);
  CheckXr(xrEnumerateSwapchainFormats(session, format_count, &format_count, formats.data()),
          "xrEnumerateSwapchainFormats(列挙)");
  formats.resize(format_count);
  for (int64_t available : formats) {
    Info("  swapchain format 対応: %s", SwapchainFormatName(static_cast<vk::Format>(available)));
  }
  for (vk::Format wanted : PreferredSwapchainFormats()) {
    if (std::find(formats.begin(), formats.end(), static_cast<int64_t>(wanted)) != formats.end()) {
      format_ = wanted;
      break;
    }
  }
  if (format_ == vk::Format::eUndefined) {
    throw std::runtime_error("対応する swapchain フォーマットがありません");
  }
  Info("swapchain format: %s", SwapchainFormatName(format_));

  if (views.size() != 2) {
    throw std::runtime_error("view 数が 2 ではありません");
  }
  eyes_.reserve(views.size());
  for (size_t i = 0; i < views.size(); ++i) {
    // extent の決定は swapchain 作成側の責務とし、推奨値を最大値で抑える。
    const uint32_t width =
        std::min(views[i].recommendedImageRectWidth, views[i].maxImageRectWidth);
    const uint32_t height =
        std::min(views[i].recommendedImageRectHeight, views[i].maxImageRectHeight);
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags =
        XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = static_cast<int64_t>(format_);
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    Eye eye;
    CheckXr(xrCreateSwapchain(session, &info, &eye.swapchain), "xrCreateSwapchain");
    eye.width = width;
    eye.height = height;
    // 画像列挙の失敗に備え、生成直後に登録する。失敗時は外側の catch が Destroy() で破棄する。
    eyes_.push_back(std::move(eye));
    Eye& created = eyes_.back();
    uint32_t image_count = 0;
    CheckXr(xrEnumerateSwapchainImages(created.swapchain, 0, &image_count, nullptr),
            "xrEnumerateSwapchainImages(件数)");
    created.images.resize(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    CheckXr(xrEnumerateSwapchainImages(
                created.swapchain, image_count, &image_count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(created.images.data())),
            "xrEnumerateSwapchainImages(列挙)");
    created.images.resize(image_count);
    Info("眼 %u: %ux%u, 画像 %u 枚", static_cast<unsigned>(i), static_cast<unsigned>(width),
         static_cast<unsigned>(height), static_cast<unsigned>(image_count));
  }
  } catch (...) {
    Destroy();
    throw;
  }
}

Swapchains::~Swapchains() { Destroy(); }

void Swapchains::Destroy() {
  for (Eye& eye : eyes_) {
    if (eye.swapchain != XR_NULL_HANDLE) {
      const XrResult result = xrDestroySwapchain(eye.swapchain);
      if (result != XR_SUCCESS) {
        Warn("xrDestroySwapchain に失敗: %s", XrResultToString(result));
      }
      eye.swapchain = XR_NULL_HANDLE;
    }
    eye.images.clear();
  }
}

}  // namespace xrift::gfx
