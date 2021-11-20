// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/router_link_state.h"

#include <cstring>
#include <new>
#include <thread>

namespace ipcz {
namespace core {

RouterLinkState::SideState::SideState() = default;

RouterLinkState::SideState::~SideState() = default;

RouterLinkState::Locked::Locked(RouterLinkState& state, Side side)
    : side_(side), state_(state) {
  state_.Lock();
}

RouterLinkState::Locked::~Locked() {
  state_.Unlock();
}

RouterLinkState::RouterLinkState() = default;

RouterLinkState::~RouterLinkState() = default;

// static
RouterLinkState& RouterLinkState::Initialize(void* where) {
  memset(where, 0, sizeof(RouterLinkState));
  return *(new (where) RouterLinkState());
}

void RouterLinkState::Lock() {
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

void RouterLinkState::Unlock() {
  locked_.store(false, std::memory_order_release);
}

}  // namespace core
}  // namespace ipcz
