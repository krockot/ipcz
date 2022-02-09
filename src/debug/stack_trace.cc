// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "debug/stack_trace.h"

#include <string>
#include <vector>

#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/debugging/stacktrace.h"
#include "third_party/abseil-cpp/absl/debugging/symbolize.h"
#include "third_party/abseil-cpp/absl/strings/str_join.h"

namespace ipcz {
namespace debug {

StackTrace::StackTrace(size_t frame_count) {
  frames_.resize(frame_count);
  int num_frames =
      absl::GetStackTrace(frames_.data(), static_cast<int>(frame_count), 1);
  ABSL_ASSERT(num_frames >= 0);
  frames_.resize(static_cast<size_t>(num_frames));
}

StackTrace::StackTrace(const StackTrace&) = default;

StackTrace& StackTrace::operator=(const StackTrace&) = default;

StackTrace::~StackTrace() = default;

// static
void StackTrace::EnableStackTraceSymbolization(const char* argv0) {
  absl::InitializeSymbolizer(argv0);
}

std::string StackTrace::ToString() const {
  static constexpr size_t kMaxSymbolizedFrameLength = 256;
  std::string str;
  for (void* frame : frames_) {
    char symbolized[kMaxSymbolizedFrameLength];
    if (absl::Symbolize(frame, symbolized, kMaxSymbolizedFrameLength)) {
      str += std::string(symbolized) + "\n";
    } else {
      str += "<unknown>\n";
    }
  }
  return str;
}

}  // namespace debug
}  // namespace ipcz
