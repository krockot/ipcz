// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/trap.h"

namespace ipcz {
namespace core {

Trap::Trap(const IpczTrapConditions& conditions,
           IpczTrapEventHandler handler,
           uintptr_t context)
    : conditions_(conditions), handler_(handler), context_(context) {}

Trap::~Trap() = default;

}  // namespace core
}  // namespace ipcz
