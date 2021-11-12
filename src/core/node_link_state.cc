// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/node_link_state.h"

#include <new>

namespace ipcz {
namespace core {

NodeLinkState::NodeLinkState() = default;

NodeLinkState::~NodeLinkState() = default;

// static
NodeLinkState& NodeLinkState::Initialize(void* where) {
  return *(new (where) NodeLinkState());
}

RoutingId NodeLinkState::AllocateRoutingIds(size_t count) {
  return next_routing_id_.fetch_add(count, std::memory_order_relaxed);
}

}  // namespace core
}  // namespace ipcz
