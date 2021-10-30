// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/portal_control_block.h"

#include <thread>

namespace ipcz {
namespace core {

PortalControlBlock::SideState::SideState() = default;

PortalControlBlock::SideState::~SideState() = default;

PortalControlBlock::Locked::Locked(PortalControlBlock& block) : block_(block) {
  block_.Lock();
}

PortalControlBlock::Locked::~Locked() {
  block_.Unlock();
}

PortalControlBlock::PortalControlBlock() = default;

PortalControlBlock::~PortalControlBlock() = default;

// static
PortalControlBlock& PortalControlBlock::Initialize(void* where) {
  return *(new (where) PortalControlBlock());
}

void PortalControlBlock::Lock() {
  for (;;) {
    size_t num_attempts_before_yield = 10;
    while (num_attempts_before_yield--) {
      bool expected = false;
      if (locked_.compare_exchange_weak(expected, true,
                                        std::memory_order_acquire,
                                        std::memory_order_relaxed)) {
        return;
      }
    }
    std::this_thread::yield();
  }
}

void PortalControlBlock::Unlock() {
  locked_.store(false, std::memory_order_release);
}

}  // namespace core
}  // namespace ipcz
