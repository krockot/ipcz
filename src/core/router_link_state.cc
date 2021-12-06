// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/router_link_state.h"

#include <cstring>
#include <new>

namespace ipcz {
namespace core {

RouterLinkState::RouterLinkState() = default;

RouterLinkState::~RouterLinkState() = default;

// static
RouterLinkState& RouterLinkState::Initialize(void* where) {
  memset(where, 0, sizeof(RouterLinkState));
  return *(new (where) RouterLinkState());
}

bool RouterLinkState::SetSideReady(Side side) {
  const Status kThisSideReady = side == Side::kLeft ? kLeftReady : kRightReady;
  const Status kOtherSideReady = side == Side::kLeft ? kRightReady : kLeftReady;

  Status expected = Status::kNotReady;
  if (status.compare_exchange_strong(expected, kThisSideReady,
                                     std::memory_order_relaxed)) {
    return true;
  }
  if (expected != kOtherSideReady) {
    return false;
  }
  return status.compare_exchange_strong(expected, kReady,
                                        std::memory_order_relaxed);
}

bool RouterLinkState::TryToDecay(Side side) {
  const Status kThisSideDecaying =
      side == Side::kLeft ? kLeftDecaying : kRightDecaying;
  Status expected = Status::kReady;
  return status.compare_exchange_strong(expected, kThisSideDecaying,
                                        std::memory_order_relaxed);
}

}  // namespace core
}  // namespace ipcz
