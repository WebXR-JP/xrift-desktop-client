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

## 転送モード（M1）

Electron の OSR 共有テクスチャを別プロセスで受けて GPU 内コピーする。
通常はハーネス（`harness/electron/run.ps1`）から起動する。単体では次の通り。

```powershell
build/windows/Release/xrift_new.exe --transfer --pipe=\\.\pipe\<名前> [--verify|--no-verify] [--gpu-check|--no-gpu-check] [--seconds=N]
  [--fail-after=N] [--inject-unconfirmed=N] [--delay-ms=N]
  [--mode=<vulkan|d3d-open-only|no-gpu>] [--mem-log=<path>]
```

- XR instance を作り、`xrGetVulkanGraphicsDeviceKHR` の物理デバイスで Vulkan device を作る
  （セッションは作らない）。外部メモリ系のデバイス拡張が無ければ名指しで落とす
- main プロセス（パイプサーバ）から Offer（複製ハンドル+実サイズ）を受け、
  Vulkan import→自前イメージへコピー→全画面 GPU 検査→部分検証→Done を返す
- import 側イメージのレイアウト遷移は内容を破棄しない（GENERAL 経由の 2 段階遷移）
- D3D11 の Desc 照合は既定では行わない。Offer の width/height/format を信じて import し、
  妥当性は Vulkan 側の import・コピーと全画面 GPU 検査で担保する（理由は後述）。
  不正な寸法・形式は失敗側に回り、黙って成功にはならない
- 検証 readback は `--verify`（既定 on）の範囲に隔離した一部分だけであり、本番経路ではない
- 受領した複製ハンドルは import 成否に関わらず閉じる。元の Electron 側は閉じない
- 障害注入フック（いずれも既定 0=無効。通常経路の挙動は変えない）:
  - `--inject-unconfirmed=N` … N 件目の Offer を完了未確認として扱う。
    実際のフェンス失敗と同じ `UnconfirmedCompletion` 経路（Done なし→受付停止→終了 2）を通す。
    import 資源は破棄せず保持する。別経路は作らない
  - `--delay-ms=N` … 1 フレームの処理前に N ミリ秒待つ（consumer 遅延の模擬）。
    ハンドル数（`GetProcessHandleCount`）は 60 件ごとの進捗と終了時の統計に出る
- 切り分け用の診断モード `--mode=`（既定 `vulkan`=Vulkan 経路。借用台帳・ACK・release は同じ）:
  - `vulkan` … 既定。D3D11 デバイスを作らず、Offer 寸法で import/コピー/検査を行う
  - `d3d-open-only` … D3D11 Open 経路の診断専用。受領ハンドルを D3D11 で開いて Desc を読み、
    即閉じる。Vulkan は使わない。長時間動かすとメモリが増えるため切り分け以外の用途では使わない
  - `no-gpu` … GPU API を一切使わず即 Done（借用の返却だけ）
- 計測 `--mem-log=<path>` … 1 秒周期の CSV（`t_ms,mode,privateBytes,workingSetBytes,handleCount,`
  `liveVkImages,liveVkImageBytes,liveVkMemories,liveVkMemoryBytes,imports,frees,framesOk,framesFailed`）。
  生存 Vk 資源は確保・破棄の箇所で数える。空 path では出さない
- 統計の正直化: `gpuChecked` は検査を実際に完了した数だけを数え、失敗フレームは
  `スキップ` に別計上する（以前は失敗分も検査済みに混ざっていた）

## D3D11 を既定から外した理由（実測値）

- D3D11 の `OpenSharedResource1` を毎フレーム呼ぶ経路は持続不能である。
  4096x2048・90Hz の実測で Private Bytes が約 +16MB/分で増加し続けた
  （`--mode=d3d-open-only` の 120 秒計測と旧既定経路の 60 秒計測で確認）。
- 一方 Vulkan 経路（既定 `--mode=vulkan`）は 10 分で完全に平坦である。
  Private Bytes 96.2→97.2MB、ハンドル数は増加せず、Vk 生存数は imports=frees で残留なし。
- このため既定では D3D11 デバイスを作らず（`D3D11CreateDevice` すら呼ばない）、
  Desc 照合も行わない。妥当性は Offer の width/height/format と GPU 検査で担保する。
  `d3d-open-only` は D3D11 Open 経路の診断専用として残すが、長時間動かしてはならない。

## 全画面 GPU 検査（M1b）

CPU に画像を戻さず、import した共有テクスチャの全画素が同じフレームの正しい
パターンであることを GPU 上で検証する。検出対象は破損・世代混在・ティアリング。

- パターン（`harness/electron/index.html` と厳密に一致させること）:
  `c = (f * 2654435761u) ^ (tx * 40503u) ^ (ty * 2246822519u)`（32bit ラップ）、
  `R = c & 0xFF, G = (c >> 8) & 0xFF, B = (c >> 16) & 0xFF, A = 255`。
  タイルは 16x16（`tx = x / 16`、`ty = y / 16` は整数除算であり丸め誤差は無い）。
  予約領域（タイルパターンを上書きする）: (64,64) から 32x32 のフレーム番号ブロック
  （bit b を `(b%8)*4`、`(b/8)*4` 起点の 4x4 セルに描く。1=白/0=黒。セル内は単色。
  使わない行は黒で埋める）と 4 隅の 64x64 ブロック（左上=赤、右上=緑、左下=青、右下=白）。
  WebGPU（WGSL の u32）と GLSL（uint）はいずれも 32bit ラップのため同じ結果になる。
- 検査（`src/transfer/gpu_check.*`、`shaders/check.comp`）:
  自前イメージをサンプルし、1 スレッド = 1 タイルで検査する。
  まず番号ブロックから `f` を復号し（各セルの単色も確認）、予約外の全タイルで
  中心 1 画素を期待値と比較する。不一致は `atomicAdd` で数え、最初の 8 件だけ
  (x, y, expected, actual, 種別) を記録する。読むのは結果バッファ（172 バイト）
  だけであり、画像全体は読まない。
- `--gpu-check`（既定 on）/`--no-gpu-check` で切り替える。
  不一致が出た Offer の Done は `kStatusVerifyFailed` になる（内訳は exe 側の統計に出る）。
  `--verify`（既存の部分 readback）はデバッグ用に残す。
- 統計（終了時と 60 件ごとの進捗に出る）:
  `検査`（実行フレーム数）、`不一致フレーム`、`不一致合計`、`check 平均/最大`、
  `最終復号`（復号したフレーム番号）、`前進/停滞/後退`（前回との差の推移。
  繰り返し paint の停滞は失敗にしない）。

## シェーダのビルドと再生成

- `shaders/check.comp`（GLSL compute）が検査の正本である。
- `glslc` があればビルド時に SPIR-V を生成し、無ければコミット済みの
  `shaders/check.comp.spv` を使う。どちらも exe に埋め込むため、実行時に
  `.spv` ファイルを探さない。再現ビルド用には `-DXRIFT_USE_COMMITTED_SPV=ON` を付ける。
- Vulkan SDK 非依存を維持すること。`VULKAN_SDK` 無しでも configure/build が
  通ることを確認してから変える（コミット済み `.spv` の経路）。
- 再生成手順（パターンを変えたら必ず行い、`.spv` もコミットする）:

```powershell
C:\VulkanSDK\<版>\Bin\glslc.exe --target-env=vulkan1.0 -O shaders/check.comp -o shaders/check.comp.spv
```

  `--target-env=vulkan1.0` を付ける（転送のデバイスはランタイム要件の最小版で作るため。
  1.1 以降の SPIR-V 版は `vkCreateComputePipelines` で弾かれる）。

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
  （転送用の追加デバイス拡張を受け付ける。既存呼び出しの動きは不変）
- `src/transfer/protocol.h` … 64 バイト固定の転送メッセージ（C++ と Node で同配置）
- `src/transfer/pipe_client.h` / `.cpp` … 名前付きパイプのクライアント（5 秒リトライ、固定長送受信）
- `src/transfer/shared_texture.h` / `.cpp` … 外部メモリ import、自前イメージへのコピー、部分検証
- `src/transfer/gpu_check.h` / `.cpp` … M1b 全画面 GPU 検査（compute 提出と結果読み）
- `shaders/check.comp` … M1b 検査の compute シェーダ正本（`.spv` は glslc 生成か同梱品を埋め込む）
- `cmake/SpvToHeader.cmake` … SPIR-V を C 配列ヘッダへ変換する生成器
- `src/transfer/transfer_app.h` / `.cpp` … `--transfer` の受信ループと統計
- `transfer-addon/` … NT ハンドル複製の N-API アドオン（cmake-js、本体 CMake とは分離）
- `harness/electron/` … M1 検証ハーネス（借用台帳のパイプサーバ、動的パターン、起動手順）
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
- 転送の既知の制約:
  - `d3d-open-only` は診断専用であり、長時間動かすとメモリが増える（実測約 +16MB/分）
  - producer 完了保証は無い（DESIGN §8）。フレーム番号の連番とセル単色で破綻を検知するが、
    無事故実測を同期証明の代用にしない
  - 単一 GPU 前提。Vulkan 物理デバイスの deviceLUID に一致する DXGI アダプタが無ければ起動しない

## 次のステージ案

- 入力アクション（コントローラの pose / ボタン）
- シェーダ描画（単色クリアの置き換え）
- local-floor 対応（`XR_EXT_local_floor` の検出済みフラグを使う）
- Linux 対応（ビルド検証と動作確認）

設計: docs/DESIGN.md
