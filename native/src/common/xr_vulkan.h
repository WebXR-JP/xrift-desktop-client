#pragma once

// Vulkan-Hpp (vulkan.hpp + vk::raii) と OpenXR の共通 include。読み順をここに集約する。
// openxr_platform.h は Vk* 型を自前で include しないため、vulkan.hpp を先に読む順序が必須。
// 逆にすると XrGraphicsBindingVulkanKHR 等の宣言が壊れる。
// VK_NO_PROTOTYPES と XR_USE_GRAPHICS_API_VULKAN と VULKAN_HPP_DISPATCH_LOADER_DYNAMIC は
// CMake が全 TU に同一定義する。ここでの再定義は行わない (定義ブレ防止)。
// 拡張関数 (将来の VK_KHR_external_memory_win32 等) を使う場合も、このヘッダ経由の
// 動的ディスパッチャ (vk::raii::Context 起点) で取得する。
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
