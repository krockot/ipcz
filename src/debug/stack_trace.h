// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_DEBUG_STACK_TRACE_H_
#define IPCZ_SRC_DEBUG_STACK_TRACE_H_

#include <string>

#include "third_party/abseil-cpp/absl/container/inlined_vector.h"

namespace ipcz {
namespace debug {

class StackTrace {
 public:
  static constexpr size_t kDefaultFrameCount = 16;

  explicit StackTrace(size_t frame_count = kDefaultFrameCount);
  ~StackTrace();

  static void EnableStackTraceSymbolization(const char* argv0);

  std::string ToString() const;

 private:
  absl::InlinedVector<void*, kDefaultFrameCount> frames_;
};

}  // namespace debug
}  // namespace ipcz

#endif  // IPCZ_SRC_DEBUG_STACK_TRACE_H_
