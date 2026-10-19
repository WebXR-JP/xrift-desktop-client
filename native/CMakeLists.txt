cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0091 NEW)
cmake_policy(SET CMP0042 NEW)

project(xrift_openxr LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OPENXR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/openxr)
set(OPENVR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/openvr)

# node-addon-api のヘッダ位置（package.json のあるプロジェクトルートから解決）
execute_process(
  COMMAND node -p "require('node-addon-api').include"
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
  OUTPUT_VARIABLE NODE_ADDON_API_DIR
)
string(REGEX REPLACE "[\r\n\"]" "" NODE_ADDON_API_DIR "${NODE_ADDON_API_DIR}")

# ---- Node アドオン本体（OpenXR + D3D11） ----
add_library(${PROJECT_NAME} SHARED src/addon.cpp src/openvr_session.cpp ${CMAKE_JS_SRC})
set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "" SUFFIX ".node")
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_JS_INC} ${NODE_ADDON_API_DIR} ${OPENXR_DIR}/include ${OPENVR_DIR}/headers)
target_compile_definitions(${PROJECT_NAME} PRIVATE NAPI_VERSION=8 NAPI_CPP_EXCEPTIONS UNICODE _UNICODE)
target_link_libraries(${PROJECT_NAME} PRIVATE ${CMAKE_JS_LIB} ${OPENXR_DIR}/x64/lib/openxr_loader.lib ${OPENVR_DIR}/lib/openvr_api.lib d3d11 dxgi)
if(MSVC)
  target_compile_options(${PROJECT_NAME} PRIVATE /EHsc /W3 /utf-8)
endif()

if(MSVC AND CMAKE_JS_NODELIB_DEF AND CMAKE_JS_NODELIB_TARGET)
  execute_process(COMMAND ${CMAKE_AR} /def:${CMAKE_JS_NODELIB_DEF} /out:${CMAKE_JS_NODELIB_TARGET} ${CMAKE_STATIC_LINKER_FLAGS})
endif()

# openxr_loader.dll を .node の隣に置く
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${OPENXR_DIR}/x64/bin/openxr_loader.dll
    ${OPENVR_DIR}/bin/openvr_api.dll
    $<TARGET_FILE_DIR:${PROJECT_NAME}>)

# ---- SteamVR / Steam から起動するためのランチャー exe ----
add_executable(xrift_launcher WIN32 src/launcher.cpp)
target_compile_definitions(xrift_launcher PRIVATE UNICODE _UNICODE)
if(MSVC)
  target_compile_options(xrift_launcher PRIVATE /utf-8)
endif()
