// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_DEBUG_HEX_DUMP_H_
#define IPCZ_SRC_DEBUG_HEX_DUMP_H_

#include <string>

#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace debug {

std::string HexDump(absl::Span<const uint8_t> bytes);

}  // namespace debug
}  // namespace ipcz

#endif  // IPCZ_SRC_DEBUG_HEX_DUMP_H_
