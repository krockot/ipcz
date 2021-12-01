// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_DEBUG_LOG_H_
#define IPCZ_SRC_DEBUG_LOG_H_

#include <ostream>
#include <sstream>

#include "third_party/abseil-cpp/absl/base/log_severity.h"

#define LOG(level)                                                     \
  ::ipcz::debug::LogMessage(__FILE__, __LINE__,                        \
                            ::ipcz::debug::LogMessage::kLevel_##level) \
      .stream()

#ifdef NDEBUG
#define DLOG(level) true ? (void)0 : std::ostream(nullptr)
#define DVLOG(verbosity) true ? (void)0 : std::ostream(nullptr)
#else
#define DLOG(level) LOG(level)
#define DVLOG(verbosity) \
    if (::ipcz::debug::GetVerbosityLevel() >= verbosity) DLOG(INFO)
#endif

namespace ipcz {
namespace debug {

class LogMessage {
 public:
  using Level = absl::LogSeverity;
  static constexpr Level kLevel_INFO = absl::LogSeverity::kInfo;
  static constexpr Level kLevel_WARNING = absl::LogSeverity::kWarning;
  static constexpr Level kLevel_ERROR = absl::LogSeverity::kError;
  static constexpr Level kLevel_FATAL = absl::LogSeverity::kFatal;

  LogMessage(const char* file, int line, Level level);
  ~LogMessage();

  std::ostream& stream() { return stream_; }

 private:
  std::stringstream stream_;
};

void SetVerbosityLevel(int level);
int GetVerbosityLevel();

}  // namespace debug
}  // namespace ipcz

#endif  // IPCZ_SRC_DEBUG_LOG_H_
