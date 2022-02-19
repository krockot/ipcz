// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util/hex_dump.h"

#include <string>
#include <vector>

#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {

namespace {

const char kHexChars[] = "0123456789ABCDEF";

}  // namespace

std::string HexDump(absl::Span<const uint8_t> bytes) {
  std::vector<char> chars(bytes.size() * 2);
  char* out = chars.data();
  for (size_t i = 0; i < bytes.size(); ++i, out += 2) {
    uint8_t byte = bytes[i];
    out[0] = kHexChars[byte >> 4];
    out[1] = kHexChars[byte & 15];
  }
  return std::string(chars.begin(), chars.end());
}

}  // namespace ipcz
