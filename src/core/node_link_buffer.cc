// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/node_link_buffer.h"

#include <new>

namespace ipcz {
namespace core {

NodeLinkBuffer::NodeLinkBuffer() = default;

NodeLinkBuffer::~NodeLinkBuffer() = default;

// static
void NodeLinkBuffer::Init(void* memory, uint32_t num_initial_portals) {
  NodeLinkBuffer& buffer = *(new (memory) NodeLinkBuffer());
  buffer.link_state_.AllocateRoutingIds(num_initial_portals);
  buffer.next_router_link_index_ = num_initial_portals;
}

RoutingId NodeLinkBuffer::AllocateRoutingIds(size_t count) {
  return link_state_.AllocateRoutingIds(count);
}

}  // namespace core
}  // namespace ipcz
