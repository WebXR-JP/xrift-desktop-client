# M1 ハーネス（検証用。 本番 TS ではない）

M1（別 exe・XR 指定 GPU への import、コピー、返却）の検証ハーネス。
Electron 側は借用台帳と `release()` 専任に徹し、提出には関与しない。
本番 TypeScript の作り直しは M5 で行う。ここにある `main.js` は使い捨ての検証器具である。

## 構成と責務

- `main.js` … パイプサーバ + 借用台帳 + `release()` 専任。exe を起動し、
  paint ごとに NT ハンドルを複製して Offer を送り、Done でその借用だけ返す。
  同時保持は処理中 1 枚 + 保留 1 枚まで。超過した古い保留は即 `release()` する。
- `index.html` … WebGPU の動的パターン（4096x2048 固定、M1b タイルパターン、4 隅固定色、
  フレーム番号埋め込み、注入フック付き）。
- `protocol-test.js` … プロトコル配置の試験（素の node で実行可）。
- `addon-test.js` … アドオン異常系の試験（electron で実行）。
- `run.ps1` … 再現手順の実行器。

## 再現手順

```powershell
# native/harness/electron から。既定 60 秒・4096x2048。
.\run.ps1# 短時間の煙試験（10 秒・2048x1024）。
.\run.ps1 -Seconds 10 -Width 2048 -Height 1024
# 検査自体の妥当性確認（100 フレーム目だけ 1 タイルを別色にする。不一致が出る想定）。
.\run.ps1 -Seconds 15 -Width 2048 -Height 1024 -InjectBadTile 100
# 世代混在の模擬（1 タイルだけ f-1 で描く）。
.\run.ps1 -Seconds 15 -Width 2048 -Height 1024 -InjectStaleTile 100
# 隅・番号セルの破壊（検出できることの確認用）。
.\run.ps1 -Seconds 15 -Width 2048 -Height 1024 -InjectBadCorner 100
.\run.ps1 -Seconds 15 -Width 2048 -Height 1024 -InjectBadBit 100
# GPU 検査の無効化（--verify のみ。デバッグ用）。
.\run.ps1 -Seconds 10 -Width 2048 -Height 1024 -NoGpuCheck
# M1b 障害注入フック（いずれも既定 0=無効）。
# exe が N 件目で完了未確認として終了 2（Done なし。import 資源は破棄しない）。
.\run.ps1 -Seconds 15 -Width 2048 -Height 1024 -InjectUnconfirmed 100
# consumer 遅延の模擬（drops が増えるが maxHeld≤2、ハンドル数は増加しない。終了 0）。
.\run.ps1 -Seconds 30 -Width 2048 -Height 1024 -DelayMs 50
# ACK 消失の模擬（N 個目の Done を main 側で捨てる。未 ACK は隔離され release されない。終了 2）。
.\run.ps1 -Seconds 15 -Width 2048 -Height 1024 -DropDoneAt 100
# M3 障害注入フック（いずれも既定 0=無効。通常経路の挙動は変えない）。
# N 回目の texture.release() を例外にする（解放失敗の模擬。隔離して新規 Offer を止め、非ゼロ終了）。
# N は定常到達後を狙う（約 175/s のため 1000 で約 6 秒後。小さすぎると起動過渡に当たる）。
.\run.ps1 -Seconds 15 -Width 2048 -Height 1024 -InjectReleaseFail 1000
# 起動から Ms 後に renderer を強制クラッシュさせる（ハングせず停止し、未 ACK を隔離、非ゼロ終了）。
.\run.ps1 -Seconds 15 -Width 2048 -Height 1024 -InjectRendererCrashMs 5000
# 起動から Ms 後に GPU プロセスを kill する（ハングしないことが必須。回復時は記録する）。
.\run.ps1 -Seconds 15 -Width 2048 -Height 1024 -InjectGpuKillMs 5000
# 既定経路の長時間計測（600 秒・4096x2048・90Hz・GPU 検査 on）。
.\run.ps1 -Seconds 600 -MemLog logs\vulkan600
# モード別の切り分け（各 120 秒・4096x2048・90Hz。プロセス再起動で 3 本）。
# <接頭辞>.exe.csv（exe 側の 1 秒周期ログ）と <接頭辞>.gpu.csv（GPU プロセスメモリ）を出す。
.\run.ps1 -Seconds 120 -Mode vulkan -MemLog logs\exp-vulkan
.\run.ps1 -Seconds 120 -Mode d3d-open-only -MemLog logs\exp-d3d
.\run.ps1 -Seconds 120 -Mode no-gpu -MemLog logs\exp-nogpu
```

`-Mode` を省略した場合は `vulkan` になる（既定経路。D3D11 デバイスを作らない）。
`d3d-open-only` は D3D11 Open 経路の診断専用であり、長時間動かすとメモリが増える
（実測約 +16MB/分）。`logs/exp-full.*` は旧既定経路（D3D11 照合あり）の過去ログであり、
現行の既定とは異なる。

手順の中身は `run.ps1` を見れば分かる（アドオンビルド→exe ビルド→配置試験→electron 起動）。
exe の単体ビルドは `native` で `cmake --build --preset windows-vs-release`。
終了後は `harness-stats.json`（台帳統計。`mode`・GPU プロセスの `gpuPidStart`/`gpuPid` を含む）と
`harness-run.log`（時系列）に証拠が残る。
exe 側の統計は標準出力（`run-out.txt` に混ざる）に出る。
`-MemLog <接頭辞>` を付けると `<接頭辞>.exe.csv`（exe 側の 1 秒周期ログ）と
`<接頭辞>.gpu.csv`（`Get-Counter` の GPU Process Memory。インスタンス名の `pid_XXXX` を
`harness-stats.json` の `gpuPid` と突き合わせる）が出る。

## 既知の制約

- パイプ名は electron 側 pid で作る（`\\.\pipe\xrift_m1_<electron-pid>`）。
  exe の pid を起動引数に使うことは Node の spawn ではできない
  （引数確定より pid 取得が後になる）ため、一意性という目的を electron pid で満たす。
- offer 済み未 ACK の借用は切断時に `release()` しない（DESIGN §6②）。
  隔離扱いで記録し、Electron 終了で回収する。`quarantined` が 0 でない終了は要確認。
- producer 完了保証は無い（DESIGN §8）。フレーム番号の連番とセル単色で破綻を検知するが、
  無事故実測を同期証明の代用にしない。
- exe の既定経路は Vulkan であり、D3D11 デバイスを作らない。
  D3D11 の `OpenSharedResource1` を毎フレーム呼ぶ経路は実測で持続不能
  （4096x2048・90Hz で約 +16MB/分）のため、既定から外した。
  `d3d-open-only` は診断専用であり、長時間動かしてはならない。
- 4096x2048 が重い場合は 2048x1024 に落としてよいが、その場合は理由と数値を報告すること。
  勝手に縮小して黙らない。
- 検証 readback は exe 側 `--verify` の範囲に隔離した部分的読み戻しであり、本番経路ではない。

## M3 マトリクス（滞留制限・借用管理・正常終了/障害時停止。検証用）

M3（DESIGN §5〜§6）の検証基盤。`fault-matrix.ps1` が各ケースを順に実行し、
期待（DESIGN §6 に基づく判定基準。スクリプト内の `$Cases` が正本）と実測を突き合わせ、
違反があれば非ゼロ終了する。製品経路の挙動は変えない（注入は既定 off）。

```powershell
# native/harness/electron から。1 ケース 15 秒・2048x1024、タイムアウト 60 秒。合計 5 分以内。
.\fault-matrix.ps1
# 短縮条件の指定（引数体系は run.ps1 と揃える）。
.\fault-matrix.ps1 -Seconds 15 -Width 2048 -Height 1024
# 指定ケースだけ実行する（デバッグ用）。
.\fault-matrix.ps1 -Only normal,delay
```

ケースと期待（判定基準。変えたら根拠を報告する）:

| ケース | 期待 |
|---|---|
| 正常 | harness 0 / exe 0、quarantine 0、offers=dones、maxHeld≤2 |
| exe kill | harness 非ゼロ、quarantine≥1、release されない、ハングなし |
| electron kill | exe が 10 秒以内に終了、ハングなし |
| drop-done | harness 非ゼロ、quarantine≥1、maxHeld≤2 |
| unconfirmed | exe 非ゼロ、Done を送っていない（offers−dones=1）、harness 非ゼロ、quarantine 1 |
| release-fail | harness 非ゼロ、quarantine≥1、二重 release なし |
| renderer-crash | ハングなし、非ゼロ、未 ACK があれば隔離 |
| gpu-kill | ハングなし（非ゼロが望ましいが、Chromium が回復する場合はその旨を記録して合否を分ける） |
| delay-ms=50 | harness 0 / exe 0、maxHeld≤2、drops 増、ハンドル数が増え続けない |

各ケース共通の観測項目: harness 終了コード、exe 終了コード、offers/dones、
quarantine、maxHeld、drops、ハング有無（タイムアウトで判定）、所要時間。
結果は表形式で出力し、`fault-matrix-results.md` と `fault-matrix-results.csv` に保存する。
ケース別の生証拠（`harness-stats.json`・`harness-run.log`・`run-out.txt`）は
`logs\fault-matrix\<ケース名>\` に退避する。

実装上の注意（DESIGN §5〜§6・§8）:

- timeout は返却許可ではない。期限切れで release しない
- 隔離した借用は release しない（参照保持）。二重 release をしない
- 注入は通常経路のコードを分岐で汚さない（`main.js` のテスト専用注入点
  `doReleaseTexture`・クラッシュ/ kill スケジューラの各 1 箇所に集約し、コメントで明示）
- 起動過渡の黒 paint は渡さない（`index.html` が最初のパターン描画後に
  `[m1] pattern-live` の合図を 1 回出し、`main.js` は合図まで転送前 drop する。
  Chromium OSR は最初の present より前に黒を paint することがあり、
  渡すと全面不一致になる。合図前の drop 数は `warmupDrops` に出す。M2 は対象外）
- `run.ps1` と引数体系を揃え、二重実装しない（マトリクスは `run.ps1` を呼ぶ。
  初回に 1 回だけ建てる `-SkipBuild` は時間合わせのためであり、通常は付けない）

## M2 ハーネス（pose/FOV と画像の対応・両眼提出。検証用）

M2（DESIGN §4・§7・§9・§11）の検証ハーネス。exe が XR フレームループで
描画レコードを発行し、`--xr-transfer` で RenderRequest を送る。レンダラ
（`index-m2.html`）は受け取ったレコードの値だけで左右横並びに描き、
main プロセスは paint に renderId を推測付与しない。

```powershell
# M2 の煙試験（10 秒・2048x1024。HMD が無くても exe の起動と pipe 接続まで進む）。
.\run.ps1 -Seconds 10 -Width 2048 -Height 1024 -M2# 旧 pose 混入の注入（不一致として棄却される想定）。
.\run.ps1 -Seconds 15 -Width 2048 -Height 1024 -M2 -InjectStaleRecord 100
# 左右分離 readback の無効化（デバッグ用）。
.\run.ps1 -Seconds 10 -Width 2048 -Height 1024 -M2 -NoSubmitVerify
# HMD ありの両眼提出（30〜60 秒。HMD 装着と SteamVR 起動が要る）。
.\run.ps1 -Seconds 60 -Width 2048 -Height 1024 -M2 -RenderScale 0.5
```

### M2 画像配置（正典。変えたら exe・レンダラ・検査を同時に更新する）

- アトラスは左右横並び（左半分=左眼、右半分=右眼）。半分の境界は来た画像の
  実寸 (`ownW/2`) で決める。奇数幅では左半分を floor に寄せる。
- ブロック A `(64,64)` 32x32: 使用レコードの renderId 下位 16bit。
  bit b のセルは `x=64+(b%8)*4`、`y=64+floor(b/8)*4` の 4x4（1=白/0=黒）。
- ブロック B `(128,64)` 32x32: pose タグ 16bit。同じセル配置。
  タグは量子化 pose + 世代 + renderId 全体の FNV-1a 下位 16bit
  （`src/transfer/render_record.h` の `PoseTag`。JS 側は `m2-codec-test.js` と
  `index-m2.html` が同じ式を持つ）。
  - 位置は ±4m を 8bit に、姿勢成分は ±1 を 8bit に量子化する。範囲外は端に寄せる。
  - 量子化の丸めは倍精度で行うこと。C++ 側も倍精度であり、単精度で計算すると
    .5 境界で 1 ずれる値がある（実測: 静止 pose のシミュレータで毎フレーム不一致）。
  - バイト列（30B）: renderId LE 8B、connEpoch LE 4B、spaceEpoch LE 4B、
    左 pos 3B、左 ori 4B、右 pos 3B、右 ori 4B。
  - タグは FOV を含まない。FOV の束縛は「同じレコードが提出時の pose/FOV を供給する」
    ことで保つ（renderId とタグの組がレコードを一意に指すため）。
- 左マーカー `(160,8)` 32x32 マゼンタ、右マーカー `(halfW+160,8)` 32x32 シアン。
  予約ブロックと重ねない。左右取り違えの検出用である。
- シーンは視差付き: 空グラデーション（水平線は pose 追従）、z=-2m の 3x3 色クアッド、
  z=-5m のグリッド。各眼の pose/FOV で投影するため、pose が動くと左右でずれる。
- ブロック A が 0（レコード未受信時の仮描画）は無効 ID であり、exe 側で棄却される。

### M2 の受理判定（exe 側）

復号した renderId 下位 16bit が保持レコードに存在し、pose タグの再計算が一致し、
spaceEpoch が一致し、発行から 500ms 以内であること。1 つでも外れたら棄却し、
理由別（不明 ID・pose 不一致・世代不一致・失効）に数える。
受理した画像の左右半分を各眼 swapchain へ `vkCmdBlitImage`（線形フィルタ）で
拡大コピーし、そのレコードの pose/FOV で `xrEndFrame` する。
最新 pose への付け替えはしない。受理時に exe 所有の hold イメージへ GPU 複製し、
新規受理が無いフレームでも同じ record の pose/FOV のまま出し直す（再提出）。
最初の 1 枚が来る前だけ `layerCount=0` で終える。再提出は record-ttl を超えても
行う（黒より古い絵が良い）。`--no-resubmit` で旧挙動（比較用）、
`--resubmit-max-age-ms=<ms>` で再提出の年齢上限（既定 0=無制限。診断用）。
統計は `層あり`（新規受理）と `再提出` を分けて数える。
blit 後に各眼の提出画像から 1 画素だけ読み、左眼=マゼンタ・右眼=シアンを確認する
（`--no-submit-verify` で無効化可）。画像全体は読まない。

### M2 の既知の制約

- producer 同期は未解決のまま（DESIGN §8）。ID 照合は読めた制御メタの対応であり、
  全画素の書き込み完了を保証しない。解決したことにしない。
- 色は未決（DESIGN §9・§13）。swapchain は `B8G8R8A8_UNORM` を優先して受理画像と
  一致させ、blit は複写・拡大のみ行う。sRGB 形式しか無い場合は警告を残す。
- `--render-scale` の既定は 0.5（VRAM 圧迫の回避）。要求寸法と実寸が食い違ったら
  実寸を正とする（DESIGN §9）。
- 中間 Offer は最新優先で Done だけ返し、照合は最新 1 件に行う（破棄数は統計に出る）。
- 終了の合図は無い。harness が Offer を止めても読取側を開いたままにするため、
  exe は 5 秒の途絶で producer 終了とみなして統計を出して終了する（DESIGN §6③）。
  切断（BROKEN_PIPE）時は新規受付を止め、現在のフレームを安全に終えて終了する。
  未完了の借用へ Done は送らない。

### null ドライバでの検証手順（HMD 無しで両眼提出まで回す。将来の自動検証用）

- 目的: HMD 無しで XR セッションを VISIBLE/FOCUSED まで進め、提出レート・blit・
  左右分離検証を測る。実機（PICO 等）の置き換えではなく、提出経路の回帰用である。
- SteamVR 設定の変更は必ず復元すること。手順はバックアップ→変更→再起動→測定→
  復元→再起動→ハッシュ一致確認までを 1 セットにする。
- 設定ファイルは `%LOCALAPPDATA%\openvr\steamvr.vrsettings`（SteamVR の
  `steamvr.vrsettings`。パスが無い場合は SteamVR の設定画面から開く）。
  変更前に全文を別名で保存し、ハッシュ（`Get-FileHash` の SHA256）を記録する。
- 追加する 3 点（JSON の対応する節に足す。既存の値は消さない）。
  - `"driver_null": {"enable": true}`
  - `"steamvr": {"forcedDriver": "null"}`（既存の `steamvr` 節があれば中に足す）
  - `"power": {"pauseCompositorOnStandby": false}`（既存の `power` 節があれば中に足す）
- 変更後に SteamVR を再起動する（SteamVR のメニューから再起動するか、
  `vrserver.exe`/`vrmonitor.exe` の終了と SteamVR の起動で代用する）。
  再起動せずに測ると旧ドライバのままになり、FOCUSED に届かない。
- 測定（例: 60 秒・4096x2048 相当。`Width`/`Height` は OSR 側であり要求寸法ではない）。
  - `.\run.ps1 -Seconds 60 -M2 -RenderScale 0.5`
  - `--render-scale 1.0` は VRAM・負荷が大きい。失敗したら 0.5 のみでよいが、
    数値と理由（VRAM 不足・タイムアウト等）を報告すること。勝手に縮小して黙らない。
- 測定後は設定ファイルをバックアップから復元し、SteamVR を再起動して PICO 等の
  実機に戻ることを確認する。復元後にハッシュがバックアップと一致することを確かめる。
- 注意点。
  - null ドライバでは pose が静止しがちで、視差シーンの動きは小さい。
    左右分離（マゼンタ/シアン）と pose 対応の有無は見られるが、追従遅延の評価にはならない。
  - 色の最終確認は未決のまま（sRGB のみの場合あり）。左右分離と pose 対応を優先する。
  - 終了時に harness が先に終わっても exe は統計を出して終了するはずである。
    exe の stdout/stderr は harness のログ（`harness-run.log`・`run-out.txt`）へ
    `exe:` 付きで回し、終了は stdio の EOF（`close`）で最大 3 秒待つ。
    統計行（`XR転送統計`）が欠けたら切断検知の退行として扱う。

## M4 ハーネス（入力・haptics・参照空間・再接続。検証用）

M4（DESIGN §10・§11）の検証ハーネス。`-M2` に足して使う。HMD 非装着では
セッションは SYNCHRONIZED 止まり・入力は inactive のままである
（構造確認まで。実機の操作・体感・追従は未検証）。

```powershell
# M4 の煙試験（10 秒・2048x1024。HMD 非装着で可）。
.\run.ps1 -Seconds 10 -Width 2048 -Height 1024 -M2 -Space stage `
  -HapticAtMs 5000 -InjectSessionRestartMs 8000
# haptics の複数送信（カンマ区切りの複数時刻。左手・振幅 0.8・300ms）。
.\run.ps1 -Seconds 30 -Width 2048 -Height 1024 -M2 -HapticAtMs 5000,15000,25000
# haptics の繰り返し送信（5 秒間隔で 3 回。左右交互・振幅 1.0・500ms）。
.\run.ps1 -Seconds 25 -Width 2048 -Height 1024 -M2 -HapticRepeat 3
# 模擬の space 変更（起動 8 秒後に 1 回適用。実イベントではない）。
.\run.ps1 -Seconds 15 -Width 2048 -Height 1024 -M2 -InjectSpaceChangeMs 8000
```

- `-Space <auto|local|stage|local-floor>`（既定 `auto`）… exe の `--space` へ渡す。
  起動ログの「参照空間の一覧」と「参照空間: …（要求=…）」で選択を確認する。
- `-HapticAtMs <Ms[,Ms...]>`（既定 `'0'`=無効）… 起動から Ms 後に HapticCommand を送る
  （各送信は左手・振幅 0.8・300ms）。カンマ区切りで複数時刻を指定できる。
  exe は `xrApplyHapticFeedback` を呼び、結果を `XrResult` でログに残す
  （`haptics 送信: …（XrResult=…）`。非対応・inactive のときは未送信として
  統計に残す。例外でループを止めない）。
  非装着時は inactive として記録される（体感の確認は実機装着時に行う）。
- `-HapticRepeat <N>`（既定 `0`=無効）… 5 秒間隔で N 回送る
  （左右交互・振幅 1.0・500ms）。実機の体感再確認用。
- `-InjectSessionRestartMs <Ms>`（既定 0=無効）… exe の `--inject-session-restart` へ渡す。
  指定時刻に意図的にセッション束を破棄・再生成する。`connEpoch` が進み、
  再生成の成功/試行が exe の `M4統計` に残る。harness 側の `[M4入力]` ログの
  `conn=` も切り替わる。
- `-InjectSpaceChangeMs <Ms>`（既定 0=無効）… exe の `--inject-space-change` へ渡す。
  指定時刻に模擬の space 変更を 1 回適用する。ランタイムの
  `REFERENCE_SPACE_CHANGE_PENDING` と同じ適用経路
  （空間作り直し・`spaceEpoch` 更新・旧レコード失効）を通すが、
  実イベントではないことを exe 側のログに明記する
  （`模擬 space 変更を適用（実イベントではない…）`）。
  統計の `space変更` に計上する。
- 入力スナップショットの表示: exe 側の `[M4入力]`（毎秒。その 1 秒間に押された
  ボタンの集合 `latch=` 付き）と harness 側の `[M4入力]`
  （毎秒。`harness-run.log` に残る。`latch=` 付き）。短い押下は 1 秒サマリの
  `latch=` と線路上の InputState 予約域（左右各 1B、buttons と同じ bit 割付け）で
  取りこぼさない。状態が変化したフレームでは exe 側に `[M4エッジ]` が 1 行出る
  （ボタン bit の press/release、trigger/squeeze の off->on/on->off、
  stick の idle->touch/touch->idle、pose 有効/無効の変化）。
  `harness-stats.json` の `inputStates` と `lastInput` に受信件数と最終要約が残る。

M4 の既知の制約:

- 実機のコントローラ操作・haptics の体感・追従は未検証。
  実機装着時の確認項目は `native/README.md` のチェックリストを参照。
  再計測の手順は下の「実機での再計測手順」を参照。
- PICO 系（bytedance）の suggest は SteamVR が `XR_ERROR_PATH_UNSUPPORTED` で拒否する。
  仕様には存在するため suggest 表に含め、失敗は記録して他を続行する。
  実機装着時に選ばれる profile は別途記録する。
- すべての参照空間（local-floor/stage/local）が利用可の環境では、
  明示指定の後退経路は通らない（後退の確認は利用不可の環境で行う）。
- producer 同期の未解決（DESIGN §8）、色（DESIGN §9）は触らない。

### 実機での再計測手順（M4 の取りこぼし・haptics 判定不能の再確認用）

前回の実機計測では primary（A/X）と menu がログに出ず（1 秒周期スナップショットの
取りこぼし・システム消費・バインディング誤りのいずれか未確定）、haptics は
`xrApplyHapticFeedback` 成功でも体感が判定不能だった。
以下の手順で 1 項目ずつ切り分ける。HMD は使わない検証（`--m2-selftest`・
`protocol-test`・`fault-matrix`）を先に通してから行う。

```powershell
# 60 秒の計測枠。haptics は 5・25・45 秒の 3 回、模擬 space 変更は使わない（実操作で確認）。
.\run.ps1 -Seconds 60 -Width 2048 -Height 1024 -M2 -HapticAtMs 5000,25000,45000
```

1. ボタンは 1 つずつ 1 秒押して 2 秒空ける（左右 trigger・squeeze・stick・
   stick click・primary（A/X）・secondary（Y/B）・menu の順）。
   1 秒周期の `[M4入力]` では `latch=` に載ること、押下の瞬間には `[M4エッジ]` に
   `L.primary press` のような行が出ることを `run-out.txt` で確認する。
   `latch=` にも `[M4エッジ]` にも出ないボタンはバインディング誤り
   （suggest 表・`SupportMask` を疑う）。出るがサマリに無いものは短押しの
   取りこぼしであり、押し方（1 秒保持）で再現するかを見る。
   menu はシステムに消費される場合があり、出なくても直ちに誤りとしない。
   その場合は「menu はシステム消費の疑い」として記録する。
2. haptics は上記の 3 回送信で左右の振動の有無を答える（左・左・左のため、
   右の確認は別途 `-HapticRepeat 4` で左右交互に送る）。
   exe 側の `haptics 送信: …（XrResult=…）` と体感の組で記録する。
   成功（`XrResult=XR_SUCCESS`）でも振動不明なら振幅・持続・周波数の問題であり、
   バインディングの問題ではない。
3. 脱着→再装着は `[M4エッジ]` の `L.connected yes->no` / `no->yes` と
   `[M4入力]` の `未接続` / `接続` の往復で確認する。exe が終了せず、
   `STOPPING` のまま束を保つこと（再生成が走らないこと）。
4. リセンタリングの代替手段: SteamVR メニューが出せない場合は
   `-InjectSpaceChangeMs`（模擬。実イベントではない）で適用経路だけを確かめ、
   実イベントの確認はダッシュボード等でリセンターできる環境に持ち越す。
   模擬では `模擬 space 変更を適用（実イベントではない…）` と `spaceEpoch` の
   進行・旧レコードの失効（`M2 棄却: space世代不一致` が出ないこと＝新世代で
   照合すること）を確認する。
5. 終了統計（`M4統計`）の `space変更`・`haptic受信`・`InputState送信` を記録し、
   `harness-stats.json` の `hapticSent`・`inputStates`・`lastInput` と突き合わせる。

### HMD 操作ガイド（実機計測用。検証器具）

HMD 装着中は PC 画面が見えないため、`index-m2.html` の WebGPU キャンバスの上に
HTML/CSS のオーバーレイで操作手順を出す。Chromium 合成で OSR 共有テクスチャに
乗るだけであり、canvas 描画・パターン画素・検査結果は変わらない。

```powershell
# 実機計測（ガイドは -M2 時に既定 on。付け外しは -NoGuide/-Guide で明示）。
.\run.ps1 -Seconds 80 -Width 2048 -Height 1024 -M2 -HapticAtMs 20000,30000,40000
# ガイド表示の事前確認（8 秒時点の見た目を PNG に保存。検証専用で描画経路に入れない）。
.\run.ps1 -Seconds 20 -Width 2048 -Height 1024 -M2 -CaptureAt '8000,logs\guide-cap.png'
```

- `-Guide` / `-NoGuide`（`-M2` 時は既定 on、それ以外は既定 off）…
  electron の `--guide[=1|0]` へ渡す。off のままの既存検証（`protocol-test`・
  `m2-render-test`・`fault-matrix`・M1 経路）に挙動の影響は無い。
- ガイドは左右眼の中央（下部。予約の ID/タグブロック・マーカーと重ならない）に
  同じ内容を出す。大きく高コントラスト（半透明の黒帯・白文字）で、現在の操作・
  残り時間・次の操作・進捗ドットを示す。手順は 4 秒刻みを基本に、
  20/30/40 秒の振動・55 秒の脱着・60 秒の再装着・70 秒の模擬 space 変更を含む。
- `-CaptureAt 'Ms,path[;Ms,path]'`（既定 `''`=無効）… electron の
  `--capture-at=Ms,path` へ渡す。起動から Ms 後に `webContents.capturePage()` で
  PNG を保存する。検証専用であり、paint/Offer/検査の経路には入れない。

### 2026-10-20 の実機確認結果

PICO 4S（PICO Connect ストリーミング）→ SteamVR/OpenXR 2.17.10、RTX 3090 での実測。
既存の M4 チェックリストは置き換えない。事実のみを残す。

| 項目 | 結果 |
|---|---|
| interaction profile | `oculus/touch_controller`。bytedance 系 4 profile は `XR_ERROR_PATH_UNSUPPORTED` で未対応 |
| 入力（エッジ記録） | 左右 trigger（0.07〜1.00）、左右 squeeze（0.19〜1.00）、左右 thumbstick（±1.0 まで）、stick click、primary（A/X）・secondary（B/Y）の押下・解放を検出 |
| menu | ユーザーは押したがアプリに届かず。システム予約と判断 |
| haptics | 2 回送信・2 回成功、ユーザーが両方とも体感 |
| 参照空間 | `auto` → `local-floor`。`--space=stage` / `local` も選択可 |
| リセンター | SteamVR メニューに到達できず実イベント未取得。`--inject-space-change` の模擬で `spaceEpoch 1→2` と旧レコード失効を確認 |
| 脱着 | 外してもセッションは FOCUSED のまま（PICO ストリーミングでは検出されないと判断）。ストリーム終了時は `STOPPING → IDLE → EXITING` でアプリ終了 |
| 提出 | blit 3,098〜4,886 回/ラン、左右分離検証 ng=0、XR fps 87〜89、`xrEndFrame` 層あり |
| 緑一色の事象 | SteamVR コンポジタの不調（null ドライバ実験後に発生）。SteamVR 再起動で復旧。`--dump-submit` で両眼 swapchain をダンプし、正しいシーン・ガイド・マーカー入りを確認済み |
| null ドライバ | SteamVR 2.17 では `xrGetVulkanGraphicsDeviceKHR` が `XR_ERROR_RUNTIME_FAILURE`（`GetOutputDevice: failed to find VkPhysicalDevice matching adapter UUID`）となり、Vulkan 経路の検証には使えない。今後は実機前提 |
| 未確認 | 実リセンター、実セッション loss/再接続（`--inject-session-restart` の模擬のみ）、menu の代替経路 |
