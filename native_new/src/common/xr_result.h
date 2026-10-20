#pragma once

#include <openxr/openxr.h>

// XrResult の文字列化と、失敗時に例外を投げる検査。
// OpenXR の呼び出し結果判定はこの関数に集約する。

namespace xrift {

// 既知の結果コードは名前を返し、未知の値は "XR_UNKNOWN" を返す。
const char* XrResultToString(XrResult result);

// result が XR_SUCCESS でなければ、what を含む std::runtime_error を投げる。
void CheckXr(XrResult result, const char* what);

}  // namespace xrift
