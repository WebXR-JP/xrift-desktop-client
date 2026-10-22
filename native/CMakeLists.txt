cmake_minimum_required(VERSION 3.24)

project(xrift_native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 依存の取得定義（OpenXR-SDK-Source / Vulkan-Headers）。
include(cmake/Dependencies.cmake)

add_executable(xrift_native
  src/main.cpp
  src/app/clear_demo_app.cpp
  src/common/log.cpp
  src/common/xr_result.cpp
  src/xr/instance.cpp
  src/xr/session.cpp
  src/xr/input.cpp
  src/gfx/vulkan_context.cpp
  src/gfx/swapchains.cpp
  src/gfx/clear_pass.cpp
  src/transfer/pipe_client.cpp
  src/transfer/shared_texture.cpp
  src/app/xr_transfer_app.cpp
  ../tools/selftest/native/transfer_selftest.cpp
)

target_include_directories(xrift_native PRIVATE src "${CMAKE_CURRENT_SOURCE_DIR}/../tools/selftest/native")

target_compile_definitions(xrift_native PRIVATE
  VK_NO_PROTOTYPES
  XR_USE_GRAPHICS_API_VULKAN
  VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1
)

if(WIN32)
  target_compile_definitions(xrift_native PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX VK_USE_PLATFORM_WIN32_KHR)
endif()

if(MSVC)
  target_compile_options(xrift_native PRIVATE /utf-8 /W4 /EHsc)
else()
  target_compile_options(xrift_native PRIVATE -Wall -Wextra -Wpedantic)
endif()

# OpenXR::openxr_loader は SDK が提供する alias。存在しない版に備えて素の名前に退避する。
if(TARGET OpenXR::openxr_loader)
  target_link_libraries(xrift_native PRIVATE OpenXR::openxr_loader)
else()
  target_link_libraries(xrift_native PRIVATE openxr_loader)
endif()

# Vulkan-Hpp はヘッダオンリーのため Vulkan::Headers (インターフェース) のみを付ける。
# vulkan-1.lib にはリンクしない。実行時の読み込みは vk::raii::Context が行う。
target_link_libraries(xrift_native PRIVATE Vulkan::Headers)

# loader が共有ビルドの場合だけ、openxr_loader.dll を exe の隣に置く。静的なら不要。
get_target_property(_xrift_loader_type openxr_loader TYPE)
if(_xrift_loader_type STREQUAL "SHARED_LIBRARY")
  add_custom_command(TARGET xrift_native POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:openxr_loader>
      $<TARGET_FILE_DIR:xrift_native>
    COMMAND_EXPAND_LISTS
  )
endif()
