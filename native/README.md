# native（`xrift_native.exe`）

責務：OpenXR＋Vulkan の単体 exe。Electron に依存せず、XR セッション・描画レコード発行・GPU 転送・照合・提出・入力・haptics を担う。

方針は `AGENTS.md` の通り。OpenXR のみ、Vulkan、CPU コピーなし、Linux は将来対応。詳細は [`../docs/architecture.md`](../docs/architecture.md)。

## 必要環境・ビルド

```powershell
cmake -S native --preset windows-vs
cmake --build native/build/windows --config Release
```

- CMake 3.24 以上、Visual Studio 2022、Python 3（OpenXR loader のコード生成用）、初回 configure 時のネットワーク（FetchContent）。
- 依存は `cmake/Dependencies.cmake` で固定：OpenXR-SDK-Source `release-1.1.65`、Vulkan-Headers `v1.4.363`。
- Vulkan SDK のインストールは不要（`vulkan-1.lib` にリンクしない。実行時は動的ディスパッチャが `vulkan-1.dll` を読む）。
- コンパイラが見つからない場合は `vcvars64.bat` を先に呼ぶ（[`../docs/development.md`](../docs/development.md)）。

## 実行

```powershell
native/build/windows/Release/xrift_native.exe --probe
native/build/windows/Release/xrift_native.exe --selftest
native/build/windows/Release/xrift_native.exe --xr-transfer --pipe=\\.\pipe\<名前> [--render-scale=0.5] [--record-ttl-ms=500] [--seconds=N] [--space=auto]
```

| 引数 | 意味 |
|---|---|
| （なし） | HMD に両眼クリア色を表示する |
| `--probe` | インスタンスと Vulkan 初期化だけ行い、ランタイム名・system 名・GPU 名・推奨解像度・swapchain フォーマット候補を出して終了する（セッションは作らない） |
| `--selftest` | 転送の値層（配置・pose タグ・受理/棄却）を HMD 無しに検証する。all ok が期待値 |
| `--xr-transfer --pipe=<名前>` | XR 提出モード（必須 `--pipe`）。pose/FOV の描画レコードを発行し、受理画像だけを左右 swapchain へ blit する |
| `--render-scale=<0.2〜1.0>` | 左右各眼の描画倍率（既定 0.5） |
| `--record-ttl-ms=<ms>` | 描画レコードの失効上限（既定 500。正の整数のみ。app 経由の通常運用は 2000 で渡す） |
| `--seconds=N` | N 秒で自動停止（既定 0=手動終了） |
| `--space=<auto\|local\|stage\|local-floor>` | 参照空間の要求（既定 auto=local-floor→stage→local の後退。不可でも落とさず後退し、理由をログに出す） |
| `--help` | 使い方を表示する |

撤去済みの実験フラグ（旧転送・ペース・注入系等）は受け付けない。指定すると usage を出して非ゼロ終了する。

通常は client（`client/src/main.ts`）から起動する。単体では同時に 1 本だけ起動すること（2 本目以降は XR 初期化に失敗して終了 1 になる）。

## プロトコルの要点

- 転送メッセージは 256 バイト固定（正本は `src/transfer/protocol.h`。TS 側の写しは `client/src/protocol.ts`）。
- 種別：`Offer`（複製ハンドル＋実サイズ）→ `Done`（GPU 完了確認）、`RenderRequest`（pose/FOV＋描画要求）、`InputState`（毎フレーム）、`HapticCommand`（main→exe）。
- main は借用台帳を持ち、`release()` は main だけが呼ぶ。exe は借用中の複製だけ扱い、所有権は取らない。
- 受領した複製ハンドルは import 成否に関わらず exe が閉じる。元の Electron 側は閉じない。
- import 側イメージのレイアウト遷移は内容を破棄しない（GENERAL 経由の 2 段階遷移）。
- 受理条件：renderId 照合・pose タグ一致・space 世代一致・TTL 内。棄却は `不明ID`・`pose不一致`・`space世代不一致`・`失効`に数える。
- 受理画像は hold イメージへ GPU 複製し、新規受理が無いフレームでも同じ pose/FOV で再提出する。
- 終了統計（`XR転送統計`・`入力統計`）を出した後、明示 teardown を済ませて `_Exit` で終わる（quick exit。理由は `../docs/architecture.md`）。

## ディレクトリ構成

| パス | 責務 |
|---|---|
| `src/main.cpp` | CLI（`--probe`・`--xr-transfer`・`--selftest`・`--help`）と起動 |
| `src/app.cpp`・`src/xr/` | ライフサイクル・セッション・入力アクション |
| `src/gfx/` | Vulkan 生成・swapchain・クリア描画 |
| `src/transfer/` | パイプ・共有テクスチャ import・照合・`--xr-transfer` 受信ループと統計 |
| `src/common/` | ログ・`XrResult` 文字列化 |
| `transfer-addon/` | NT ハンドル複製の N-API アドオン（cmake-js。本体 CMake とは分離） |
| `cmake/Dependencies.cmake` | FetchContent の取得定義 |

## 実機装着時にしか確認できない項目（チェックリスト。未検証）

- [ ] コントローラ装着・追従後に interaction profile が記録されること
- [ ] 左右 grip/aim の接続表示と pose 追従（HMD を動かしたときの値変化）
- [ ] trigger / squeeze のアナログ値（0〜1 の変化）
- [ ] thumbstick の 2 軸値と click、primary（A/X）/ secondary（B/Y）、menu の押下表示
- [ ] trackpad が非対応として出ること（PICO 系に trackpad は無い）
- [ ] haptics の体感（指令の送信時にコントローラが振動すること）
- [ ] FOCUSED 時の `xrSyncActions` 成功（統計の `sync` が進むこと）
- [ ] 再センタリング後の `REFERENCE_SPACE_CHANGE_PENDING` と `spaceEpoch` の進行、旧レコードの失効（統計の `space変更`）
- [ ] HMD 脱着で提出だけ止まり・復帰すること（exe が終了しないこと）
- [ ] SteamVR 再起動等の再接続で loss→新 `connEpoch` の再生成が走ること

検証手順は [`../docs/verification.md`](../docs/verification.md)。
