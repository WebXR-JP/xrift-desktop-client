<p align="center">
  <img width="160" src="assets/icon.png"/>
</p>
<p align="center">
  ※本家 XRift（app.xrift.net）を SteamVR で遊ぶための非公式テストクライアントです。XRift 公式とは関係ありません。
</p>
<p align="center">
  <a href="https://github.com/KinuX2/XRift-Client/blob/main/LICENSE"><img src="https://img.shields.io/github/license/KinuX2/XRift-Client?style=for-the-badge" alt="ライセンス"></a>
  <img src="https://img.shields.io/badge/PLATFORM-WINDOWS%20%2B%20STEAMVR-565b63?style=for-the-badge" alt="Windows + SteamVR">
  <img src="https://img.shields.io/badge/STATUS-TEST-c08a2b?style=for-the-badge" alt="テスト版">
</p>

# XRift-Client
### -XRift クライアント（非公式テスト）-

- 本家フロントエンド（app.xrift.net）をそのまま表示する専用ブラウザ。ログイン・インスタンス・マルチプレイ・ボイス・アバター・UI は本家のまま
- ページの WebXR（`navigator.xr`）を差し替えて、SteamVR（OpenVR）へ出力。VRChat などと同じ経路なので ALVR でも表示できる
- 解像度は SteamVR の推奨値どおり（SteamVR の解像度設定に従う）
- SteamVR のライブラリから起動できる（`npm run register`）
- おまけ: Node.js + WebGPU（Dawn）+ three.js XRManager で公開ワールドを直接描く実験モード

## 動作確認した環境

| 項目 | 内容 |
| --- | --- |
| OS | Windows 11 |
| GPU | Radeon RX 9070 XT |
| HMD | Meta Quest 2（ALVR 経由）|
| SteamVR | 2.17 |
| 解像度・リフレッシュレート | 片目 1920×1920（SteamVR の推奨値 100%）・72 Hz |
| 結果 | 実際のワールドで 60〜72 fps、負荷テストで 60 秒間 72 fps（取りこぼし 4299 中 1） |

## インストール

必要なもの: Windows 10/11（x64）、SteamVR、Node.js 22.15 以上。
ビルドに Visual Studio 2022（C++ によるデスクトップ開発）、CMake、Windows SDK が必要です。

```powershell
git clone https://github.com/KinuX2/XRift-Client.git
cd XRift-Client
npm install
node node_modules/electron/install.js   # Electron 本体の取得（npm のインストールスクリプトが無効な環境のみ）
npm run build:native                     # bin/xrift_openxr.node と XRiftVR.exe をビルド
npm run register                         # SteamVR のライブラリに「XRift VR」を登録（SteamVR を起動しておく）
```

## 使い方

1. SteamVR のライブラリから「XRift VR」を起動（デスクトップから `XRiftVR.exe` でも可）
2. ウィンドウで本家にログインし、ワールド → インスタンスに入る
3. 画面の「🥽 VR」ボタン、または **Ctrl+Shift+V** で VR に入る
4. 終了は SteamVR のダッシュボード、または **Ctrl+Shift+X**

| オプション | 説明 |
| --- | --- |
| `--world <ワールドID>` | そのワールドのページを開く |
| `--scale <倍率>` | SteamVR 推奨解像度に対する倍率（既定 1） |
| `--backend openxr` | OpenVR ではなく OpenXR で出力する（既定は openvr） |
| `npm run register -- --world <ID>` | SteamVR から起動したときに開くワールドを固定 |
| `npm run register -- --remove` | SteamVR の登録を解除 |

ログイン状態は Electron のユーザーデータ（`%APPDATA%\XRift VR`）に保存され、リポジトリには含まれません。
動作ログは `%LOCALAPPDATA%\xrift-openxr-client\browser.log` に出ます（不具合の報告に使えますが、公開する前に中身を確認してください）。

## 仕組み

```
app.xrift.net（three r185 WebGLRenderer + @react-three/xr + LiveKit）※本家のコードは無改造
   │ navigator.xr.requestSession('immersive-vr')
   ▼
electron/webxr-mainworld.cjs   ページ内に注入した WebXR 実装（XRSession / XRFrame / 入力 / XRWebGLLayer）
   │ three が両眼を横並びでキャンバスに描く
   ▼
electron/preload.cjs           Chromium の WebGPU でキャンバスを GPU コピー → 非同期で読み戻し
   │ ページにはこの XR 用の関数しか見せない（ワールドのコードに Node の機能を渡さない）
   ▼
native/（C++ / N-API）         ステージングへ並列コピー → 毎フレーム定刻に「最新の画像＋描いたときの姿勢」を提出
   ▼
SteamVR（OpenVR）→ ALVR など
```

- 画像は描画から 1〜2 フレーム遅れて届きますが、描いたときの頭の姿勢と一緒に提出するので SteamVR が補正します
- SteamVR は提出やフレーム待ちの中でアプリの D3D11 コンテキストを使うため、D3D11 のマルチスレッド保護を有効にしています

## 既知の制限

- 非公式のテスト版です。本家の仕様変更で動かなくなることがあります
- コントローラの「指す向き」は OpenVR のコントローラ姿勢をそのまま使っているため、ブラウザ版とレーザーの角度が少し違うことがあります
- `--backend openxr` は環境によって SteamVR が表示状態に進まないことがあります（ALVR + SteamVR 2.17 で確認）。その場合は既定の OpenVR を使ってください

## 実験モード（Node.js クライアント）

Electron を使わず、Node.js + WebGPU（npm `webgpu` / Dawn）+ three.js の XRManager で公開ワールドを直接描きます。
ログインが必要な機能（マルチプレイ・ボイスなど）はありません。

```powershell
npm run list                                   # 公開ワールドの一覧
npm run native -- --world "XRift The Lobby"    # VR で入る
node src/main.js --world <ID> --screenshot out.png   # HMD なしで描画を確認
```

## テスト

```powershell
npm run browser:selftest                                         # three の WebGL ページで VR に入り、デスクトップのミラーを保存
npx electron . --selftest --selftest-page selftest-r3f.html       # 本家と同じ R3F + @react-three/xr 構成
npx electron . --selftest --selftest-page selftest-heavy.html --selftest-seconds 60   # 負荷テスト
node test/openvr-smoke.cjs                                       # ネイティブ層だけで SteamVR に単色を送る
```

VS Code のターミナルなど `ELECTRON_RUN_AS_NODE=1` が設定された環境では、その変数を外してから `npx electron` を実行してください（`XRiftVR.exe` は自動で外します）。

## 開発について

このプロジェクトは、Anthropic の Claude（Claude Code）を使って開発しました。
調査・設計・実装・計測とチューニングまでを、Claude との対話で進めています。

## ライセンス

MIT License（[LICENSE](LICENSE)）。同梱の OpenXR SDK（Apache-2.0）と OpenVR SDK（BSD-3-Clause）はそれぞれのライセンスに従います。
XRift は XRift 運営の、SteamVR は Valve の、ALVR は ALVR プロジェクトの、それぞれ商標・著作物です。
