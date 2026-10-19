<p align="center">
  <img width="160" src="assets/icon.png"/>
</p>
<p align="center">
  ※本家 XRift（app.xrift.net）を SteamVR で遊ぶための非公式テストクライアントです。XRift 公式とは関係ありません。
</p>
<p align="center">
  <a href="https://github.com/KinuX2/XRift-Client/releases/latest/download/XRift-Client-win-x64.zip"><img src="https://img.shields.io/badge/DOWNLOAD-WINDOWS%20(ZIP)-565b63?style=for-the-badge&logo=download" alt="Windows 版をダウンロード"></a>
  <a href="https://github.com/KinuX2/XRift-Client/blob/main/LICENSE"><img src="https://img.shields.io/badge/LICENSE-MIT-565b63?style=for-the-badge" alt="ライセンス"></a>
  <img src="https://img.shields.io/badge/PLATFORM-WINDOWS%20%2B%20STEAMVR-565b63?style=for-the-badge" alt="Windows + SteamVR">
  <img src="https://img.shields.io/badge/STATUS-TEST-c08a2b?style=for-the-badge" alt="テスト版">
</p>

# XRift-Client
### -XRift クライアント（非公式テスト）-
  <a href="https://github.com/KinuX2/XRift-Client/releases/latest"><img src="https://img.shields.io/github/v/release/KinuX2/XRift-Client?include_prereleases&style=for-the-badge&label=VERSION" alt="最新リリース"></a>

- 本家フロントエンド（app.xrift.net）をそのまま表示する専用ブラウザ。ログイン・インスタンス・マルチプレイ・ボイス・アバター・UI は本家のまま
- ページの WebXR（`navigator.xr`）を差し替えて、SteamVR（OpenVR）へ出力。VRChat などと同じ経路なので ALVR でも表示できる
- 解像度は SteamVR の推奨値どおり（SteamVR の解像度設定に従う）
- 低遅延: 描いたフレームをその場で SteamVR に提出（遅れ 0）。間に合わないフレームだけ 1 フレーム遅れで出し、fps は落とさない
- SteamVR のライブラリから起動できる（アプリのメニューから登録）

## 動作確認した環境

| 項目 | 内容 |
| --- | --- |
| OS | Windows 11 |
| GPU | Radeon RX 9070 XT |
| HMD | Meta Quest 2（ALVR 経由）|
| SteamVR | 2.17 |
| 解像度・リフレッシュレート | 片目 1920×1920（SteamVR の推奨値 100%）・72 Hz |
| 負荷テスト | メッシュ 1500 個・影あり（60 秒）で 72 fps、約 99% のフレームを遅れ 0 で提出、SteamVR 側の取りこぼし 0 |
| 実際のワールド | 通常は 62〜71 fps で、大半のフレームを遅れ 0 で提出。ページ自体の処理が 1 フレーム 14〜17ms かかる重い場所では 36 fps |

## インストール

1. 上のボタン（または [Releases](https://github.com/KinuX2/XRift-Client/releases/latest)）から `XRift-Client-win-x64.zip` をダウンロードして、好きな場所に解凍
2. SteamVR を起動しておく
3. `XRiftVR.exe` を起動し、メニュー「XRift」→「SteamVR のライブラリに登録」（初回のみ）

必要なもの: Windows 10/11（x64）、SteamVR。

### ソースからビルドする場合

Node.js 22.15 以上、Visual Studio 2022（C++ によるデスクトップ開発）、CMake、Windows SDK が必要です。

```powershell
git clone https://github.com/KinuX2/XRift-Client.git
cd XRift-Client
pnpm install
pnpm electron:install   # Electron 本体の取得（pnpm のインストールでは取得されないため）
pnpm build:native                     # bin/xrift_openxr.node と XRiftVR.exe をビルド
pnpm register                         # SteamVR のライブラリに「XRift VR」を登録（SteamVR を起動しておく）
pnpm start                                # 起動
pnpm package                          # 配布用 zip（dist/XRift-Client-win-x64.zip）を作る
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
| `--register` / `--unregister` | ウィンドウを出さずに SteamVR へ登録 / 解除して終了（`--world` と組み合わせると開くワールドを固定） |

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

- SteamVR のフレーム待ちを行うスレッドが、次のフレーム開始の 4ms 前までそのフレームの転送完了を待ってから提出します（遅れ 0）。間に合わなかった枠だけはその時点の最新の画像を出し、fps は落としません
- 画像は描いたときの頭の姿勢と一緒に提出するので、遅れたフレームも SteamVR が補正します
- 目のテクスチャは共有可能な形式で作り、SteamVR が提出時にコピーしなくて済むようにしています
- ページと GPU のプロセスは優先度「高」で動かし、1 フレームの持ち時間（72 Hz なら 13.9ms）に収めやすくしています
- WebXR の `local-floor` の原点は、ブラウザと同じく「VR に入ったときの立ち位置と向き」にしています（SteamVR のプレイエリアの中心ではない）
- コントローラのビームの向きは SteamVR のレンダーモデルの `tip`、握りの位置は `handgrip` の姿勢を使っています（WebXR の targetRaySpace / gripSpace に相当）
- SteamVR は提出やフレーム待ちの中でアプリの D3D11 コンテキストを使うため、D3D11 のマルチスレッド保護を有効にしています
- 5秒ごとのログには、`submitTiming`（描画終了→SteamVR の Submit、upload 完了→Submit、Submit/Handoff の内訳）と `frameTiming`（SteamVR 側の GPU・コンポジタ・転送）も出ます。`renderToSubmit` は描画終了タグが有効なフレームだけの平均です

## 困ったとき

| 症状 | 対処 |
| --- | --- |
| VR に入っても映像が止まる・SteamVR のグリッドのまま | 既定の OpenVR で起動しているか確認してください（`--backend openxr` を付けていないか）。ALVR + SteamVR 2.17 の環境では、OpenXR のアプリは SteamVR に表示してもらえないことがあります |
| 「SteamVR のライブラリに登録」に失敗する | SteamVR を起動してから、もう一度メニューから登録してください |
| 重い・カクつく | `%LOCALAPPDATA%\xrift-openxr-client\browser.log` の `page render` がページ自体の 1 フレームの処理時間です。72 Hz で遊ぶには 13.9ms 以内が必要で、超えると SteamVR が 36 fps に落とします。SteamVR の解像度を下げても、この時間は変わりません |
| 「an immersive session is already active」と出て VR に入れない | 前の VR セッションが終わりきっていません。少し待つか、アプリを再起動してください |

## 既知の制限

- 非公式のテスト版です。本家の仕様変更で動かなくなることがあります
- ページ自体の処理が重いワールドの場所では 36 fps に落ちます（本家のワールドとページの処理量によるもので、ブラウザ版の WebXR でも同じ処理が必要です）
- `--backend openxr` は環境によって SteamVR が表示状態に進まないことがあります（ALVR + SteamVR 2.17 で確認）。その場合は既定の OpenVR を使ってください

## テスト

```powershell
pnpm browser:selftest                                         # three の WebGL ページで VR に入り、デスクトップのミラーを保存
pnpm exec electron . --selftest --selftest-page selftest-r3f.html       # 本家と同じ R3F + @react-three/xr 構成
pnpm exec electron . --selftest --selftest-page selftest-heavy.html --selftest-seconds 60   # 負荷テスト
node test/openvr-smoke.cjs                                       # ネイティブ層だけで SteamVR に単色を送る
node test/native-smoke.cjs                                       # 同じく OpenXR 経路
```

VS Code のターミナルなど `ELECTRON_RUN_AS_NODE=1` が設定された環境では、その変数を外してから `pnpm exec electron` を実行してください（配布版の `XRiftVR.exe` はこの変数を無視します）。

## 開発について

このプロジェクトは、Anthropic の Claude（Claude Code）を使って開発しました。
調査・設計・実装・計測とチューニングまでを、Claude との対話で進めています。

## ライセンス

MIT License（[LICENSE](LICENSE)）。同梱の OpenXR SDK（Apache-2.0）と OpenVR SDK（BSD-3-Clause）はそれぞれのライセンスに従います（[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)）。
XRift は XRift 運営の、SteamVR は Valve の、ALVR は ALVR プロジェクトの、それぞれ商標・著作物です。
