// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/link_type.h"

namespace ipcz {
namespace core {

// static
constexpr LinkType::Value LinkType::kCentral;

// static
constexpr LinkType::Value LinkType::kPeripheralInward;

// static
constexpr LinkType::Value LinkType::kPeripheralOutward;

// static
constexpr LinkType::Value LinkType::kBridge;

std::string LinkType::ToString() const {
  switch (value_) {
    case Value::kCentral:
      return "central";
    case Value::kPeripheralInward:
      return "peripheral-inward";
    case Value::kPeripheralOutward:
      return "peripheral-outward";
    case Value::kBridge:
      return "bridge";
  }
}

}  // namespace core
}  // namespace ipcz
