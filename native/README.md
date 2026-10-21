# xrift_native（単体 exe）

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
（configure 時に `Found Vulkan: ...` と出ることがあるが、`xrift_native` 本体は SDK にリンクしない）。

## ビルド

```powershell
cmake --preset windows-vs
cmake --build --preset windows-vs-release
```

## 実行

```powershell
build/windows/Release/xrift_native.exe --probe
build/windows/Release/xrift_native.exe
```

`--probe` はインスタンスと Vulkan 初期化だけ行い、ランタイム名・system 名・GPU 名・
推奨解像度・swapchain フォーマット候補を出して終了する。セッションは作らない。
引数なしは通常のフレームループ。`--help` で使い方を表示する。

## 転送モード

Electron の OSR 共有テクスチャを別プロセスで受けて GPU 内コピーする。
通常はハーネス（`harness/electron/run.ps1`）から起動する。単体では次の通り。

```powershell
build/windows/Release/xrift_native.exe --transfer --pipe=\\.\pipe\<名前> [--verify|--no-verify] [--gpu-check|--no-gpu-check] [--seconds=N]
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

## 全画面 GPU 検査

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

- `CMakeLists.txt` … exe ターゲット `xrift_native` の定義
- `CMakePresets.json` … `windows-vs` / `linux` と各ビルド preset
- `cmake/Dependencies.cmake` … FetchContent による依存の取得定義
- `src/main.cpp` … CLI（`--probe` / `--help`）と `XrApp` の起動
- `src/app.h` / `src/app.cpp` … ライフサイクルとフレームループ、イベント処理、投影レイヤ提出
- `src/common/log.h` / `.cpp` … `[info]` / `[warn]` / `[error]` 出力
- `src/common/xr_vulkan.h` … vulkan.hpp→OpenXR の読み順を固定する共通 include
- `src/common/xr_result.h` / `.cpp` … `XrResult` の文字列化と `CheckXr`
- `src/xr/instance.h` / `.cpp` … `XrInstance` と system、拡張列挙と必須チェック
- `src/xr/session.h` / `.cpp` … Vk バインディング付き session、参照空間、view 構成、状態遷移
- `src/xr/input.h` / `.cpp` … 入力アクション（セット `xrift`、12 アクション、
  profile 別 suggest、毎フレームの sync・スナップショット、haptics 適用）
- `src/gfx/vulkan_context.h` / `.cpp` … OpenXR 要件からの `vk::raii::Instance` / `PhysicalDevice` / `Device` 生成
  （転送用の追加デバイス拡張を受け付ける。既存呼び出しの動きは不変）
- `src/transfer/protocol.h` … 192 バイト固定の転送メッセージ（C++ と Node で同配置。
  64B→192B 化で HapticCommand・InputState の種別を追加）
- `src/transfer/pipe_client.h` / `.cpp` … 名前付きパイプのクライアント（5 秒リトライ、固定長送受信）
- `src/transfer/shared_texture.h` / `.cpp` … 外部メモリ import、自前イメージへのコピー、部分検証
- `src/transfer/gpu_check.h` / `.cpp` … 全画面 GPU 検査（compute 提出と結果読み）
- `shaders/check.comp` … 全画面 GPU 検査の compute シェーダ正本（`.spv` は glslc 生成か同梱品を埋め込む）
- `cmake/SpvToHeader.cmake` … SPIR-V を C 配列ヘッダへ変換する生成器
- `src/transfer/transfer_app.h` / `.cpp` … `--transfer` の受信ループと統計
- `transfer-addon/` … NT ハンドル複製の N-API アドオン（cmake-js、本体 CMake とは分離）
- `harness/electron/` … 旧検証ハーネス（借用台帳のパイプサーバ、動的パターン、起動手順）
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
- 入力アクションは取得・haptics 適用まで実装済みだが、実機のコントローラ操作・
  haptics の体感・追従は未検証（入力の検証は HMD 非装着の構造確認まで。下の入力節を参照）
- 投影行列未使用（pose / fov をレイヤに載せるだけ）
- Windows のみ検証。Linux 対応は preset を用意した段階
- 転送の既知の制約:
  - `d3d-open-only` は診断専用であり、長時間動かすとメモリが増える（実測約 +16MB/分）
  - producer 完了保証は無い（DESIGN §8）。フレーム番号の連番とセル単色で破綻を検知するが、
    無事故実測を同期証明の代用にしない
  - 単一 GPU 前提。Vulkan 物理デバイスの deviceLUID に一致する DXGI アダプタが無ければ起動しない

## 次のステージ案

- シェーダ描画（単色クリアの置き換え）
- 実機での入力確認（下のチェックリスト。コントローラ操作・haptics 体感・追従は未検証）
- Linux 対応（ビルド検証と動作確認）

## 入力・haptics・参照空間・再接続

`--xr-transfer` を土台に、入力アクション・haptics・reference space 変更・
HMD 脱着/再接続を実装する（DESIGN §10・§11）。
実機のコントローラ操作・haptics の体感・追従は未検証であり、
下の「実機装着時の確認項目」が終わるまで未検証と書く。

```powershell
build/windows/Release/xrift_native.exe --xr-transfer --pipe=\\.\pipe\<名前>
  [--render-scale=<0.2〜1.0>] [--record-ttl-ms=<ms>]
  [--space=<auto|local|stage|local-floor>] [--inject-session-restart=Ms]
  [--inject-space-change=Ms]
```

- `--space`（既定 `auto`）… 参照空間の要求。起動時に `xrEnumerateReferenceSpaces` で
  availability を確認し、`auto` は local-floor→stage→local の順で後退する。
  明示指定が不可なら落とさず後退し、理由をログに出す。選択結果は起動ログに出る。
- `--render-scale`（既定 `0.5`、範囲 `0.2`〜`1.0`）… 左右各眼の描画倍率。
  本番サイトでは `0.35` 程度を試せる。
- `--record-ttl-ms`（既定 `500`）… 描画レコードの失効上限（ms）。
  実測: 本番サイトで 350〜500ms の遅延のため、app 経由の通常運用は暫定 `2000` で渡す。
- `--inject-session-restart=Ms`（既定 0=無効）… 起動から Ms 後に意図的にセッション束を
  破棄・再生成する（復帰経路の検証用）。成功/失敗は統計に出る。
- `--inject-space-change=Ms`（既定 0=無効）… 起動から Ms 後に模擬の space 変更を
  1 回適用する。ランタイムの `REFERENCE_SPACE_CHANGE_PENDING` と同じ適用経路
  （空間作り直し・`spaceEpoch` 更新・旧レコード失効）を通すが、
  実イベントではないことをログに明記する
  （`模擬 space 変更を適用（実イベントではない…）`）。統計の `space変更` に計上する。
- ハーネス側（`harness/electron/run.ps1`）:
  `-Space`（exe の `--space` へ渡す）、`-HapticAtMs`（起動から Ms 後に
  HapticCommand を送る。既定 `'0'`=無効。カンマ区切りで複数時刻を指定できる。
  各送信は左手・振幅 0.8・300ms）、`-HapticRepeat`（5 秒間隔で N 回送る。
  既定 0=無効。左右交互・振幅 1.0・500ms）、
  `-InjectSessionRestartMs`（exe へ渡す。既定 0=無効）、
  `-InjectSpaceChangeMs`（exe へ渡す。既定 0=無効）。

入力の取得内容（`src/xr/input.h`）: 左右 grip/aim pose、trigger、squeeze、
thumbstick＋click、primary/secondary、menu、haptic の適用。trackpad＋click も
データ項目として維持する（束縛を持つ profile でのみ有効になる）。
suggest 先は khr/simple、oculus/touch、valve/index、htc/vive、microsoft/motion に
加え、bytedance 系（pico4、pico_neo3、pico_ultra_controller_bd、pico_g3）を
仕様（OpenXR 1.1.65 の xr.xml）に確認して追加した。
ランタイムが知らない profile への suggest は失敗として記録し、他を止めない
（実測: SteamVR は bytedance 系を `XR_ERROR_PATH_UNSUPPORTED` で拒否する）。

観測点:

- 起動ログ: アクションセット作成・attach、suggest の profile 別成否、
  参照空間の一覧と選択結果、実際に選ばれた profile（変化時に記録）。
- 毎フレーム: `xrSyncActions`（FOCUSED 時のみ）とスナップショット取得。
  main へは毎フレーム InputState（種別 5。予約先頭 2B に左右の押下ラッチ付き）を送り、
  ログは毎秒に抑える（接続状態、profile 名、押下中のボタン、trigger/squeeze 値、
  thumbstick、その 1 秒間に押された `latch=`）。
- 押下エッジ: 状態が変化したフレームで `[入力エッジ]` を 1 行出す
  （ボタン bit の press/release、trigger/squeeze の off->on/on->off、
  stick の idle->touch/touch->idle、pose 有効/無効の変化。変化が無ければ出さない）。
  1 秒周期のサマリでは短い押下を取りこぼすため、実機の取りこぼし診断は
  `[入力エッジ]` と `latch=` で行う。実機での再計測手順は
  `harness/electron/README.md` の「実機での再計測手順」を参照。
- inactive / 未接続 / 非対応の区別: 値とは別に active/supported の 2 段を持つ。
  supported=0 は非対応、supported=1 かつ active=0 は inactive、active=1 は有効値。
  未接続の手は flags=0 のまま全欄 0 で送る。
- haptics: main からの HapticCommand（種別 4。hand@40、amplitude@56、duration@48）を受け、
  `xrApplyHapticFeedback` を呼ぶ。送信結果は `XrResult` でログに残す
  （`haptics 送信: …（XrResult=…）`。VkResult ではない）。
  非対応・inactive の未送信と呼出し失敗は区別して出し、統計に残す。
  例外でループを止めない。
- `REFERENCE_SPACE_CHANGE_PENDING` を受けたら次フレームで空間を作り直し、
  新しい原点で locate できる確認が取れてから `spaceEpoch` を進め、
  旧世代の RenderLedger レコードを失効させる。挙動はログと統計（`space変更`）に出る。
- `STOPPING` → `xrEndSession`（束は保持し `READY` で再開）。
  `LOSS_PENDING` → 束を破棄し、新しい `connEpoch` で再生成する（1 秒周期・上限 30 回。
  ランタイム未復帰なら安全に終了し、統計を出す）。`EXITING` は再生成せず終了する。
  脱着は loss とは限らないため、focus/visible/`shouldRender` に従って提出だけ止める。
- 終了統計（`入力統計`）: connEpoch、InputState 送信数、haptic 受信数、space 変更数、
  再生成の成功/試行、入力 sync/失敗/snapshot/profile 変化、haptics 成功/未送信/失敗。

### HMD 非装着での検証手順（構造確認まで）

```powershell
# 1. 配置と値層（HMD 不要）。
node harness/electron/protocol-test.js
build/windows/Release/xrift_native.exe --selftest
build/windows/Release/xrift_native.exe --probe
# 2. XR 転送＋入力（HMD 非装着で可。SteamVR 起動中・HMD 未装着のまま行う）。
#    セッションは SYNCHRONIZED 止まり・入力は inactive のままである。
.\harness\electron\run.ps1 -Seconds 10 -Width 2048 -Height 1024 -Space stage `
  -HapticAtMs 5000 -InjectSessionRestartMs 8000
# 3. ハーネスの回帰（旧経路が壊れていないこと）。
.\harness\electron\fault-matrix.ps1
```

確認する証拠（HMD 非装着で取れる範囲）:

- アクションセット作成・attach が成功し、エラーログが無いこと
- suggest した profile と成否のログ（bytedance 系の失敗と続行を含む）
- 実際に選ばれた profile のログ（未装着時は未取得のまま。装着・追従後に記録される）
- inactive 時のスナップショットが「未接続/inactive」として出ること（0 と区別）
- haptics 指令が例外なく処理され、inactive として記録されること
- 参照空間の選択結果と `--space` 指定時の選択（`--space` 不正値は起動時に拒否）
- `--inject-session-restart` で破棄→再生成が走り、`connEpoch` が進み、統計が残ること
- `fault-matrix.ps1` が全 PASS であること

### 実機装着時にしか確認できない項目（チェックリスト。未検証）

- [ ] コントローラ装着・追従後に interaction profile が記録されること（PICO 4 の想定）
- [ ] 左右 grip/aim の `接続` 表示と pose 追従（HMD を動かしたときの値変化）
- [ ] trigger / squeeze のアナログ値（0〜1 の変化）
- [ ] thumbstick の 2 軸値と click、primary（A/X）/ secondary（B/Y）、menu の押下表示
- [ ] trackpad が非対応として出ること（PICO 系に trackpad は無い）
- [ ] haptics の体感（`-HapticAtMs` 送信時にコントローラが振動すること）
- [ ] FOCUSED 時の `xrSyncActions` 成功（統計の `sync` が進むこと）
- [ ] 再センタリング（ダッシュボード等）後の `REFERENCE_SPACE_CHANGE_PENDING` と
      `spaceEpoch` の進行、旧レコードの失効（統計の `space変更`）
- [ ] HMD 脱着で提出だけ止まり・復帰すること（exe が終了しないこと）
- [ ] SteamVR 再起動等の再接続で loss→新 `connEpoch` の再生成が走ること

設計: docs/DESIGN.md
