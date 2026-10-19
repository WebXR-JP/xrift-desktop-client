// XRiftVR.exe — デスクトップ / Steam から起動するための小さなランチャー
//
// 既定: 専用ブラウザ（Electron で本家 app.xrift.net を開き、WebXR を OpenXR に繋ぐ）
//   node_modules/electron/dist/electron.exe "<exe のフォルダ>" [引数]
// コマンドライン引数はそのまま渡す（例: XRiftVR.exe --world <id>）。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

static std::wstring DirOf(const std::wstring& path) {
  size_t pos = path.find_last_of(L"\\/");
  return pos == std::wstring::npos ? L"." : path.substr(0, pos);
}

static bool FileExists(const std::wstring& p) {
  DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  wchar_t exe[MAX_PATH * 4];
  GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
  std::wstring root = DirOf(exe);

  // 自分自身の exe 名を除いた引数部分
  std::wstring args;
  {
    LPWSTR cmd = GetCommandLineW();
    bool quoted = false;
    while (*cmd && (quoted || (*cmd != L' ' && *cmd != L'	'))) {
      if (*cmd == L'"') quoted = !quoted;
      ++cmd;
    }
    while (*cmd == L' ' || *cmd == L'	') ++cmd;
    args = cmd;
  }

  // 親（VS Code など）から ELECTRON_RUN_AS_NODE を引き継ぐと Electron が Node として動いてしまう
  SetEnvironmentVariableW(L"ELECTRON_RUN_AS_NODE", nullptr);

  {
    std::wstring electron = root + L"\\node_modules\\electron\\dist\\electron.exe";
    if (!FileExists(electron)) {
      MessageBoxW(nullptr, L"Electron が見つかりません。プロジェクトのフォルダで npm install を実行してください。", L"XRift VR", MB_ICONERROR);
      return 1;
    }
    std::wstring cmdline = L"\"" + electron + L"\" \"" + root + L"\"";
    if (!args.empty()) cmdline += L" " + args;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr, root.c_str(), &si, &pi)) {
      MessageBoxW(nullptr, L"Electron の起動に失敗しました。", L"XRift VR", MB_ICONERROR);
      return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
  }

  return 0;
}
