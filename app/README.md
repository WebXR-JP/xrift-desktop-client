# XRift M5 新アプリ（骨格 + WebXR シム + OSR/パイプ統合）

M5 の最初のスライス。exe 主導の描画要求（`RenderRequest`）を OSR で描き、
共有テクスチャで exe へ返す。ページには WebXR シム経由で three.js を動かす。
検証用ハーネス（`native_new/harness/electron/main.js`）の借用台帳・パイプサーバ・
終了経路を TypeScript に整理して流用している。

## 構成と責務

| ファイル | 責務 |
|---|---|
| `src/main.ts` | パイプサーバ + 借用台帳 + `release()` 専任。exe を spawn し、paint を Offer、Done で返却。`RenderRequest`/`InputState` を renderer へ中継し、renderer からの haptics IPC を exe へ送る。提出には関与しない |
| `src/protocol.ts` | パイプ配置の読み取り専用の写し（正本は `native_new/src/transfer/protocol.h`）。`RenderRequest`/`InputState` の復号、`Offer`/`HapticCommand` の生成 |
| `src/preload.ts` | `contextBridge` で `window.__xriftXR`（`onRecord`/`onInput`/`haptic`/`probe`）だけを公開し、`executeInMainWorld` でシムを注入する。Node の機能は渡さない |
| `src/m2codec.ts` | M2 制御ブロックのコーデック（正本は C++ の `render_record.h`・`m2_blocks.h`。harness の `m2-codec.js` の移植）。量子化・pose タグ・ブロック符号化。`test/m2codec.test.cjs` で C++ との一致を確認する |
| `src/shim.ts` | WebXR シム。移植元は `electron/webxr-mainworld.cjs`（API 形状・数学・イベントロジックは同一）。旧ブリッジ `B` を新ブリッジに置換。`present` は無い（OSR の paint が提出を駆動する）。record ごとに M2 制御ブロック（renderId/pose タグ・左右マーカー）を overlay canvas に描く |
| `pages/test.html` | three.js のテストページ（グリッド + 色キューブ + テキスト板 + 左右識別板 + 入力 HUD）。読み込み後に自動で `immersive-vr` セッションを開始する。HUD には `input sources: N`・左右の接続（`__m5input`）・直近の入力イベント（`select L`・`squeeze R` 等）を出し、`select` では `hapticActuators[0].pulse(0.8, 120)` を打つ |
| `logs/` | 実行証拠（`m5-run.log`、`m5-stats.json`、capture PNG）。生成物でありコミットしない |

## 前提

- ビルド済み `native_new/build/windows/Release/xrift_new.exe`
- ビルド済み `native_new/transfer-addon/build/Release/xrift_transfer_addon.node`
- root の `node_modules`（`electron`、`three`。新規インストールはしない）
- SteamVR（HMD 非装着でも exe の起動とパイプ接続までは進むことを期待する）

## 起動方法

```powershell
# XRift-Client から
npx tsc -p app                 # ビルド（app/dist へ）
npx electron app               # 起動（pages/test.html を開く。Ctrl+C で終了）

# 20 秒の非装着検証（15 秒時点でページを PNG 保存）
npx electron app --seconds=20 --capture-at=15000,m5-cap.png

# 引数（いずれも任意）
#   --exe=<path>           xrift_new.exe（既定は native_new のビルド物）
#   --addon=<path>         transfer-addon の .node（既定は native_new のビルド物）
#   --pipe=<name>          パイプ名（既定は \\.\pipe\xrift_m5_<pid>）
#   --render-scale=0.5     exe への要求倍率
#   --space=<auto|local|stage|local-floor>  exe の参照空間の要求（既定 auto）
#   --seconds=N            N 秒で自動停止（既定 0=手動終了）
#   --width=W --height=H   OSR ウィンドウ寸法（既定 2048x1024）
#   --page=<file>          pages/ 以下の HTML（既定 test.html。`--url` があればそちらが優先）
#   --url=<url>            開く対象（正典）。`https://…`/`file://…` は URL として開き、
#                          それ以外（`pages/test.html`・`test.html` 等）は pages 配下の
#                          ファイルとして開く。既定は `pages/test.html`。
#                          例: `--url=pages/test.html`、`--url=https://app.xrift.net/`
#   --capture-at=Ms,path[;Ms,path]  指定時刻に capturePage で PNG 保存（描画経路に入れない）
#   --exe-log=<path>       exe の stdout/stderr の保存先（既定 m5-exe.log。logs/ 基準。空で保存しない）
```

root の `package.json` にもある（`pnpm app:build`、`pnpm app:start`）。

## 実機（HMD 装着時）の確認手順

1. **単独で実行すること**。`xrift_new.exe --xr-transfer` は同時に 1 本だけ
   （2 本目以降は XR ランタイムの初期化に失敗して終了 1 になる。
   並行実行すると互いの証拠（`logs/m5-run.log`、`logs/m5-stats.json`）を
   上書きし合い、セッションも壊れる）。
2. SteamVR を起動し、HMD を装着する（PICO Connect 等のストリーミングも可）。
3. `npx electron app --capture-at=15000,m5-cap.png` で起動する。
4. `logs/m5-run.log` に `exe がパイプに接続` と renderer の
   `[XRift M5] session started, layer <W> x <H>` が出ることを確認する。
5. HMD 内に three.js のシーン（グリッド・色キューブ・`XRift M5 test` の文字・
   左右のマゼンタ/シアン板）がステレオで見え、頭の向きに追従すること。
6. ページ HUD（capture 画像にも写る）に `presenting=true` と
   コントローラの `trigger`/`squeeze`/`stick`/ボタン表示が出ること。
   ボタンを押して表示が変わること。
7. `logs/m5-stats.json` の `offers`/`dones`、`statusOk` を記録する。
   exe 側の標準出力（`XR転送統計`）の `受理`・`blit`・`層あり` も記録する。
8. 終了は Ctrl+C（`--seconds` を付けた場合は自動停止）。

## 非装着実行の実測（2026-10-20、RTX 3090 + SteamVR/PICO 系ランタイム）

- exe は XR セッションを SYNCHRONIZED 付近まで進めるが FOCUSED には届かず、
  `RenderRequest` は起動直後の 1 件のみ。以後は `InputState`（約 30/s、未接続・
  inactive）のみが流れる。HMD 無しで `offers>0` を超える提出検証はできない
  （blit は FOCUSED 時のみ。非装着で blit=0 は正常）。
- 転送路自体は回ることを確認済み（`app/logs/m5-console7.log`）。
  `paints`（90fps）→ `Offer` → `Done`（`statusOk`）→ `release`、
  `maxHeld≤2`、`quarantine=0`、`offers=dones` で終了 0 の回あり。
  - 非装着で record が 1 件しか来ない場合、ページはその 1 フレームを描いて
    静止する。以後の paint が止まると exe は「Offer 5 秒途絶」で統計を出して
    正常終了し、連鎖して main も終わる（設計どおり）。
- XR フレームの描画証拠: ページが `canvas=7144x1786`（要求 3572x1786/眼）に
  広げ、ブロック A の bit0 中心画素を `readPixels` で `[255,255,255]` と確認
  （`[XRift M5] xr frame id=1 ... bit0=[255,255,255]`）。
- 共有テクスチャの向き: 上記の drawing buffer 画素は正しいのに exe の復号が
  `id16=0x0000` だったため、OSR 共有テクスチャは bottom-up のまま渡される
  （compositor が表示時に反転する）と判断。テストページはブロック・
  マーカーを上下対称に刻む（`pages/test.html` の ADR を参照）。
  この対応後の `受理=1` は未確認（並行実行の干渉で取り直しが必要）。
- exe の警告（既知・未決）: `swapchain は VK_FORMAT_B8G8R8A8_SRGB` で
  受理画像と不一致の可能性（二重符号化の疑い。DESIGN §9 の色未決）。
  提出画像の色の最終確認は HMD 実機で行うこと。

## 現時点の制約（未対応）

- 手の pose（grip/targetRay）は未対応。pipe の `InputState` が pose を運ばないため、
  `gripSpace`/`targetRaySpace` の `getPose` は `null` を返す。ボタン・軸・振動は有効。
- 色は未決（exe 側の DESIGN §9 に準じる）。sRGB のみの環境では警告が出る場合がある。
- `positionTracked` は常に `true` を報告する（非装着時の無効 pose の検出はしない）。
- `isSessionSupported('immersive-vr')` は exe の準備ができるまで最大 15 秒待ってから
  結果を返す。`requestSession` は最初の `RenderRequest` まで最大 20 秒待つ。
- `window.open` は VR 用 OSR には載せず、認証用ポップアップとして通常の可視ウィンドウで
  開く（OSR は VR 用の 1 枚だけ）。外部ホストへの遷移は阻止せず、`will-navigate` に記録する。
  本番のホーム画面は VR に自動で入らないため、`isSessionSupported`/`requestSession` の
  呼び出しはワールド入室等の操作があるまでログに出ない。
- セッション状態（begin/end/visibility）は main が管理しない。
  シムは起動時から `visible` として record 駆動でフレームを進める。
- SteamVR への登録（ライブラリ登録・ランチャー）は未対応。
- `--url` での外部ページ（app.xrift.net 等）はホーム画面の読み込みとシム注入まで確認。
  XR 呼び出しはページ側の操作待ち（詳細は「非装着実行の実測」を参照）。
- 旧アプリ（`src/electron`・`electron/`・`native/`）とは無関係に動く。
  共通の依存（`electron`、`three`）は root の `node_modules` を使う。
