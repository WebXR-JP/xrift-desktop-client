#pragma once

// 標準出力・標準エラーへの簡易ログ出力。[info]/[warn]/[error] を接頭辞に付ける。
// printf 形式の可変引数を受け付ける。Info/Warn は stdout、Error は stderr に出す。

namespace xrift {

void Info(const char* format, ...);
void Warn(const char* format, ...);
void Error(const char* format, ...);

}  // namespace xrift
