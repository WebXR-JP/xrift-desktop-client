# client（Electron アプリ）

責務：パイプサーバ＋借用台帳＋`release()` 専任。exe を spawn し、paint を Offer、Done で返却する。`RenderRequest`・`InputState` を renderer へ中継し、renderer からの haptics IPC を exe へ送る。提出には関与しない。

## 構成

| ファイル | 責務 |
|---|---|
| `src/main.ts` | パイプサーバ・借用台帳・exe の spawn・ミラー・入力転送 |
| `src/protocol.ts` | パイプ配置の読み取り専用の写し（正本は `native/src/transfer/protocol.h`） |
| `src/preload.ts` | `window.__xriftXR` だけを公開し、シムを注入する。Node の機能は渡さない |
| `src/control-codec.ts` | 制御ブロックのコーデック（正本は C++ の `render_record.h`・`control_blocks.h`） |
| `src/shim.ts` | WebXR シム。`present` は無い（OSR の paint が提出を駆動する）。制御ブロックはページの描画コールバックを呼び終えた後に overlay canvas へ描く |
| `src/mirror-input.ts`・`src/control-preload.ts` | 入力境界・コントロール用ブリッジ |
| `pages/control.html`・`pages/mirror-map.js` | ミラー表示（レターボックス往復の正本は `mirror-map.js`） |
| `scripts/package.cjs` | 配布物作成。`scripts/register-steamvr.cjs` | SteamVR 登録 |
| `logs/` | 実行証拠（`client.log`・`stats.json`・capture PNG。コミットしない） |

## 引数

```powershell
npx electron client --url=<絶対パス>/tools/pages/xr-smoke.html --seconds=20
```

| 引数 | 既定 | 意味 |
|---|---|---|
| `--exe=<path>` | native のビルド物 | `xrift_native.exe`（`--packaged` 時は resources 直下） |
| `--addon=<path>` | native のビルド物 | transfer-addon の `.node` |
| `--pipe=<name>` | `\\.\pipe\xrift_<pid>` | パイプ名 |
| `--render-scale=0.5` | 0.5 | exe への要求倍率（0.2〜1.0） |
| `--record-ttl-ms=2000` | 2000 | exe へ渡す描画レコード失効上限（ms） |
| `--space=<auto\|local\|stage\|local-floor>` | auto | exe の参照空間の要求 |
| `--seconds=N` | 0 | N 秒で自動停止（0=手動終了） |
| `--width=W --height=H` | 1280x800 | OSR ウィンドウ寸法（VR 開始で 2\*eye×eye に広げる） |
| `--url=<url\|パス>` | `https://app.xrift.net/` | 開く対象（正典） |
| `--mirror-fps=N` | 10 | ミラーの目標 fps |
| `--mirror-vr-fps=N` | 0 | VR 中のミラー fps（0=停止） |
| `--control-width=W --control-height=H` | 960x600 | コントロール窓の寸法 |
| `--exe-log=<path>` | `native.log` | exe の stdout/stderr の保存先（logs/ 基準。空で保存しない） |
| `--selftest` | — | ミラー・入力・VR リサイズ・停止を自動検証し終了（HMD 不要） |

削除済み・未知の `--*` はエラー終了する。

## ミラー

- OSR（不可視）の `capturePage` をタイマーで拾い、コントロール窓へ送る（通常 10fps。VR 中は既定で停止）。
- 色は BGRA→RGBA に直して canvas に載せる。窓のリサイズにミラーも追従する。
- ミラー窓からのマウス・ホイール・キー入力を OSR へ転送する（転送直前に OSR の `webContents.focus()` を呼ぶ）。

## ログ

- `logs/client.log`（stdout とファイルの両方。`[描画要求]`・`[VR計測]`・`[入力]`・`exe統計: XR転送統計:` 等）
- `logs/stats.json`（`offers`・`dones`・`statusOk`・`droppedStale` 等）
- VR 中は 5 秒ごとに `[VR計測]` を出す（paint レートと exe の受理率の内訳。描画と転送の切り分け用）。

## テスト

```powershell
pnpm --dir client test   # tsc + tools/test/client の 5 件
npx electron client --selftest   # ミラー・入力・VR リサイズ・停止の自動検証（終了 0 が期待値）
```

## package と register

```powershell
npm run package                        # 配布物（dist-package/XRiftClient-win32-x64/）
npm run register -- add | check | remove [--packaged]   # SteamVR 登録・確認・解除
```

`XRiftClient.exe --selftest` で addon/exe のパス解決を含めて検証できる。開発版と配布版は同一 `app_key`（`net.xrift.client`）のため排他である。詳細は [`../docs/development.md`](../docs/development.md)。
