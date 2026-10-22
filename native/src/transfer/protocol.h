#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// 転送プロトコル（Electron main ⇔ xrift_native.exe）の固定長バイナリ形式。
// main がサーバ、exe がクライアント。1 メッセージ 256 バイト、リトルエンディアン。
// 旧 64B 配置は先頭 64B として保存する（Offer/Done の既存オフセットは不変）。
// 64〜191B に RenderRequest 領域を足し、192〜255B に拡張した。
// InputState のコントローラ pose は 88〜143B（InputState では未使用の
// RenderRequest 欄の再利用。種別で区別する）と 160〜215B に載る（下の kInput* 配置）。
// Node 側（client/src/protocol.ts と tools/test/client/protocol.test.cjs）は
// 同じオフセットで Buffer を読み書きする。
// レイアウトを変えたら両側と static_assert を同時に更新する。

namespace xrift::transfer {

// "XRFT" をリトルエンディアン u32 で読んだ値。
constexpr uint32_t kMagic = 0x54465258u;
// 192B 化したため版を上げる。旧 64B 送信側とは枠が合わないため混用しない。
// 256B 化したため更に版を上げる。192B の相手とは枠が合わないため混用しない。
constexpr uint16_t kVersion = 3;

constexpr uint16_t kTypeOffer = 1;          // main → exe。借用中の複製ハンドルを渡す。
constexpr uint16_t kTypeDone = 2;           // exe → main。GPU コピー完了（または失敗）を返す。
constexpr uint16_t kTypeRenderRequest = 3;  // exe → main。描画要求（pose/FOV と要求を供給）。
constexpr uint16_t kTypeHapticCommand = 4;  // main → exe。haptics 指令。
constexpr uint16_t kTypeInputState = 5;     // exe → main。入力スナップショット。

constexpr uint32_t kFormatBgra8Unorm = 0;  // OSR pixelFormat "bgra" に対応。

constexpr uint32_t kStatusOk = 0;           // コピー成功（検査付きの場合は検査も成功）。
constexpr uint32_t kStatusCopyFailed = 1;   // import またはコピーに失敗した。
// コピーは成功したが画素検査に失敗した。--verify の部分検証と --gpu-check の
// 全画面 GPU 検査のどちらかの不一致である（内訳は exe 側の統計に出る）。
constexpr uint32_t kStatusVerifyFailed = 2;

// GPU 完了を確認できない失敗（フェンス待ち失敗・device lost・waitIdle 失敗）の
// 内部合図。線路上には出さない。timeout は返却許可ではないため、Done を送らず
// main は該当借用を release しない（docs/architecture.md「借用の所有権と release 遅延」の quarantined 経路）。
// 値は status 域と衝突しない番兵である。
constexpr uint32_t kStatusNoAck = 0xFFFFFFFFu;

constexpr size_t kMessageSize = 256;

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
// reqWidth 144, reqHeight 148, reserved 152。合計 256 バイト。
// Offer/Done では 40 以降を 0 で送り、受信側は読まない（旧経路の挙動は変えない）。
// ただし Offer のみ reserved[0..7]（152〜159B）に paint タグ（main の onPaint 入口で
// 取得した QPC µs、uint64 LE、0=欠測）を載せる。旧受信側は reserved を読まないため
// 無視し、旧送信側の 0 は欠測として扱うため版は上げない。
// RenderRequest（exe→main）では borrowId/handle/status を 0 にし、renderId 以降を使う。
// width/height は RenderRequest では要求アトラス寸法（横並び全体）を入れる。
// InputState（exe→main）では予約先頭 2B（reserved[0]=左、[1]=右）に押下ラッチ
// （その 1 秒間に押されたボタンの集合。buttons 欄と同じ bit 割付け）を載せる。
// 短い押下の取りこぼし診断用であり、旧復号器は予約域を読まないため版は上げない。
// InputState のコントローラ pose（下の kInputGrip*・kInputAim* 配置）は
// 種別で区別する再利用であり、RenderRequest の既存欄のオフセットは変えない。
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
  uint8_t reserved[104] = {};
};
#pragma pack(pop)

static_assert(sizeof(TransferMessage) == 256, "転送メッセージは 256 バイト固定であること");
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

// Offer の paint タグ（QPC µs）の配置。reserved の先頭 8B である。
// InputState の予約先頭 2B（reserved[0]=左、[1]=右の押下ラッチ）とは type で区別する再利用であり、
// 版は上げない（旧受信側は reserved を読まず、旧送信側の 0 は欠測扱い）。
constexpr size_t kOfferPaintQpcOffset = 152;
static_assert(kOfferPaintQpcOffset == offsetof(TransferMessage, reserved),
              "paint タグは reserved の先頭であること");

// paint タグを設定する。packed 領域への型ポインタキャストは行わず memcpy で書く。
inline void SetOfferPaintQpcUs(TransferMessage& m, uint64_t qpcUs) {
  uint8_t* base = reinterpret_cast<uint8_t*>(&m);
  std::memcpy(base + kOfferPaintQpcOffset, &qpcUs, sizeof(qpcUs));
}

// paint タグを読む。0 は欠測である。
inline uint64_t OfferPaintQpcUs(const TransferMessage& m) {
  const uint8_t* base = reinterpret_cast<const uint8_t*>(&m);
  uint64_t qpcUs = 0;
  std::memcpy(&qpcUs, base + kOfferPaintQpcOffset, sizeof(qpcUs));
  return qpcUs;
}

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

// ---- haptics 指令（main → exe）と入力スナップショット（exe → main） ----
// どちらも 256B 固定の TransferMessage に載せる。192B→256B 化したため
// 版を上げる（kVersion=3。枠が合わない 192B の相手とは混用しない）。
// 未知の種別を読む側は無視するため、既存経路の挙動は変えない。
// レイアウトを変えたら C++ 側・JS 側（client/src/protocol.ts と tools/test/client/protocol.test.cjs）を同時に更新する。

// HapticCommand（main → exe）の配置。hand 以外は 0 で送らない。
// hand: u32（renderId の下位 32bit。0=左、1=右。上位 32bit は 0）、
// amplitude: float（leftPos[0] と同位置。0〜1）、durationMs: u32（connEpoch と同位置）。
// いずれも RenderRequest とは種別で区別するため、既存オフセットの再利用である。
inline TransferMessage MakeHapticCommand(uint32_t hand, float amplitude, uint32_t durationMs) {
  TransferMessage m{};
  m.magic = kMagic;
  m.version = kVersion;
  m.type = kTypeHapticCommand;
  m.renderId = hand;
  m.leftPos[0] = amplitude;
  m.connEpoch = durationMs;
  return m;
}

// HapticCommand の最低限の正当性。hand は 0/1 のみ、振幅は 0〜1、持続は正であること。
inline bool IsValidHapticCommand(const TransferMessage& m) {
  if (m.magic != kMagic || m.version != kVersion || m.type != kTypeHapticCommand) {
    return false;
  }
  if (m.renderId > 1) {
    return false;
  }
  const float amp = m.leftPos[0];
  if (!(amp >= 0.0f && amp <= 1.0f)) {
    return false;
  }
  return m.connEpoch > 0;
}

inline uint32_t HapticHand(const TransferMessage& m) {
  return static_cast<uint32_t>(m.renderId & 0xFFFFFFFFu);
}
inline float HapticAmplitude(const TransferMessage& m) { return m.leftPos[0]; }
inline uint32_t HapticDurationMs(const TransferMessage& m) { return m.connEpoch; }

// interaction profile の識別子（InputState の profile 欄用）。
// 0 は未取得・未装着、255 は未知の profile である。JS 側も同じ表を持つ。
constexpr uint8_t kProfileNone = 0;
constexpr uint8_t kProfileSimple = 1;    // /interaction_profiles/khr/simple_controller
constexpr uint8_t kProfileOculus = 2;    // /interaction_profiles/oculus/touch_controller
constexpr uint8_t kProfileIndex = 3;     // /interaction_profiles/valve/index_controller
constexpr uint8_t kProfileVive = 4;      // /interaction_profiles/htc/vive_controller
constexpr uint8_t kProfileMotion = 5;    // /interaction_profiles/microsoft/motion_controller
constexpr uint8_t kProfilePico4 = 6;     // /interaction_profiles/bytedance/pico4_controller
constexpr uint8_t kProfilePicoNeo3 = 7;  // /interaction_profiles/bytedance/pico_neo3_controller
constexpr uint8_t kProfilePicoUltra = 8;  // /interaction_profiles/bytedance/pico_ultra_controller_bd
constexpr uint8_t kProfilePicoG3 = 9;    // /interaction_profiles/bytedance/pico_g3_controller
constexpr uint8_t kProfileOther = 255;

inline uint8_t ProfileIdFromName(const char* name) {
  if (name == nullptr || name[0] == '\0') {
    return kProfileNone;
  }
  // 最終節の比較で十分である（suggest 表の顔ぶれは最終節が重複しない）。
  // 将来 profile を足して最終節が衝突したら、ここを完全一致に戻すこと。
  const char* tail = name;
  for (const char* p = name; *p != '\0'; ++p) {
    if (*p == '/') {
      tail = p + 1;
    }
  }
  auto eq = [](const char* a, const char* b) {
    while (*a != '\0' && *a == *b) {
      ++a;
      ++b;
    }
    return *a == *b;
  };
  if (eq(tail, "simple_controller")) {
    return kProfileSimple;
  }
  if (eq(tail, "touch_controller")) {
    return kProfileOculus;
  }
  if (eq(tail, "index_controller")) {
    return kProfileIndex;
  }
  if (eq(tail, "vive_controller")) {
    return kProfileVive;
  }
  if (eq(tail, "motion_controller")) {
    return kProfileMotion;
  }
  if (eq(tail, "pico4_controller")) {
    return kProfilePico4;
  }
  if (eq(tail, "pico_neo3_controller")) {
    return kProfilePicoNeo3;
  }
  if (eq(tail, "pico_ultra_controller_bd")) {
    return kProfilePicoUltra;
  }
  if (eq(tail, "pico_g3_controller")) {
    return kProfilePicoG3;
  }
  return kProfileOther;
}

// 0〜1 のアナログ値を 0〜255 に丸める。範囲外は端に寄せる。
inline uint8_t QuantizeUnitFloat(float v) {
  float t = v * 255.0f;
  if (t < 0.0f) {
    return 0;
  }
  if (t > 255.0f) {
    return 255;
  }
  return static_cast<uint8_t>(t + 0.5f);
}

// -1〜1 のスティック値を int8（-127〜127）に丸め、u8 として載せる。範囲外は端に寄せる。
inline uint8_t QuantizeStick(float v) {
  float t = v * 127.0f;
  if (t < -127.0f) {
    t = -127.0f;
  }
  if (t > 127.0f) {
    t = 127.0f;
  }
  const int q = static_cast<int>(t + (t >= 0.0f ? 0.5f : -0.5f));
  return static_cast<uint8_t>(static_cast<int8_t>(q));
}

// ボタンの bit 位置（InputState の buttons/active/supported 欄で共通）。
constexpr uint8_t kBtnStickClick = 1u << 0;
constexpr uint8_t kBtnPadClick = 1u << 1;
constexpr uint8_t kBtnPrimary = 1u << 2;
constexpr uint8_t kBtnSecondary = 1u << 3;
constexpr uint8_t kBtnMenu = 1u << 4;

// InputState（exe → main）の片手 16B の欄位置（手 0 は 56〜、手 1 は 72〜）。
// flags: bit0=接続（grip/aim のいずれかが locate 可）、bit1=grip 有効、bit2=aim 有効。
// active 系は「値が意味を持つ」合図、supported 系は「profile が束縛を持つ」合図である。
// supported=0 は非対応、supported=1 かつ active=0 は inactive、active=1 は有効値である。
// 「0」と「未対応」の区別はこの 2 段で行う（単なる 0 埋めと混同しない）。
constexpr size_t kInputHandStride = 16;
constexpr size_t kInputHand0 = 56;
constexpr size_t kInputHandFlags = 0;
constexpr size_t kInputHandProfile = 1;
constexpr size_t kInputHandTrigger = 2;
constexpr size_t kInputHandSqueeze = 3;
constexpr size_t kInputHandStickX = 4;
constexpr size_t kInputHandStickY = 5;
constexpr size_t kInputHandPadX = 6;
constexpr size_t kInputHandPadY = 7;
constexpr size_t kInputHandButtons = 8;
constexpr size_t kInputHandBtnActive = 9;
constexpr size_t kInputHandFloatActive = 10;  // bit0=trigger、bit1=squeeze。
constexpr size_t kInputHandVecActive = 11;    // bit0=thumbstick、bit1=trackpad。
constexpr size_t kInputHandBtnSupported = 12;
constexpr size_t kInputHandAnalogSupported = 13;  // bit0=trigger、bit1=squeeze、bit2=stick、bit3=pad。

// InputState（exe → main）のコントローラ pose 配置。各 pose は position xyz＋
// orientation xyzw の float LE（28B）である。
// 左手 2 pose は InputState では未使用の 88〜143B（RenderRequest では rightPos 末尾・
// rightOri・左右 FOV の位置）に、右手 2 pose は 160〜215B の拡張領域に載せる。
// 種別（type=5）で区別する再利用であり、RenderRequest の既存欄のオフセットは
// 変えない。InputState では 144〜159B（req 欄・ラッチ・詰め物）と 216〜255B は 0 である。
// 有効性は既存の手 flags（bit1=grip・bit2=aim）で表し、新たな欄は設けない。
// 無効時の値は 0 のままにし、受信側は読まないこと（JS 側も flags を見て valid を決める）。
constexpr size_t kInputPoseSize = 28;
constexpr size_t kInputGripLeft = 88;
constexpr size_t kInputAimLeft = 116;
constexpr size_t kInputGripRight = 160;
constexpr size_t kInputAimRight = 188;

// pose 1 件の値束。所有権の意味は持たない。
struct ControllerPose {
  float position[3] = {};
  float orientation[4] = {};
};

// InputState メッセージの指定オフセットへ pose を書く・読む。
// 呼び出し側は 4B 境界の上記 kInput* 配置を使うこと。
inline void WriteInputPose(TransferMessage* m, size_t offset, const ControllerPose& pose) {
  uint8_t* base = reinterpret_cast<uint8_t*>(m);
  float* dst = reinterpret_cast<float*>(base + offset);
  for (int i = 0; i < 3; ++i) {
    dst[i] = pose.position[i];
  }
  for (int i = 0; i < 4; ++i) {
    dst[3 + i] = pose.orientation[i];
  }
}

inline ControllerPose ReadInputPose(const TransferMessage& m, size_t offset) {
  const uint8_t* base = reinterpret_cast<const uint8_t*>(&m);
  const float* src = reinterpret_cast<const float*>(base + offset);
  ControllerPose pose{};
  for (int i = 0; i < 3; ++i) {
    pose.position[i] = src[i];
  }
  for (int i = 0; i < 4; ++i) {
    pose.orientation[i] = src[3 + i];
  }
  return pose;
}

// InputState pose 配置。左手 2 pose は 88〜143B、右手 2 pose は 160〜215B であり、
// 境界は 4B 揃え、末尾は 256B に収まること。ラッチ（152,153）と req 欄（144〜151）は避ける。
static_assert(kInputAimLeft == kInputGripLeft + kInputPoseSize, "左 aim pose は左 grip の直後であること");
static_assert(kInputAimRight == kInputGripRight + kInputPoseSize,
              "右 aim pose は右 grip の直後であること");
static_assert(kInputAimRight + kInputPoseSize == 216, "pose 領域の末尾は 216 であること");
static_assert(kInputAimRight + kInputPoseSize <= kMessageSize, "pose 領域はメッセージに収まること");

}  // namespace xrift::transfer
