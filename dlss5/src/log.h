// SPDX-License-Identifier: MIT
//
// Tiny logger shared by the helper and the tests: stdout plus an optional file.

#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace dsf_dlss5 {

inline std::mutex& LogMutex() {
  static std::mutex mutex;
  return mutex;
}

inline FILE*& LogFile() {
  static FILE* file = nullptr;
  return file;
}

inline void OpenLogFile(const wchar_t* path) {
  std::lock_guard<std::mutex> lock(LogMutex());
  if (LogFile() != nullptr) std::fclose(LogFile());
  LogFile() = _wfsopen(path, L"w", _SH_DENYWR);
}

// toConsole=false keeps chatty sources (NGX) in the log file only.
inline void LogRaw(const char* prefix, const char* text, bool toConsole = true) {
  std::lock_guard<std::mutex> lock(LogMutex());
  const size_t length = std::strlen(text);
  const bool newline = length != 0 && text[length - 1] == '\n';
  if (toConsole) {
    std::fprintf(stdout, "%s%s%s", prefix, text, newline ? "" : "\n");
    std::fflush(stdout);
  }
  if (FILE* file = LogFile()) {
    std::fprintf(file, "%s%s%s", prefix, text, newline ? "" : "\n");
    std::fflush(file);
  }
}

inline void Log(const char* format, ...) {
  char buffer[2048];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  LogRaw("", buffer);
}

}  // namespace dsf_dlss5
