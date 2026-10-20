# 依存の取得定義。FetchContent で自動取得するため、Vulkan SDK のインストールは不要。
# なお OpenXR-SDK-Source 側の configure は、存在すれば Vulkan SDK を任意検出する (本体はリンクしない)。
# Vulkan の C++ バインディング (vulkan.hpp + vk::raii) は Vulkan-Headers 同梱を使い、
# 実行時の読み込みは vulkan.hpp の動的ディスパッチャ (vk::raii::Context) が行う。
include(FetchContent)

# OpenXR-SDK-Source。loader のみ使い、layer と test は落とす。
set(BUILD_LOADER ON CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  openxr
  GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK-Source.git
  GIT_TAG release-1.1.65
  GIT_SHALLOW TRUE
)

# Vulkan-Headers。vulkan.hpp (Vulkan-Hpp) 同梱。リンクはインターフェースのみで、
# vulkan-1.lib にはリンクしない (実行時は動的ディスパッチャが vulkan-1.dll を読む)。
FetchContent_Declare(
  vulkan_headers
  GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
  GIT_TAG v1.4.363
  GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(openxr vulkan_headers)
