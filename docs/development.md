# 開発手順

## 依存

- native（FetchContent で自動取得。`native/cmake/Dependencies.cmake`）
  - OpenXR-SDK-Source `release-1.1.65`（loader のみ。layer と test は落とす）
  - Vulkan-Headers `v1.4.363`（`vulkan.hpp`＋`vk::raii` 同梱。ヘッダのみで `vulkan-1.lib` にはリンクしない。実行時は動的ディスパッチャが `vulkan-1.dll` を読む）
- JS（ルート `package.json` の devDependencies）
  - `electron` 44.9.0（実行環境。`pnpm install` では本体が落ちてこないため `pnpm electron:install` が別に要る）
  - `three` 0.186.1（検証ページ用。配布物には `three.module.js` と `three.core.js` の 2 ファイルだけ同梱する）
  - `@electron/packager`（配布物作成用）、`cmake-js`（transfer-addon ビルド用）、`typescript`、`@types/node`（ビルド用）
- 検証ページの three.js はルートの `node_modules` を使う。新規インストールはしない。

## 必要環境

- Windows（x64。検証は Windows のみ。Linux は将来対応で preset だけ用意）
- Visual Studio 2022（MSVC。`vcvars64.bat` 経由で環境を入れる）
- CMake 3.24 以上、Ninja（`windows-vs` preset が使う）
- Python 3（OpenXR-SDK-Source の loader コード生成に使う。実測では 3.13 で configure/build）
- Node 22.15 以上、pnpm 10 以上
- 初回 configure 時のネットワーク（FetchContent の取得用）

## ビルド手順（PowerShell。リポジトリルートから）

```powershell
pnpm install
pnpm electron:install   # Electron 本体の取得
pnpm build              # client のビルド（tsc → client/dist）
pnpm build:native       # xrift_native.exe のビルド（cmake preset windows-vs）
pnpm build:addon        # transfer-addon のビルド（cmake-js。Electron 44.9.0 用）
```

成果物の配置は次の通り。

| 成果物 | 配置 |
|---|---|
| client 本体 | `client/dist/`（`tsc -p client` の出力。`main.js` が入口） |
| exe | `native/build/windows/Release/xrift_native.exe` |
| addon | `native/transfer-addon/build/Release/xrift_transfer_addon.node` |
| 配布物 | `client/dist-package/XRiftClient-win32-x64/`（`XRiftClient.exe`） |
| 実行証拠 | `client/logs/`（`client.log`・`stats.json`・capture PNG。コミットしない） |

## 起動

```powershell
pnpm start -- --url=<絶対パス>/tools/pages/xr-smoke.html --seconds=15
```

既定では `https://app.xrift.net/` を開く。引数の正本は [`../client/README.md`](../client/README.md)。

## package と register の使い方

```powershell
pnpm --dir client run package
# → client/dist-package/XRiftClient-win32-x64/ を作る。
#    client/dist・pages・tools/pages・exe・addon・three 2 ファイルを同梱する

pnpm register -- add              # 開発版を SteamVR に登録（SteamVR を起動しておく）
pnpm register -- check            # 登録確認（IsApplicationInstalled）
pnpm register -- remove           # 登録解除
pnpm register -- add --packaged   # 配布版を登録（package の後）
pnpm register -- check --packaged # 配布版の登録確認
pnpm register -- remove --packaged # 配布版の登録解除
```

開発版と配布版は同一 `app_key`（`net.xrift.client`）のため排他であり、`add` 時に置き換わる。

## トラブルシュート

- MSVC 環境：`cmake` がコンパイラを見つけられない場合は `vcvars64.bat` を先に呼ぶ。
  ```powershell
  & "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
  pnpm build:native
  ```
  版により配置が違う場合は `vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools` で探す。
- `XR_RUNTIME_JSON`：OpenXR ランタイム設定を上書きする環境変数である。シミュレータで動かす場合はランタイムの JSON を指す。例：
  ```powershell
  $env:XR_RUNTIME_JSON = "<シミュレータの JSON への絶対パス>"
  ```
  通常の SteamVR 実行では設定しない（設定したままだと SteamVR を見に行かない）。
- addon が無い：`pnpm build:addon` を実行する。Electron の版が変わったら `native/transfer-addon/build.ps1` の `--runtime-version` を更新する。
- Electron 本体が無い：`pnpm electron:install` を実行する（`pnpm install` では取得されない）。
- exe がパイプに繋がらない：`client/logs/client.log` の `exe がパイプに接続` と exe 側ログを確認する。`--xr-transfer` は同時に 1 本だけ起動すること（2 本目は XR 初期化に失敗して終了 1 になる）。
