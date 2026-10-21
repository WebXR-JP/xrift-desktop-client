# Native（`xrift_native.exe`）

## 前提

- OpenXR のみ（D3D11 バインディングは使わない）
- Vulkan（`vulkan-1.lib` にはリンクしない。動的ディスパッチャが `vulkan-1.dll` を読む）
- CPU コピーを経由せず、Electron の OSR 共有テクスチャから GPU 内コピーだけでフレームを受け取る
- Linux 対応は将来対応（preset だけ用意。Windows 固有 API を共通境界へ漏らさない）

## ここで何をするか

- `xrift_native.exe`（OpenXR＋Vulkan・転送・入力・提出）の保守と拡張
- `transfer-addon/`（NT ハンドル複製の N-API アドオン）の保守
- 設計は [`../docs/architecture.md`](../docs/architecture.md)、手順は [`../docs/development.md`](../docs/development.md)、検証は [`../docs/verification.md`](../docs/verification.md)

## 制約

- ビルドは MSVC＋CMake preset（`windows-vs`）。`vcvars64.bat` 経由で環境を入れる
- 依存は FetchContent で固定（OpenXR-SDK-Source `release-1.1.65`、Vulkan-Headers `v1.4.363`）。版を変えるときは `cmake/Dependencies.cmake` で変える
- 撤去済みの実験フラグ（旧転送・ペース・注入系等）を復活させない。CLI は `src/main.cpp` が正本
- `--xr-transfer` は同時に 1 本だけ起動する（2 本目以降は XR 初期化に失敗して終了 1 になる）
- 借用の所有権は Electron/Chromium にあり、`release()` は main プロセスだけが呼ぶ。exe 側で返却しない
- 終了時は統計と明示 teardown の後に `_Exit` で終わる（quick exit）。残りの RAII 破棄を足さない

# 特記事項

- OpenXR-Hpp は更新が停止しているため使用しない
