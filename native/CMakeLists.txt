cmake_minimum_required(VERSION 3.24)

project(xrift_new LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 依存の取得定義（OpenXR-SDK-Source / Vulkan-Headers）。
include(cmake/Dependencies.cmake)

add_executable(xrift_new
  src/main.cpp
  src/app.cpp
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
  src/transfer/mem_log.cpp
  src/transfer/gpu_check.cpp
  src/transfer/transfer_app.cpp
  src/transfer/m2_selftest.cpp
  src/transfer/xr_transfer_app.cpp
)

# M1b 全画面 GPU 検査の compute シェーダ（shaders/check.comp）。
# glslc があればビルド時に SPIR-V を生成し、無ければコミット済みの .spv を使う。
# どちらも Vulkan SDK へのリンクは生まない（glslc は任意の外部ツール扱い）。
# 再現ビルド用に強制する場合は -DXRIFT_USE_COMMITTED_SPV=ON を付ける。
option(XRIFT_USE_COMMITTED_SPV "コミット済みの check.comp.spv を使い glslc を使わない" OFF)
set(XRIFT_CHECK_COMP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/shaders/check.comp")
set(XRIFT_CHECK_SPV_COMMITTED "${CMAKE_CURRENT_SOURCE_DIR}/shaders/check.comp.spv")
set(XRIFT_CHECK_SPV_H "${CMAKE_CURRENT_BINARY_DIR}/check_comp_spv.h")
if(NOT XRIFT_USE_COMMITTED_SPV)
  find_program(XRIFT_GLSLC glslc)
endif()
if(XRIFT_GLSLC)
  # 生成と埋め込みは 1 つのカスタムコマンドに直列化する。
  # 分割すると並列ビルドで埋め込みが先走ることがある。
  # --target-env=vulkan1.0 を付ける。転送のデバイスはランタイム要件の最小版
  # （実測 1.0）で作るため、1.1 以降の SPIR-V 版は vkCreateComputePipelines で弾かれる。
  # 検査に使う機能は 1.0 の core だけである。
  set(XRIFT_CHECK_SPV "${CMAKE_CURRENT_BINARY_DIR}/check.comp.spv")
  add_custom_command(
    OUTPUT "${XRIFT_CHECK_SPV_H}"
    COMMAND "${XRIFT_GLSLC}" --target-env=vulkan1.0 -O "${XRIFT_CHECK_COMP_SRC}" -o "${XRIFT_CHECK_SPV}"
    COMMAND "${CMAKE_COMMAND}" -DSPV_IN=${XRIFT_CHECK_SPV} -DHEADER_OUT=${XRIFT_CHECK_SPV_H}
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SpvToHeader.cmake"
    DEPENDS "${XRIFT_CHECK_COMP_SRC}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SpvToHeader.cmake"
    COMMENT "glslc で M1b 検査シェーダを生成・埋め込み"
    VERBATIM)
else()
  if(NOT EXISTS "${XRIFT_CHECK_SPV_COMMITTED}")
    message(FATAL_ERROR
      "glslc が無く、コミット済みの shaders/check.comp.spv も無い。"
      "Vulkan SDK の glslc で shaders/check.comp から生成してコミットすること"
      "（README の「シェーダの再生成」を参照）。")
  endif()
  add_custom_command(
    OUTPUT "${XRIFT_CHECK_SPV_H}"
    COMMAND "${CMAKE_COMMAND}" -DSPV_IN=${XRIFT_CHECK_SPV_COMMITTED}
      -DHEADER_OUT=${XRIFT_CHECK_SPV_H}
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SpvToHeader.cmake"
    DEPENDS "${XRIFT_CHECK_SPV_COMMITTED}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SpvToHeader.cmake"
    COMMENT "コミット済み SPIR-V から M1b 検査シェーダを埋め込み"
    VERBATIM)
endif()
# SPIR-V を C 配列ヘッダへ変換して埋め込む。実行時に .spv ファイルを探さない。
add_custom_target(xrift_check_spv DEPENDS "${XRIFT_CHECK_SPV_H}")
add_dependencies(xrift_new xrift_check_spv)

target_include_directories(xrift_new PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}")

target_compile_definitions(xrift_new PRIVATE
  VK_NO_PROTOTYPES
  XR_USE_GRAPHICS_API_VULKAN
  VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1
)

if(WIN32)
  target_compile_definitions(xrift_new PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX VK_USE_PLATFORM_WIN32_KHR)
  # 転送の D3D11 照合（OpenSharedResource1）に使う。Vulkan 本体は動的読み込みのまま。
  # psapi はメモリログの GetProcessMemoryInfo に使う。
  target_link_libraries(xrift_new PRIVATE d3d11 dxgi dxguid psapi)
endif()

if(MSVC)
  target_compile_options(xrift_new PRIVATE /utf-8 /W4 /EHsc)
else()
  target_compile_options(xrift_new PRIVATE -Wall -Wextra -Wpedantic)
endif()

# OpenXR::openxr_loader は SDK が提供する alias。存在しない版に備えて素の名前に退避する。
if(TARGET OpenXR::openxr_loader)
  target_link_libraries(xrift_new PRIVATE OpenXR::openxr_loader)
else()
  target_link_libraries(xrift_new PRIVATE openxr_loader)
endif()

# Vulkan-Hpp はヘッダオンリーのため Vulkan::Headers (インターフェース) のみを付ける。
# vulkan-1.lib にはリンクしない。実行時の読み込みは vk::raii::Context が行う。
target_link_libraries(xrift_new PRIVATE Vulkan::Headers)

# loader が共有ビルドの場合だけ、openxr_loader.dll を exe の隣に置く。静的なら不要。
get_target_property(_xrift_loader_type openxr_loader TYPE)
if(_xrift_loader_type STREQUAL "SHARED_LIBRARY")
  add_custom_command(TARGET xrift_new POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:openxr_loader>
      $<TARGET_FILE_DIR:xrift_new>
    COMMAND_EXPAND_LISTS
  )
endif()
