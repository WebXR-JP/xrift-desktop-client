# SPIR-V バイナリを uint32_t 配列の C++ ヘッダへ変換する。
# 使い捨ての埋め込み生成器であり、実行時の外部ファイル参照を無くすためのものである。
# 必須の引数: SPV_IN（入力 .spv）、HEADER_OUT（出力 .h）。
#
# バイト順の注意: file(READ HEX) で得る 8 文字はファイルのバイト列そのまま
# （先頭ワードなら 03 02 23 07）であり、そのまま 0x........u と書くと
# リトルエンディアンでは値が反転して読める（0x03022307 になってしまう）。
# uint32 ワードの値として正しくなるよう、バイト対を並べ替えて出す
# （先頭ワードは 0x07230203 になる）。並べ替え忘れは下の表明で止める。

if(NOT DEFINED SPV_IN OR NOT DEFINED HEADER_OUT)
  message(FATAL_ERROR "SpvToHeader.cmake には SPV_IN と HEADER_OUT が必要")
endif()
if(NOT EXISTS "${SPV_IN}")
  message(FATAL_ERROR "SPIR-V が見つからない: ${SPV_IN}")
endif()

file(READ "${SPV_IN}" SPV_HEX HEX)
string(LENGTH "${SPV_HEX}" HEX_LEN)
math(EXPR REMAINDER "${HEX_LEN} % 8")
if(NOT REMAINDER EQUAL 0)
  message(FATAL_ERROR "SPIR-V の大きさが 4 の倍数ではない: ${SPV_IN}")
endif()

# 先頭 4 バイトは LE で 03 02 23 07（0x07230203）と並ぶ。
string(SUBSTRING "${SPV_HEX}" 0 8 MAGIC)
if(NOT MAGIC STREQUAL "03022307")
  message(FATAL_ERROR "SPIR-V のマジックナンバが無い: ${SPV_IN}")
endif()

math(EXPR WORD_COUNT "${HEX_LEN} / 8")
math(EXPR LAST_INDEX "${WORD_COUNT} - 1")
set(BODY "")
set(FIRST_WORD "")
foreach(I RANGE 0 ${LAST_INDEX})
  math(EXPR OFF "${I} * 8")
  string(SUBSTRING "${SPV_HEX}" ${OFF} 8 WORD)
  # バイト列を uint32 の値に直す（LE の語順に組み直す）。
  string(SUBSTRING "${WORD}" 0 2 B0)
  string(SUBSTRING "${WORD}" 2 2 B1)
  string(SUBSTRING "${WORD}" 4 2 B2)
  string(SUBSTRING "${WORD}" 6 2 B3)
  set(WORD "${B3}${B2}${B1}${B0}")
  if(I EQUAL 0)
    set(FIRST_WORD "${WORD}")
  endif()
  string(APPEND BODY "    0x${WORD}u,\n")
endforeach()
# 出力側の先頭ワードがマジック (0x07230203) になること。
# バイト順の取り違えはドライバの vkCreateShaderModule をすり抜け、
# vkCreateComputePipelines の ErrorUnknown として現れるため、ここで止める。
if(NOT FIRST_WORD STREQUAL "07230203")
  message(FATAL_ERROR "SPIR-V の先頭ワードがマジックにならない: ${FIRST_WORD}")
endif()

file(WRITE "${HEADER_OUT}"
  "// 自動生成。直接編集しない（shaders/check.comp から生成）。\n"
  "#pragma once\n"
  "\n"
  "#include <cstdint>\n"
  "\n"
  "// M1b 全画面 GPU 検査の compute シェーダ（SPIR-V ワード列）。\n"
  "namespace xrift::transfer {\n"
  "\n"
  "inline constexpr uint32_t kCheckCompSpv[] = {\n"
  "${BODY}"
  "};\n"
  "\n"
  "}  // namespace xrift::transfer\n"
)
