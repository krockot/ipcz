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

bool RouterLinkState::SetSideReady(LinkSide side) {
  const Status kReadyOnThisSide = side == LinkSide::kA ? kReadyOnA : kReadyOnB;
  const Status kReadyOnOtherSide = side == LinkSide::kA ? kReadyOnB : kReadyOnA;

  Status expected = Status::kNotReady;
  if (status.compare_exchange_strong(expected, kReadyOnThisSide,
                                     std::memory_order_relaxed)) {
    return true;
  }
  if (expected == kReadyOnThisSide || expected == kReady) {
    return true;
  }
  if (expected != kReadyOnOtherSide) {
    return false;
  }
  return status.compare_exchange_strong(expected, kReady,
                                        std::memory_order_relaxed);
}

bool RouterLinkState::TryToLockForBypass(LinkSide side) {
  const Status kLockedByThisSide =
      side == LinkSide::kA ? kLockedByA : kLockedByB;
  Status expected = Status::kReady;
  return status.compare_exchange_strong(expected, kLockedByThisSide,
                                        std::memory_order_relaxed);
}

bool RouterLinkState::CancelBypassLock() {
  Status expected = kLockedByA;
  if (status.compare_exchange_strong(expected, kReady,
                                     std::memory_order_relaxed)) {
    return true;
  }
  if (expected == kReady) {
    return true;
  }
  if (expected != kLockedByB) {
    return false;
  }
  if (status.compare_exchange_strong(expected, kReady,
                                     std::memory_order_relaxed)) {
    return true;
  }
  return expected == kReady;
}

}  // namespace core
}  // namespace ipcz
