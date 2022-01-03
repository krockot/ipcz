// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_ROUTER_DESCRIPTOR_H_
#define IPCZ_SRC_CORE_ROUTER_DESCRIPTOR_H_

#include "core/node_link_address.h"
#include "core/node_name.h"
#include "core/routing_id.h"
#include "core/sequence_number.h"
#include "ipcz/ipcz.h"

namespace ipcz {
namespace core {

// Serialized representation of a Router sent in a parcel. When a portal is
// transferred to a new node, we use this structure to serialize a description
// of a new Router to back that moved portal. This new router is an inward peer
// of the portal's previous router at the sending location.
struct IPCZ_ALIGN(16) RouterDescriptor {
  RouterDescriptor();
  RouterDescriptor(const RouterDescriptor&);
  RouterDescriptor& operator=(const RouterDescriptor&);
  ~RouterDescriptor();

  // These fields are set if and only if proxy bypass should be initiated
  // immediately on deserialization of the new Router. The deserializing node
  // must contact `proxy_peer_node_name` with the name of the node who sent this
  // descriptor, along with `proxy_peer_routing_id` (an existing routing ID
  // between those two nodes, identifying the link we want to bypass).
  NodeName proxy_peer_node_name;
  RoutingId proxy_peer_routing_id;

  // If the other end of the route is already known to be closed when this
  // router is serialized, this is the total number of parcels sent from that
  // end.
  SequenceNumber closed_peer_sequence_length;

  // A new routing ID and RouterLinkState address allocated by the sender on the
  // NodeLink which sends this descriptor. The routing ID may be used either as
  // a peripheral link (the default case) or the route's new central link in the
  // optimized case where `proxy_already_bypassed` is true below. Only in the
  // latter case is the RouterLinkState address used.
  RoutingId new_routing_id;
  NodeLinkAddress new_link_state_address;

  // When `proxy_already_bypassed` is true, this is another new routing ID
  // allocated by the sender on the NodeLink which sends this descriptor. This
  // routing ID is used as peripheral link to the new router's outward peer back
  // on the sending node, as a way for that router to forward any inbound
  // parcels that were still queued or in flight when this router was
  // serialized.
  RoutingId new_decaying_routing_id;

  // The SequenceNumber of the next outbound parcel which can be produced by
  // this router.
  SequenceNumber next_outgoing_sequence_number;

  // The SequenceNumber of the next inbound parcel expected by this router.
  SequenceNumber next_incoming_sequence_number;

  // The total length of the sequence of parcels expected on the decaying link
  // established by `new_decaying_routing_id`, if and only if
  // `proxy_already_bypassed` is true. The decaying link is expected to receive
  // only parcels between `next_incoming_sequence_number` (inclusive) and
  // `decaying_incoming_sequence_length` (exclusive). If those fields are equal
  // then the decaying link should be ignored and `new_decaying_routing_id` may
  // not be valid.
  SequenceNumber decaying_incoming_sequence_length;

  // Indicates that, as an optimization, the sender was able to circumvent the
  // usual process of first establishing a peripheral link and then initiating
  // proxy bypass. Instead the outward peer of this new router is already
  // configured to route messages directly to the new router, and its former
  // (and local) outward peer is configured to proxy any previously queued or
  // in-flight messages to us over the decaying link described above.
  bool proxy_already_bypassed : 1;

  // Indicates that the other end of the route is already known to be closed.
  // In this case sending any new outbound parcels from this router would be
  // pointless, but there may still be in-flight parcels to receive from the
  // other end. `closed_peer_sequence_length` will indicate the total number of
  // parcels sent from that end, and `next_incoming_sequence_number` can be used
  // to determine whether there are any parcels left to receive.
  bool peer_closed : 1;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_ROUTER_DESCRIPTOR_H_
