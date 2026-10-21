# サードパーティのライセンス

このリポジトリと配布版には、次のサードパーティの成果物が含まれる。それぞれのライセンスに従うこと。
**配布時にはライセンス全文の同梱が必要なものがある**（下の「配布時の注意」）。

## 実行時に使うもの（配布物に同梱する）

| 成果物 | 内容 | ライセンス | 上流 |
|---|---|---|---|
| OpenXR-SDK-Source `release-1.1.65` | OpenXR loader（FetchContent で取得。`native/cmake/Dependencies.cmake`） | Apache License 2.0 | https://github.com/KhronosGroup/OpenXR-SDK-Source |
| Vulkan-Headers `v1.4.363` | `vulkan.hpp`＋`vk::raii`（ヘッダのみ。リンクはしない） | Apache License 2.0 | https://github.com/KhronosGroup/Vulkan-Headers |
| Electron 44.9.0 | 実行環境（Chromium 同梱） | MIT（本体。同梱の Chromium 等は別ライセンス） | https://github.com/electron/electron |
| three 0.186.1 | 検証ページ用（配布物には `three.module.js`・`three.core.js` の 2 ファイルだけ同梱） | MIT | https://github.com/mrdoob/three.js |

## ビルド時のみ使うもの（配布物に同梱しない）

| 成果物 | 内容 | ライセンス | 上流 |
|---|---|---|---|
| TypeScript 7.0.2 | client のビルド | Apache License 2.0 | https://github.com/microsoft/TypeScript |
| @types/node 24.x | 型定義 | MIT | https://github.com/DefinitelyTyped/DefinitelyTyped |
| @electron/packager 20.3.0 | 配布物作成 | BSD 2-Clause | https://github.com/electron/packager |
| cmake-js 8.0.0 | transfer-addon のビルド | MIT | https://github.com/cmake-js/cmake-js |

## 配布時の注意

- Apache License 2.0 の成果物（OpenXR loader、Vulkan-Headers 由来のコード、TypeScript 由来のコードが含まれる場合）は、配布時にライセンス全文と変更表示の同梱が必要である。
- Electron 配布物に含まれる Chromium 等のライセンスは、配布物の `LICENSES.chromium.html` に従う。
- three の 2 ファイルを同梱する場合は MIT 表示（著作権表示とライセンス文）を保持すること。
