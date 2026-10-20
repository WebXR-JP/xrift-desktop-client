# 依存の取得定義。FetchContent で自動取得するため、Vulkan SDK への依存は作らない。
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

# volk。Vulkan-Headers を内包させるため VOLK_PULL_IN_VULKAN を有効にする。
set(VOLK_PULL_IN_VULKAN ON CACHE BOOL "" FORCE)
FetchContent_Declare(
  volk
  GIT_REPOSITORY https://github.com/zeux/volk.git
  GIT_TAG vulkan-sdk-1.4.363.0
  GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(openxr volk)

# volk の版によって alias 名が volk だけの場合に備える。
if(NOT TARGET volk::volk AND TARGET volk)
  add_library(volk::volk ALIAS volk)
endif()
