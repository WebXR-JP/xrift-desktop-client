#pragma once

// 転送の HMD 無し自己検証。render_record.h の値層と protocol.h の配置を
// XR セッション無しに検証する。成功時は 0 を返す。

namespace xrift::transfer {

int RunTransferSelfTest();

}  // namespace xrift::transfer
