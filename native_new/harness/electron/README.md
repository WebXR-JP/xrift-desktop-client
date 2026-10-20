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
# native_new/harness/electron から。既定 60 秒・4096x2048。
.\run.ps1
# 短時間の煙試験（10 秒・2048x1024）。
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
exe の単体ビルドは `native_new` で `cmake --build --preset windows-vs-release`。
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
