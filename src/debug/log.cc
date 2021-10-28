// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "debug/log.h"

#include <iostream>

#include "build/build_config.h"
#include "third_party/abseil-cpp/absl/base/log_severity.h"

#if defined(OS_POSIX)
#include <sys/types.h>
#include <unistd.h>
#endif

namespace ipcz {
namespace debug {

LogMessage::LogMessage(const char* file, int line, Level level) {
  stream_ << "[";
#if defined(OS_POSIX)
  stream_ << getpid() << ":" << gettid() << ":";
  const char* trimmed_file = strrchr(file, '/') + 1;
#else
  const char* trimmed_file = file;
#endif
  stream_ << absl::LogSeverityName(level) << ":"
          << (trimmed_file ? trimmed_file : file) << "(" << line << ")] ";
}

LogMessage::~LogMessage() {
  std::cerr << stream_.str() << std::endl;
}

}  // namespace debug
}  // namespace ipcz
