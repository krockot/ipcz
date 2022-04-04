// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/node_name.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>

#include "third_party/abseil-cpp/absl/base/macros.h"

namespace ipcz {

NodeName::~NodeName() = default;

std::string NodeName::ToString() const {
  char chars[33];
  int length = snprintf(chars, 33, "%016" PRIx64 "%016" PRIx64, high_, low_);
  ABSL_ASSERT(length == 32);
  return std::string(chars, 32);
}

}  // namespace ipcz
