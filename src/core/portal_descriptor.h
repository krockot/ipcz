// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_PORTAL_DESCRIPTOR_H_
#define IPCZ_SRC_CORE_PORTAL_DESCRIPTOR_H_

#include "core/node_name.h"
#include "core/routing_id.h"
#include "core/sequence_number.h"
#include "core/side.h"
#include "ipcz/ipcz.h"
#include "third_party/abseil-cpp/absl/numeric/int128.h"

namespace ipcz {
namespace core {

// Serialized representation of a Portal sent in a parcel.
struct IPCZ_ALIGN(16) PortalDescriptor {
  PortalDescriptor();
  PortalDescriptor(const PortalDescriptor&);
  PortalDescriptor& operator=(const PortalDescriptor&);
  ~PortalDescriptor();

  Side side;
  bool route_is_peer : 1;
  bool peer_closed : 1;
  SequenceNumber closed_peer_sequence_length;
  RoutingId new_routing_id;
  RoutingId new_decaying_routing_id;
  SequenceNumber next_outgoing_sequence_number;
  SequenceNumber next_incoming_sequence_number;
  NodeName proxy_peer_node_name;
  RoutingId proxy_peer_routing_id;
  absl::uint128 bypass_key;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_PORTAL_DESCRIPTOR_H_
