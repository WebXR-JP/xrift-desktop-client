// M1 転送用 N-API アドオン（C ABI、node_api.h のみ）。
// Electron main が OSR paint で受けた NT ハンドルを、別 exe のプロセスへ複製する。
// 所有権の移動はしない。複製元のハンドルは閉じない。複製先プロセスのハンドルも閉じない
// （OpenProcess の疑似でない実ハンドルだけを閉じる）。
// 失敗は例外ではなく {ok:false, error, hresult} で返す。
#define NAPI_VERSION 8
#include <node_api.h>
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void SetStr(napi_env env, napi_value obj, const char *name, const char *s) {
  napi_value v;
  napi_create_string_utf8(env, s ? s : "", NAPI_AUTO_LENGTH, &v);
  napi_set_named_property(env, obj, name, v);
}

static void SetBool(napi_env env, napi_value obj, const char *name, bool b) {
  napi_value v;
  napi_get_boolean(env, b, &v);
  napi_set_named_property(env, obj, name, v);
}

static void SetU32(napi_env env, napi_value obj, const char *name, uint32_t x) {
  napi_value v;
  napi_create_uint32(env, x, &v);
  napi_set_named_property(env, obj, name, v);
}

static void Fail(napi_env env, napi_value out, const char *error, HRESULT hr) {
  SetBool(env, out, "ok", false);
  SetStr(env, out, "error", error);
  if (hr != 0) {
    char buf[32];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "0x%08lX", (unsigned long)hr);
    SetStr(env, out, "hresult", buf);
  }
}

// duplicateHandle(ntHandleBuffer, targetPid)
// ntHandleBuffer: 8 バイト Buffer（textureInfo.handle.ntHandle の複製元）。
// targetPid: 複製先プロセスの pid（数値）。
// 成功時は {ok:true, handle: BigInt}（複製先で有効なハンドル値）を返す。
static napi_value DuplicateHandle(napi_env env, napi_callback_info info) {
  napi_value out;
  napi_create_object(env, &out);

  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc < 2) {
    Fail(env, out, "引数が足りない（8バイトBuffer, pidが必要）", 0);
    return out;
  }
  bool is_buf = false;
  napi_is_buffer(env, argv[0], &is_buf);
  size_t len = 0;
  void *data = NULL;
  if (is_buf) {
    napi_get_buffer_info(env, argv[0], &data, &len);
  }
  if (!is_buf || len != 8) {
    char msg[128];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                "8バイトBufferが必要（isBuffer=%d len=%zu）", (int)is_buf, len);
    Fail(env, out, msg, 0);
    return out;
  }
  HANDLE src = NULL;
  memcpy(&src, data, 8);
  if (src == NULL || src == INVALID_HANDLE_VALUE) {
    Fail(env, out, "HANDLEがNULL/INVALID", 0);
    return out;
  }
  int64_t pid = 0;
  napi_status st = napi_get_value_int64(env, argv[1], &pid);
  if (st != napi_ok || pid <= 0 || pid > 0x7FFFFFFF) {
    Fail(env, out, "pidが不正（正の整数が必要）", 0);
    return out;
  }

  HANDLE target = OpenProcess(PROCESS_DUP_HANDLE, FALSE, (DWORD)pid);
  if (target == NULL) {
    char msg[128];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE, "OpenProcess(%d)失敗", (int)pid);
    Fail(env, out, msg, (HRESULT)GetLastError());
    return out;
  }
  HANDLE remote = NULL;
  BOOL ok = DuplicateHandle(GetCurrentProcess(), src, target, &remote, 0, FALSE,
                            DUPLICATE_SAME_ACCESS);
  DWORD dup_err = ok ? 0 : GetLastError();
  CloseHandle(target);
  if (!ok || remote == NULL) {
    char msg[128];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE, "DuplicateHandle失敗");
    Fail(env, out, msg, (HRESULT)dup_err);
    return out;
  }

  SetBool(env, out, "ok", true);
  napi_value big;
  napi_create_bigint_uint64(env, (uint64_t)(uintptr_t)remote, &big);
  napi_set_named_property(env, out, "handle", big);
  return out;
}

static napi_value Init(napi_env env, napi_value exports) {
  napi_value fn;
  napi_create_function(env, "duplicateHandle", NAPI_AUTO_LENGTH, DuplicateHandle, NULL, &fn);
  napi_set_named_property(env, exports, "duplicateHandle", fn);
  return exports;
}

NAPI_MODULE(xrift_transfer_addon, Init)
