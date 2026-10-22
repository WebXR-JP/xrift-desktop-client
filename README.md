# XRift-Client

XRift（`https://app.xrift.net/`）を表示する専用ブラウザであり、ページの WebXR（`navigator.xr`）を差し替えて OpenXR + Vulkan の GPU 内コピー経路で HMD に出す。XRift 公式とは関係ない。

## 対応環境

- Windows（x64）のみ検証。Linux は将来対応（preset だけ用意）。
- SteamVR（または OpenXR 互換ランタイム）。HMD は PICO 4（Ultra）等での目視確認を想定する。
- HMD 非装着でも起動とパイプ接続までは進むが、XR セッションは FOCUSED に届かず提出（blit）の検証はできない。

## 利用方法（PowerShell。リポジトリルートから）

```powershell
pnpm install
pnpm electron:install   # Electron 本体の取得（pnpm では取得されないため）
pnpm build              # client のビルド
pnpm build:native       # xrift_native.exe のビルド
pnpm build:addon        # transfer-addon のビルド
pnpm start -- --url=<絶対パス>/tools/pages/xr-smoke.html --seconds=15
```

既定では `https://app.xrift.net/` を開く。主な起動引数は次の通り（正本は [`client/README.md`](client/README.md)）。

| 引数 | 意味 |
|---|---|
| `--url=<url\|パス>` | 開く対象。`https?://`・`file://` は URL として開き、それ以外は検証ページとして開く |
| `--render-scale=0.5` | exe への要求倍率（0.2〜1.0。既定 0.5。本番サイトでは 0.35 程度を試せる） |
| `--seconds=N` | N 秒で自動停止（既定 0=手動終了。Ctrl+C で終了） |
| `--space=<auto\|local\|stage\|local-floor>` | exe の参照空間の要求（既定 auto） |
| `--mirror-fps=N` | ミラーの目標 fps（既定 10） |
| `--mirror-vr-fps=N` | VR 中のミラー fps（既定 0=停止。VR の負荷を優先） |

ミラー操作：起動すると OSR のミラーを出すコントロール窓が開く。ミラー窓から OSR へマウス・ホイール・キー入力を転送できる（窓のリサイズにミラーも追従する）。

SteamVR 登録と配布：

```powershell
pnpm register -- add              # 開発版を登録（SteamVR を起動しておく）
pnpm register -- check            # 登録確認
pnpm register -- remove           # 登録解除
pnpm register -- add --packaged   # 配布版を登録（下の package の後）
pnpm --dir client run package     # 配布物（client/dist-package/）を作る
```

## リポジトリ構成

| パス | 内容 |
|---|---|
| `client/` | Electron アプリ（OSR 描画・借用台帳・WebXR シム）。手順は [`client/README.md`](client/README.md) |
| `native/` | 単体 exe（`xrift_native.exe`。OpenXR＋Vulkan・転送・入力・提出）と N-API アドオン（`transfer-addon/`）。手順は [`native/README.md`](native/README.md) |
| `tools/` | 検証ページ（`pages/`）・単体試験（`test/client/`）・native selftest（`selftest/native/`）。製品経路ではない |
| `docs/` | 設計と手順。[`docs/architecture.md`](docs/architecture.md)・[`docs/development.md`](docs/development.md)・[`docs/verification.md`](docs/verification.md) |

## 開発入口

- 手順の正本は [`docs/development.md`](docs/development.md)、設計は [`docs/architecture.md`](docs/architecture.md)、検証は [`docs/verification.md`](docs/verification.md)。
- `pnpm typecheck`（型検査）、`pnpm test`（client の単体試験）、`pnpm verify`（build＋test）。

## 既知の制約

- Windows のみ検証。Linux 対応は将来対応。
- SteamVR の VRAM が XR セッション 1 回あたり約 300MB 増え、SteamVR 再起動で解放される（詳細は `docs/architecture.md`）。
- exe の通常出口は統計と明示 teardown の後に `_Exit` で終わる（終了コード確定のため。詳細は `docs/architecture.md` の「quick exit」）。
- 手の pose（grip/aim）は対応済みであり、`gripSpace`/`targetRaySpace` の `getPose` が返す。未接続・locate 失敗・値不正は null になる。出力色の最終確認は未決（HMD 実機で行う）。
