#pragma once

#include <cstddef>
#include <cstdint>

// M1 転送プロトコル（Electron main ⇔ xrift_new.exe）の固定長バイナリ形式。
// main がサーバ、exe がクライアント。1 メッセージ 64 バイト、リトルエンディアン。
// Node 側（harness/electron/main.js）は同じオフセットで Buffer を読み書きする。
// レイアウトを変えたら両側と static_assert を同時に更新する。

namespace xrift::transfer {

// "XRFT" をリトルエンディアン u32 で読んだ値。
constexpr uint32_t kMagic = 0x54465258u;
constexpr uint16_t kVersion = 1;

constexpr uint16_t kTypeOffer = 1;  // main → exe。借用中の複製ハンドルを渡す。
constexpr uint16_t kTypeDone = 2;   // exe → main。GPU コピー完了（または失敗）を返す。

constexpr uint32_t kFormatBgra8Unorm = 0;  // OSR pixelFormat "bgra" に対応。

constexpr uint32_t kStatusOk = 0;           // コピー成功（検査付きの場合は検査も成功）。
constexpr uint32_t kStatusCopyFailed = 1;   // import またはコピーに失敗した。
// コピーは成功したが画素検査に失敗した。--verify の部分検証と --gpu-check の
// 全画面 GPU 検査のどちらかの不一致である（内訳は exe 側の統計に出る）。
constexpr uint32_t kStatusVerifyFailed = 2;

// GPU 完了を確認できない失敗（フェンス待ち失敗・device lost・waitIdle 失敗）の
// 内部合図。線路上には出さない。timeout は返却許可ではないため、Done を送らず
// main は該当借用を release しない（DESIGN §5〜§6 の quarantined 経路）。
// 値は status 域と衝突しない番兵である。
constexpr uint32_t kStatusNoAck = 0xFFFFFFFFu;

#pragma pack(push, 1)
// オフセット: magic 0, version 4, type 6, borrowId 8, width 16, height 20,
// format 24, handle 28, status 36, reserved 40。合計 64 バイト。
struct TransferMessage {
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t type = 0;
  uint64_t borrowId = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  uint64_t handle = 0;
  uint32_t status = 0;
  uint8_t reserved[24] = {};
};
#pragma pack(pop)

static_assert(sizeof(TransferMessage) == 64, "転送メッセージは 64 バイト固定であること");
static_assert(offsetof(TransferMessage, borrowId) == 8, "borrowId の位置は Node 側と一致すること");
static_assert(offsetof(TransferMessage, handle) == 28, "handle の位置は Node 側と一致すること");
static_assert(offsetof(TransferMessage, status) == 36, "status の位置は Node 側と一致すること");
static_assert(offsetof(TransferMessage, reserved) == 40, "予約領域の位置は Node 側と一致すること");

// Offer として受信したメッセージの最低限の正当性を確認する。
// 借用の実体（ハンドルの有効性）は呼び出し側が D3D11/Vulkan で確認する。
inline bool IsValidOffer(const TransferMessage& m) {
  return m.magic == kMagic && m.version == kVersion && m.type == kTypeOffer && m.width > 0 &&
         m.height > 0 && m.format == kFormatBgra8Unorm && m.handle != 0;
}

inline TransferMessage MakeDone(uint64_t borrowId, uint32_t status) {
  TransferMessage m{};
  m.magic = kMagic;
  m.version = kVersion;
  m.type = kTypeDone;
  m.borrowId = borrowId;
  m.status = status;
  return m;
}

}  // namespace xrift::transfer
