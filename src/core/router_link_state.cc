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
  if (expected == kReady) {
    return true;
  }
  if (expected != kReadyOnOtherSide) {
    return false;
  }
  return status.compare_exchange_strong(expected, kReady,
                                        std::memory_order_relaxed);
}

bool RouterLinkState::TryToDecay(LinkSide side) {
  const Status kDecayOnThisSide = side == LinkSide::kA ? kDecayOnA : kDecayOnB;
  Status expected = Status::kReady;
  return status.compare_exchange_strong(expected, kDecayOnThisSide,
                                        std::memory_order_relaxed);
}

bool RouterLinkState::CancelDecay() {
  Status expected = kDecayOnA;
  if (status.compare_exchange_strong(expected, kReady,
                                     std::memory_order_relaxed)) {
    return true;
  }
  if (expected != kDecayOnB) {
    return false;
  }
  return status.compare_exchange_strong(expected, kReady,
                                        std::memory_order_relaxed);
}

}  // namespace core
}  // namespace ipcz
