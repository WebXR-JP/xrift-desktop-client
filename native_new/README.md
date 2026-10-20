# xrift_new（単体 exe のひな形）

OpenXR + Vulkan の単体 exe。Electron / Node に依存せず、HMD の両眼に単色クリアを出す。

方針は `AGENTS.md` の通り。OpenXR のみ、Vulkan、Linux は将来対応。

## 必要環境

- CMake 3.24 以上
- Visual Studio 2022（Windows）または C++17 コンパイラ
- Python 3（必須。OpenXR-SDK-Source の loader コード生成に使う。実測では Python 3.13 で configure/build した）
- ネットワーク（初回 configure 時に FetchContent で依存を取得する）

依存は `cmake/Dependencies.cmake` で固定している。

- OpenXR-SDK-Source `release-1.1.65`
- Vulkan-Headers `v1.4.363`（Vulkan-Hpp `vulkan.hpp` + `vk::raii` 同梱）

Vulkan SDK のインストールは不要（`vulkan-1.lib` にリンクしない）。
実行時は `vulkan.hpp` の動的ディスパッチャ（`vk::raii::Context`）が `vulkan-1.dll` を読み込む。
ただし OpenXR-SDK-Source 側の configure は、インストール済みの Vulkan SDK があれば任意検出する
（configure 時に `Found Vulkan: ...` と出ることがあるが、`xrift_new` 本体は SDK にリンクしない）。

## ビルド

```powershell
cmake --preset windows-vs
cmake --build --preset windows-vs-release
```

## 実行

```powershell
build/windows/Release/xrift_new.exe --probe
build/windows/Release/xrift_new.exe
```

`--probe` はインスタンスと Vulkan 初期化だけ行い、ランタイム名・system 名・GPU 名・
推奨解像度・swapchain フォーマット候補を出して終了する。セッションは作らない。
引数なしは通常のフレームループ。`--help` で使い方を表示する。

## ディレクトリ構成と各ファイルの責務

- `CMakeLists.txt` … exe ターゲット `xrift_new` の定義
- `CMakePresets.json` … `windows-vs` / `linux` と各ビルド preset
- `cmake/Dependencies.cmake` … FetchContent による依存の取得定義
- `src/main.cpp` … CLI（`--probe` / `--help`）と `XrApp` の起動
- `src/app.h` / `src/app.cpp` … ライフサイクルとフレームループ、イベント処理、投影レイヤ提出
- `src/common/log.h` / `.cpp` … `[info]` / `[warn]` / `[error]` 出力
- `src/common/xr_vulkan.h` … vulkan.hpp→OpenXR の読み順を固定する共通 include
- `src/common/xr_result.h` / `.cpp` … `XrResult` の文字列化と `CheckXr`
- `src/xr/instance.h` / `.cpp` … `XrInstance` と system、拡張列挙と必須チェック
- `src/xr/session.h` / `.cpp` … Vk バインディング付き session、LOCAL 空間、view 構成、状態遷移
- `src/gfx/vulkan_context.h` / `.cpp` … OpenXR 要件からの `vk::raii::Instance` / `PhysicalDevice` / `Device` 生成
- `src/gfx/swapchains.h` / `.cpp` … 両眼の `XrSwapchain` とフォーマット選択（`vk::Format` 保持）
- `src/gfx/clear_pass.h` / `.cpp` … acquire / wait / clear / release とコマンド・フェンス管理（`vk::raii` 所有）

## OpenXR の呼び出し順

初期化:

1. `xrEnumerateInstanceExtensionProperties`（`XR_KHR_vulkan_enable` 必須確認）
2. `xrCreateInstance`（API 版は 1.0）
3. `xrGetInstanceProperties` / `xrGetSystem`（HMD）/ `xrGetSystemProperties`
4. `xrGetInstanceProcAddr`（Vulkan 拡張関数 4 本）
5. `xrGetVulkanGraphicsRequirementsKHR` / `xrGetVulkanInstanceExtensionsKHR`
6. `vkCreateInstance` / `xrGetVulkanGraphicsDeviceKHR` / `xrGetVulkanDeviceExtensionsKHR`
7. `vkCreateDevice`（graphics 対応 queue family、空 feature）
8. `xrCreateSession`（`XrGraphicsBindingVulkanKHR` 付き）
9. `xrCreateReferenceSpace`（LOCAL）
10. `xrEnumerateViewConfigurationViews`（PRIMARY_STEREO、2 眼）
11. `xrEnumerateSwapchainFormats` / `xrCreateSwapchain` / `xrEnumerateSwapchainImages`

フレーム（実行中のみ）:

1. `xrWaitFrame` / `xrBeginFrame`
2. `xrLocateViews`
3. `shouldRender` かつ位置・向きが有効なら両眼クリア描画
4. `xrEndFrame`（描画しないフレームは `layerCount=0`、必ず呼ぶ）

終了時は clear_pass → swapchain → session → device → instance の順に破棄する。

## 現状の制限

- 単色クリアのみ。シェーダ描画はない
- 入力アクション未実装
- 投影行列未使用（pose / fov をレイヤに載せるだけ）
- Windows のみ検証。Linux 対応は preset を用意した段階

## 次のステージ案

- 入力アクション（コントローラの pose / ボタン）
- シェーダ描画（単色クリアの置き換え）
- local-floor 対応（`XR_EXT_local_floor` の検出済みフラグを使う）
- Linux 対応（ビルド検証と動作確認）

設計: docs/DESIGN.md
