// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/portal_link_state.h"

#include <new>
#include <thread>

namespace ipcz {
namespace core {

PortalLinkState::SideState::SideState() = default;

PortalLinkState::SideState::~SideState() = default;

PortalLinkState::Locked::Locked(const os::Memory::Mapping& link_state_mapping,
                                Side side)
    : side_(side), link_state_(*link_state_mapping.As<PortalLinkState>()) {}

PortalLinkState::Locked::Locked(PortalLinkState& link_state, Side side)
    : side_(side), link_state_(link_state) {
  link_state_.Lock();
}

PortalLinkState::Locked::~Locked() {
  link_state_.Unlock();
}

PortalLinkState::PortalLinkState() = default;

PortalLinkState::~PortalLinkState() = default;

// static
PortalLinkState& PortalLinkState::Initialize(void* where) {
  return *(new (where) PortalLinkState());
}

void PortalLinkState::Lock() {
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

    // TODO: instrument this path

    std::this_thread::yield();
  }
}

void PortalLinkState::Unlock() {
  locked_.store(false, std::memory_order_release);
}

}  // namespace core
}  // namespace ipcz
