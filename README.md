# XRift-Client（暫定）

XRift クライアント（非公式テスト）: 本家 XRift（app.xrift.net）を表示する専用ブラウザであり、ページの WebXR（`navigator.xr`）を差し替えて OpenXR + Vulkan の GPU 内コピー経路で HMD に出す。XRift 公式とは関係ない。詳細は段階Eで拡充する。

## 入口

- `client/` … Electron アプリ（OSR 描画・借用台帳・WebXR シム）。手順の正本は [`client/README.md`](client/README.md)。
- `native/` … 単体 exe（`xrift_new.exe`。OpenXR + Vulkan・転送・入力・提出）と N-API アドオン（`transfer-addon/`）。手順の正本は [`native/README.md`](native/README.md)、設計は [`native/docs/DESIGN.md`](native/docs/DESIGN.md)。
- 旧アプリ（`src/`・`electron/`・旧 `native/`）は削除済み（段階A）。M語彙の改名は段階Dで行う。

## ビルド・起動・テスト（PowerShell。リポジトリルートから）

```powershell
pnpm install
pnpm electron:install   # Electron 本体の取得（pnpm では取得されないため）
pnpm build              # client のビルド（tsc）
pnpm build:native       # native/xrift_new.exe のビルド（cmake preset）
pnpm build:addon        # transfer-addon のビルド
pnpm start -- --url=<絶対パス>/tools/pages/xr-smoke.html --seconds=15  # client を起動
pnpm test               # client の単体試験
pnpm verify             # build + test
pnpm register           # SteamVR のライブラリに登録（SteamVR を起動しておく）
pnpm package            # 配布物（client/dist-package/）を作る
```

起動引数（`--exe`・`--addon`・`--url`・`--seconds` 等）の正本は [`client/README.md`](client/README.md)。

## 既知の制約

- Windows のみ検証。Linux 対応は将来対応。
- HMD 非装着では XR セッションが FOCUSED に届かず、提出（blit）の検証はできない。
- 色の最終確認は未決（`native/docs/DESIGN.md` §9）。手の pose（grip/targetRay）は未対応。
- `client` と `native` の検証器具（`tools/`）は製品経路ではない。
