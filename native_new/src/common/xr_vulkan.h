#pragma once

// Vulkan と OpenXR の共通 include。読み順をここに集約する。
// openxr_platform.h は Vk* 型を自前で include しないため、volk.h を先に読む順序が必須。
// 逆にすると XrGraphicsBindingVulkanKHR 等の宣言が壊れる。
// VK_NO_PROTOTYPES と XR_USE_GRAPHICS_API_VULKAN は CMake が全体に定義する。
#include <volk.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
