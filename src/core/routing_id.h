// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_ROUTING_ID_H_
#define IPCZ_SRC_CORE_ROUTING_ID_H_

#include <cstdint>

namespace ipcz {
namespace core {

// Identifies a route along a NodeLink. Each route is a path between a unique
// pair of Router instances, one in each linked node. New RoutingIds are
// allocated atomically by either side of the NodeLink.
//
// TODO: strong alias?
using RoutingId = uint64_t;

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_ROUTING_ID_H_
