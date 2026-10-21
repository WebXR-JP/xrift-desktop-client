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
#include <stdlib.h>
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

// ---- SteamVR アプリ登録（旧 native/src/addon.cpp の移植） ----
// SteamVR 付属の openvr_api.dll をユーティリティとして初期化し、
// IVRApplications のフラット関数テーブル（FnTable:IVRApplications_007）で
// .vrmanifest を登録 / 解除 / 確認する。失敗は例外ではなく
// {ok:false, error} で返し、プロセスを落とさない。
typedef uint32_t(__cdecl *PFN_VR_InitInternal2)(int *peError, int eType, const char *pStartupInfo);
typedef void(__cdecl *PFN_VR_ShutdownInternal)(void);
typedef void *(__cdecl *PFN_VR_GetGenericInterface)(const char *pchInterfaceVersion, int *peError);
typedef int(__stdcall *PFN_AddApplicationManifest)(const char *path, bool temporary);
typedef int(__stdcall *PFN_RemoveApplicationManifest)(const char *path);
typedef bool(__stdcall *PFN_IsApplicationInstalled)(const char *appKey);
struct IVRApplicationsFnTable {
  PFN_AddApplicationManifest AddApplicationManifest;
  PFN_RemoveApplicationManifest RemoveApplicationManifest;
  PFN_IsApplicationInstalled IsApplicationInstalled;
};
#define XRIFT_VRAPPLICATION_UTILITY 3

// UTF-8 の napi 文字列引数をヒープ確保の C 文字列として取り出す。
// 呼び出し側が free() する。失敗時は NULL を返す。
static char *GetUtf8Arg(napi_env env, napi_value v) {
  size_t len = 0;
  if (napi_get_value_string_utf8(env, v, NULL, 0, &len) != napi_ok) return NULL;
  char *buf = (char *)malloc(len + 1);
  if (!buf) return NULL;
  size_t written = 0;
  if (napi_get_value_string_utf8(env, v, buf, len + 1, &written) != napi_ok) {
    free(buf);
    return NULL;
  }
  return buf;
}

// UTF-8 → ワイド文字列（LoadLibraryW 用）。呼び出し側が free() する。
static wchar_t *Utf8ToWide(const char *s) {
  if (!s) return NULL;
  int wlen = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  if (wlen <= 0) return NULL;
  wchar_t *w = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
  if (!w) return NULL;
  if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, wlen) <= 0) {
    free(w);
    return NULL;
  }
  return w;
}

// steamvrManifest(op, dllPath, manifestPath | appKey)
// op: 'add' | 'remove' | 'check'。add/remove の第 3 引数はマニフェストのパス、
// check の第 3 引数は app_key。戻り値は {ok, error?, installed?}。
static napi_value SteamVRManifest(napi_env env, napi_callback_info info) {
  napi_value out;
  napi_create_object(env, &out);

  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc < 3) {
    Fail(env, out, "引数が足りない（op, dllPath, manifestPath|appKeyが必要）", 0);
    return out;
  }
  char *op = GetUtf8Arg(env, argv[0]);
  char *dll = GetUtf8Arg(env, argv[1]);
  char *arg = GetUtf8Arg(env, argv[2]);
  if (!op || !dll || !arg) {
    free(op);
    free(dll);
    free(arg);
    Fail(env, out, "引数はすべて文字列であること", 0);
    return out;
  }
  bool isAdd = strcmp(op, "add") == 0;
  bool isRemove = strcmp(op, "remove") == 0;
  bool isCheck = strcmp(op, "check") == 0;
  if (!isAdd && !isRemove && !isCheck) {
    Fail(env, out, "opは add/remove/check のいずれかであること", 0);
    free(op);
    free(dll);
    free(arg);
    return out;
  }

  wchar_t *wdll = Utf8ToWide(dll);
  if (!wdll) {
    Fail(env, out, "dllパスの変換に失敗", 0);
    free(op);
    free(dll);
    free(arg);
    return out;
  }
  HMODULE mod = LoadLibraryW(wdll);
  free(wdll);
  if (!mod) {
    char msg[512];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE, "openvr_api.dll を読み込めない: %s", dll);
    Fail(env, out, msg, (HRESULT)GetLastError());
    free(op);
    free(dll);
    free(arg);
    return out;
  }
  PFN_VR_InitInternal2 initFn =
      (PFN_VR_InitInternal2)GetProcAddress(mod, "VR_InitInternal2");
  PFN_VR_ShutdownInternal shutdownFn =
      (PFN_VR_ShutdownInternal)GetProcAddress(mod, "VR_ShutdownInternal");
  PFN_VR_GetGenericInterface getIfaceFn =
      (PFN_VR_GetGenericInterface)GetProcAddress(mod, "VR_GetGenericInterface");
  if (!initFn || !shutdownFn || !getIfaceFn) {
    FreeLibrary(mod);
    Fail(env, out, "openvr_api.dll に必要な関数が無い", 0);
    free(op);
    free(dll);
    free(arg);
    return out;
  }
  int err = 0;
  initFn(&err, XRIFT_VRAPPLICATION_UTILITY, NULL);
  if (err != 0) {
    FreeLibrary(mod);
    char msg[256];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                "VR_Init失敗 (EVRInitError %d)。SteamVR を起動してから実行すること", err);
    Fail(env, out, msg, 0);
    free(op);
    free(dll);
    free(arg);
    return out;
  }
  int ifaceErr = 0;
  struct IVRApplicationsFnTable *apps =
      (struct IVRApplicationsFnTable *)getIfaceFn("FnTable:IVRApplications_007", &ifaceErr);
  if (!apps || ifaceErr != 0) {
    shutdownFn();
    FreeLibrary(mod);
    char msg[256];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                "IVRApplications_007 を取得できない (%d)", ifaceErr);
    Fail(env, out, msg, 0);
    free(op);
    free(dll);
    free(arg);
    return out;
  }
  int appErr = 0;
  if (isAdd) {
    appErr = apps->AddApplicationManifest(arg, false);
  } else if (isRemove) {
    appErr = apps->RemoveApplicationManifest(arg);
  } else {
    SetBool(env, out, "installed", apps->IsApplicationInstalled(arg) ? true : false);
  }
  shutdownFn();
  FreeLibrary(mod);
  free(op);
  free(dll);
  free(arg);
  if (appErr != 0) {
    char msg[128];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE, "EVRApplicationError %d", appErr);
    Fail(env, out, msg, 0);
    return out;
  }
  SetBool(env, out, "ok", true);
  return out;
}

static napi_value Init(napi_env env, napi_value exports) {
  napi_value fn;
  napi_create_function(env, "duplicateHandle", NAPI_AUTO_LENGTH, DuplicateHandle, NULL, &fn);
  napi_set_named_property(env, exports, "duplicateHandle", fn);
  napi_value fnManifest;
  napi_create_function(env, "steamvrManifest", NAPI_AUTO_LENGTH, SteamVRManifest, NULL, &fnManifest);
  napi_set_named_property(env, exports, "steamvrManifest", fnManifest);
  return exports;
}

NAPI_MODULE(xrift_transfer_addon, Init)
