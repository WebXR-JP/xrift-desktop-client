#pragma once

#include <cstddef>
#include <cstdint>

// M2 転送プロトコル（Electron main ⇔ xrift_new.exe）の固定長バイナリ形式。
// main がサーバ、exe がクライアント。1 メッセージ 192 バイト、リトルエンディアン。
// M1 の 64B 配置は先頭 64B として保存する（Offer/Done の既存オフセットは不変）。
// 64〜191B に M2 の RenderRequest 領域を足す。Node 側（harness/electron/main.js と
// protocol-test.js）は同じオフセットで Buffer を読み書きする。
// レイアウトを変えたら両側と static_assert を同時に更新する。

namespace xrift::transfer {

// "XRFT" をリトルエンディアン u32 で読んだ値。
constexpr uint32_t kMagic = 0x54465258u;
// M2 で 192B 化したため版を上げる。M1 の 64B 送信側とは枠が合わないため混用しない。
constexpr uint16_t kVersion = 2;

constexpr uint16_t kTypeOffer = 1;          // main → exe。借用中の複製ハンドルを渡す。
constexpr uint16_t kTypeDone = 2;           // exe → main。GPU コピー完了（または失敗）を返す。
constexpr uint16_t kTypeRenderRequest = 3;  // exe → main。描画要求（pose/FOV と要求を供給）。

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

constexpr size_t kMessageSize = 192;

// 左右の pose（position xyz + orientation xyzw）と FOV（left/right/up/down）。
// 単位は XR そのまま（位置=m、角度=rad）。所有権の意味は持たない値束である。
struct EyePose {
  float position[3] = {};
  float orientation[4] = {};
};

struct EyeFov {
  float left = 0.0f;
  float right = 0.0f;
  float up = 0.0f;
  float down = 0.0f;
};

#pragma pack(push, 1)
// オフセット: magic 0, version 4, type 6, borrowId 8, width 16, height 20,
// format 24, handle 28, status 36, renderId 40, connEpoch 48, spaceEpoch 52,
// leftPos 56, leftOri 68, rightPos 84, rightOri 96, leftFov 112, rightFov 128,
// reqWidth 144, reqHeight 148, reserved 152。合計 192 バイト。
// Offer/Done では 40 以降を 0 で送り、受信側は読まない（M1 経路の挙動は変えない）。
// RenderRequest（exe→main）では borrowId/handle/status を 0 にし、renderId 以降を使う。
// width/height は RenderRequest では要求アトラス寸法（横並び全体）を入れる。
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
  uint64_t renderId = 0;
  uint32_t connEpoch = 0;
  uint32_t spaceEpoch = 0;
  float leftPos[3] = {};
  float leftOri[4] = {};
  float rightPos[3] = {};
  float rightOri[4] = {};
  float leftFov[4] = {};
  float rightFov[4] = {};
  uint32_t reqWidth = 0;
  uint32_t reqHeight = 0;
  uint8_t reserved[40] = {};
};
#pragma pack(pop)

static_assert(sizeof(TransferMessage) == 192, "転送メッセージは 192 バイト固定であること");
static_assert(sizeof(TransferMessage) == kMessageSize, "kMessageSize と実体は一致すること");
static_assert(offsetof(TransferMessage, borrowId) == 8, "borrowId の位置は Node 側と一致すること");
static_assert(offsetof(TransferMessage, handle) == 28, "handle の位置は Node 側と一致すること");
static_assert(offsetof(TransferMessage, status) == 36, "status の位置は Node 側と一致すること");
static_assert(offsetof(TransferMessage, renderId) == 40, "renderId の位置は Node 側と一致すること");
static_assert(offsetof(TransferMessage, spaceEpoch) == 52, "spaceEpoch の位置は Node 側と一致すること");
static_assert(offsetof(TransferMessage, leftPos) == 56, "leftPos の位置は Node 側と一致すること");
static_assert(offsetof(TransferMessage, leftFov) == 112, "leftFov の位置は Node 側と一致すること");
static_assert(offsetof(TransferMessage, rightFov) == 128, "rightFov の位置は Node 側と一致すること");
static_assert(offsetof(TransferMessage, reqWidth) == 144, "reqWidth の位置は Node 側と一致すること");
static_assert(offsetof(TransferMessage, reserved) == 152, "予約領域の位置は Node 側と一致すること");

// Offer として受信したメッセージの最低限の正当性を確認する。
// 借用の実体（ハンドルの有効性）は呼び出し側が D3D11/Vulkan で確認する。
inline bool IsValidOffer(const TransferMessage& m) {
  return m.magic == kMagic && m.version == kVersion && m.type == kTypeOffer && m.width > 0 &&
         m.height > 0 && m.format == kFormatBgra8Unorm && m.handle != 0;
}

// RenderRequest として受信したメッセージの最低限の正当性を確認する。
// pose/FOV の値域までは見ない（実行時 pose の不定値を黙って捨てない。受理判定は
// レコード照合で行う）。寸法 0 は要求として無意味なため落とす。
inline bool IsValidRenderRequest(const TransferMessage& m) {
  return m.magic == kMagic && m.version == kVersion && m.type == kTypeRenderRequest &&
         m.renderId != 0 && m.width > 0 && m.height > 0;
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

inline TransferMessage MakeRenderRequest(uint64_t renderId, uint32_t connEpoch,
                                         uint32_t spaceEpoch, const EyePose& left,
                                         const EyePose& right, const EyeFov& leftFov,
                                         const EyeFov& rightFov, uint32_t width,
                                         uint32_t height) {
  TransferMessage m{};
  m.magic = kMagic;
  m.version = kVersion;
  m.type = kTypeRenderRequest;
  m.width = width;
  m.height = height;
  m.renderId = renderId;
  m.connEpoch = connEpoch;
  m.spaceEpoch = spaceEpoch;
  for (int i = 0; i < 3; ++i) {
    m.leftPos[i] = left.position[i];
    m.rightPos[i] = right.position[i];
  }
  for (int i = 0; i < 4; ++i) {
    m.leftOri[i] = left.orientation[i];
    m.rightOri[i] = right.orientation[i];
  }
  m.leftFov[0] = leftFov.left;
  m.leftFov[1] = leftFov.right;
  m.leftFov[2] = leftFov.up;
  m.leftFov[3] = leftFov.down;
  m.rightFov[0] = rightFov.left;
  m.rightFov[1] = rightFov.right;
  m.rightFov[2] = rightFov.up;
  m.rightFov[3] = rightFov.down;
  m.reqWidth = width;
  m.reqHeight = height;
  return m;
}

}  // namespace xrift::transfer
