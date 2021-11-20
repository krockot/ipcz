// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_NODE_LINK_BUFFER_H_
#define IPCZ_SRC_CORE_NODE_LINK_BUFFER_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "core/node_link_state.h"
#include "core/router_link_state.h"
#include "core/routing_id.h"

namespace ipcz {
namespace core {

// A memory buffer shared by both ends of a NodeLink.
struct NodeLinkBuffer {
 public:
  NodeLinkBuffer();
  ~NodeLinkBuffer();

  static void Init(void* memory, uint32_t num_initial_portals);

  NodeLinkState& node_link_state() { return link_state_; }

  RouterLinkState& router_link_state(uint32_t index) {
    return router_link_states_[index];
  }

  RoutingId AllocateRoutingIds(size_t count);

 private:
  NodeLinkState link_state_;

  // TODO: this is quite a hack - implement a pool of NodeLinkBuffers and
  // allocate RouterLinkStates dynamically
  std::atomic<uint32_t> next_router_link_index_{0};
  std::array<RouterLinkState, 2048> router_link_states_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NODE_LINK_BUFFER_H_
