// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_UTIL_HEX_DUMP_H_
#define IPCZ_SRC_UTIL_HEX_DUMP_H_

#include <string>

#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {

// Stringifies a span of bytes as a string of hex octets.
std::string HexDump(absl::Span<const uint8_t> bytes);

}  // namespace ipcz

#endif  // IPCZ_SRC_UTIL_HEX_DUMP_H_
