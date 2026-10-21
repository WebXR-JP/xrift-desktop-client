# transfer-addon（転送用 N-API アドオン）

Electron main が OSR `paint` で受けた共有テクスチャの NT ハンドルを、
別 exe（`xrift_native.exe --xr-transfer`）のプロセスへ複製する薄い層。
`DuplicateHandle` の呼び出しだけを担い、GPU 操作はしない。

## 公開関数

- `duplicateHandle(ntHandleBuffer, targetPid)` → `{ok:true, handle: BigInt}`。
  `handle` は複製先プロセスで有効なハンドル値。
- 失敗時は例外ではなく `{ok:false, error, hresult}` を返す。
  `hresult` は `OpenProcess` / `DuplicateHandle` の失敗時のみ付く。

## 所有権の注意

- 複製元のハンドルは閉じない（Chromium の pool が所有する）。
- 複製先プロセスのハンドル（`OpenProcess` の戻り値）は呼び出し内で閉じる。
- 返した複製ハンドルは exe 側が import 後に閉じる。

## ビルド（再現コマンド）

```powershell
# native/transfer-addon から
.\build.ps1
```

実体は `XRift-Client/node_modules/cmake-js` を `--runtime electron --runtime-version 44.9.0`
で呼ぶだけである。本体（`native/CMakeLists.txt`）には混ぜない。

## 制約

- Windows のみ。Linux では FD 転送に差し替える（時期未定）。
- Electron 44.9.0 用にビルドする。ランタイムが変わったら `--runtime-version` を更新する。
