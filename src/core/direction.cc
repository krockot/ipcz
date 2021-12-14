// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/direction.h"

namespace ipcz {
namespace core {

// static
constexpr Direction::Value Direction::kInward;

// static
constexpr Direction::Value Direction::kOutward;

std::string Direction::ToString() const {
  return value_ == Direction::kInward ? "inward" : "outward";
}

}  // namespace core
}  // namespace ipcz
