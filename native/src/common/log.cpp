#include "common/log.h"

#include <cstdarg>
#include <cstdio>

namespace xrift {
namespace {

// 書式付きで 1 行出す。末尾に改行が無ければ付ける。
void WriteLine(FILE* out, const char* prefix, const char* format, va_list args) {
  std::fputs(prefix, out);
  std::fputc(' ', out);
  std::vfprintf(out, format, args);
  std::fputc('\n', out);
  std::fflush(out);
}

}  // namespace

void Info(const char* format, ...) {
  va_list args;
  va_start(args, format);
  WriteLine(stdout, "[info]", format, args);
  va_end(args);
}

void Warn(const char* format, ...) {
  va_list args;
  va_start(args, format);
  WriteLine(stdout, "[warn]", format, args);
  va_end(args);
}

void Error(const char* format, ...) {
  va_list args;
  va_start(args, format);
  WriteLine(stderr, "[error]", format, args);
  va_end(args);
}

}  // namespace xrift
