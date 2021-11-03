// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/node_name.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>

#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/numeric/int128.h"
#include "util/random.h"

namespace ipcz {
namespace core {

NodeName::NodeName(decltype(kRandom)) : value_(util::RandomUint128()) {}

NodeName::~NodeName() = default;

std::string NodeName::ToString() const {
  char chars[33];
  int length =
      snprintf(chars, 33, "%016" PRIx64 "%016" PRIx64,
               absl::Uint128High64(value_), absl::Uint128Low64(value_));
  ABSL_ASSERT(length == 32);
  return std::string(chars, 32);
}

}  // namespace core
}  // namespace ipcz
