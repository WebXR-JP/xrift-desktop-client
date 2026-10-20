# M1 ハーネス（検証用。 本番 TS ではない）

M1（別 exe・XR 指定 GPU への import、コピー、返却）の検証ハーネス。
Electron 側は借用台帳と `release()` 専任に徹し、提出には関与しない。
本番 TypeScript の作り直しは M5 で行う。ここにある `main.js` は使い捨ての検証器具である。

## 構成と責務

- `main.js` … パイプサーバ + 借用台帳 + `release()` 専任。exe を起動し、
  paint ごとに NT ハンドルを複製して Offer を送り、Done でその借用だけ返す。
  同時保持は処理中 1 枚 + 保留 1 枚まで。超過した古い保留は即 `release()` する。
- `index.html` … WebGPU の動的パターン（4096x2048 固定、4 隅固定色、フレーム番号埋め込み）。
- `protocol-test.js` … プロトコル配置の試験（素の node で実行可）。
- `addon-test.js` … アドオン異常系の試験（electron で実行）。
- `run.ps1` … 再現手順の実行器。

## 再現手順

```powershell
# native_new/harness/electron から。既定 60 秒・4096x2048。
.\run.ps1
# 短時間の煙試験（10 秒・2048x1024）。
.\run.ps1 -Seconds 10 -Width 2048 -Height 1024
```

手順の中身は `run.ps1` を見れば分かる（アドオンビルド→exe ビルド→配置試験→electron 起動）。
exe の単体ビルドは `native_new` で `cmake --build --preset windows-vs-release`。
終了後は `harness-stats.json`（台帳統計）と `harness-run.log`（時系列）に証拠が残る。
exe 側の統計は標準出力（`run-out.txt` に混ざる）に出る。

## 既知の制約

- パイプ名は electron 側 pid で作る（`\\.\pipe\xrift_m1_<electron-pid>`）。
  exe の pid を起動引数に使うことは Node の spawn ではできない
  （引数確定より pid 取得が後になる）ため、一意性という目的を electron pid で満たす。
- offer 済み未 ACK の借用は切断時に `release()` しない（DESIGN §6②）。
  隔離扱いで記録し、Electron 終了で回収する。`quarantined` が 0 でない終了は要確認。
- producer 完了保証は無い（DESIGN §8）。フレーム番号の連番とセル単色で破綻を検知するが、
  無事故実測を同期証明の代用にしない。
- 4096x2048 が重い場合は 2048x1024 に落としてよいが、その場合は理由と数値を報告すること。
  勝手に縮小して黙らない。
- 検証 readback は exe 側 `--verify` の範囲に隔離した部分的読み戻しであり、本番経路ではない。
