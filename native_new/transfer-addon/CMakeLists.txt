cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0091 NEW)
cmake_policy(SET CMP0042 NEW)

project(xrift_transfer_addon LANGUAGES C CXX)

# cmake-js が渡す Node/Electron ヘッダとライブラリを使う。
# 素の cmake 直叩きでは CMAKE_JS_* が未定義になるため、その場合は失敗理由を明示する。
if(NOT CMAKE_JS_INC)
  message(FATAL_ERROR "CMAKE_JS_INC が未定義。cmake-js 経由でビルドすること（build.ps1 を参照）。")
endif()

add_library(${PROJECT_NAME} SHARED addon.cpp ${CMAKE_JS_SRC})
set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "" SUFFIX ".node")
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_JS_INC})
target_compile_definitions(${PROJECT_NAME} PRIVATE NAPI_VERSION=8 UNICODE _UNICODE)
target_link_libraries(${PROJECT_NAME} PRIVATE ${CMAKE_JS_LIB})
if(MSVC)
  target_compile_options(${PROJECT_NAME} PRIVATE /EHsc /W3 /utf-8)
endif()

if(MSVC AND CMAKE_JS_NODELIB_DEF AND CMAKE_JS_NODELIB_TARGET)
  execute_process(COMMAND ${CMAKE_AR} /def:${CMAKE_JS_NODELIB_DEF} /out:${CMAKE_JS_NODELIB_TARGET} ${CMAKE_STATIC_LINKER_FLAGS})
endif()
