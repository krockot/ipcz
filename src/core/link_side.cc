// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/link_side.h"

namespace ipcz {
namespace core {

// static
constexpr LinkSide::Value LinkSide::kA;

// static
constexpr LinkSide::Value LinkSide::kB;

std::string LinkSide::ToString() const {
  return value_ == Value::kA ? "left" : "right";
}

}  // namespace core
}  // namespace ipcz
