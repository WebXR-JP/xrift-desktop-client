#pragma once

// --probe の実体。instance と Vulkan 初期化だけ行い環境情報を出す。成功時 0。
// セッションは作らない。

namespace xrift::app {

int Probe();

}  // namespace xrift::app
