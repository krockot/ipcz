// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_NODE_LINK_STATE_H_
#define IPCZ_SRC_CORE_NODE_LINK_STATE_H_

#include <atomic>

#include "core/route_id.h"

namespace ipcz {
namespace core {

// Shared memory state object shared exclusively between two ends of a NodeLink.
struct NodeLinkState {
  NodeLinkState();
  ~NodeLinkState();

  static NodeLinkState& Initialize(void* where);

  RouteId AllocateRoutes(size_t count);

 private:
  std::atomic<RouteId> next_route_id_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NODE_LINK_STATE_H_
