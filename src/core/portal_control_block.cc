// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/portal_control_block.h"

namespace ipcz {
namespace core {

PortalControlBlock::PortalControlBlock() = default;

PortalControlBlock::~PortalControlBlock() = default;

// static
PortalControlBlock& PortalControlBlock::Initialize(void* where) {
  return *(new (where) PortalControlBlock());
}

PortalControlBlock::SideState::SideState() = default;

PortalControlBlock::SideState::~SideState() = default;

}  // namespace core
}  // namespace ipcz
