# 検証

## 単体テスト

```powershell
pnpm test   # client の単体試験（tsc + tools/test/client の 5 件）
```

| 試験 | 対象 |
|---|---|
| `control-codec.test.cjs` | 制御ブロックのコーデックが C++ と一致すること |
| `protocol.test.cjs` | `RenderRequest`・`InputState` の復号と `Offer`・`HapticCommand` の生成 |
| `shim-pose.test.cjs` | WebXR シムの pose 処理 |
| `mirror-map.test.cjs` | ミラー canvas と OSR content の座標対応 |
| `mirror-input.test.cjs` | 入力境界検証 |

## native の検証（HMD 不要）

```powershell
native/build/windows/Release/xrift_native.exe --selftest   # 値層（配置・pose タグ・受理/棄却）。all ok が期待値
native/build/windows/Release/xrift_native.exe --probe      # インスタンスと Vulkan 初期化。ランタイム名・system 名・GPU 名・推奨解像度・swapchain フォーマット候補を出す
```

`--selftest` の正本は `tools/selftest/native/transfer_selftest.cpp` である。

## client の検証（HMD 不要）

```powershell
npx electron client --selftest   # tools/pages/mirror-input.html でミラー・入力・VR リサイズ・停止を自動検証。終了 0 が期待値
```

証拠は `client/logs/`（`mirror.png`・`mirror-vr.png`・`mirror-resize.png` 等）。

## シミュレータでの統合スモーク

```powershell
$env:XR_RUNTIME_JSON = "<シミュレータの JSON への絶対パス>"
npx electron client --url=<絶対パス>/tools/pages/xr-smoke.html --seconds=15
```

期待する結果は次の通り。

- `xrActive=true`、`offers>0`、受理＞0、再提出＞0、`[VR計測]` の提出＞0
- app 終了コード 0、exe 終了コード 0、統計行（`exe統計: XR転送統計:`）あり
- 残存プロセスなし（client と exe が残らないこと）
- 期待ログ：セッション開始、描画要求（`[描画要求]`）、Offer/Done、XR 提出（`XR 提出:`）、再提出（統計の `再提出=`）、`XR転送統計`

注意：`--xr-transfer` は同時に 1 本だけ起動すること。並行実行すると証拠（`client.log`・`stats.json`）を上書きし合い、セッションも壊れる。

## PICO / SteamVR の目視確認項目

1. SteamVR を起動し、HMD を装着する（単独で実行すること）。
2. `npx electron client --seconds=20` で起動する。
3. `logs/client.log` に `exe がパイプに接続` と `[XRift XR] session started` が出ること。
4. HMD 内に three.js のシーン（グリッド・色キューブ・`XRift XR test` の文字・左右のマゼンタ/シアン板）がステレオで見え、頭の向きに追従すること。
5. ページ HUD に `presenting=true` とコントローラの `trigger`/`squeeze`/`stick`/ボタン表示が出て、ボタンを押すと表示が変わること。
6. `logs/stats.json` の `offers`/`dones`・`statusOk` と、exe 側の `XR転送統計`（受理・blit・層あり）を記録すること。
7. ミラー窓（コントロールウィンドウ）から OSR へのマウス・ホイール・キー入力が届くこと。
8. haptics 指令の送信時にコントローラが振動すること。
9. 終了は Ctrl+C（`--seconds` を付けた場合は自動停止）。終了コード 0 と残存なしを確認すること。

## 未確認事項

- HMD 非装着では XR セッションが FOCUSED に届かず、提出（blit）の検証はできない（blit=0 は正常）。
- 出力色の最終確認（二重符号化の有無）は未決である。HMD 実機で行う。
- 手の pose（grip/targetRay）は未対応である。
- 実機のコントローラ操作・haptics の体感・追従、再センタリング後の `spaceEpoch` 進行、HMD 脱着・再接続の復帰は、実機があるときに確認する（項目は `native/README.md` の「実機装着時にしか確認できない項目」を参照）。
