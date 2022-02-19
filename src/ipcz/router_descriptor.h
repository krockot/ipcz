// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_ROUTER_DESCRIPTOR_H_
#define IPCZ_SRC_IPCZ_ROUTER_DESCRIPTOR_H_

#include "ipcz/fragment_descriptor.h"
#include "ipcz/ipcz.h"
#include "ipcz/node_name.h"
#include "ipcz/sequence_number.h"
#include "ipcz/sublink_id.h"

namespace ipcz {

// Serialized representation of a Router sent in a parcel. When a portal is
// transferred to a new node, we use this structure to serialize a description
// of the new Router that will back the portal at its new location. This new
// router is an inward peer of the portal's previous router on the sending node.
struct IPCZ_ALIGN(8) RouterDescriptor {
  RouterDescriptor();
  RouterDescriptor(const RouterDescriptor&);
  RouterDescriptor& operator=(const RouterDescriptor&);
  ~RouterDescriptor();

  // These fields are set if and only if proxy bypass should be initiated
  // immediately on deserialization of the new Router. The deserializing node
  // must contact `proxy_peer_node_name` with the name of the node who sent this
  // descriptor, along with `proxy_peer_sublink` (an existing sublink
  // between those two nodes, identifying the link we want to bypass).
  NodeName proxy_peer_node_name;
  SublinkId proxy_peer_sublink;

  // If the other end of the route is already known to be closed when this
  // router is serialized, this is the total number of parcels sent from that
  // end.
  SequenceNumber closed_peer_sequence_length;

  // A new sublink and RouterLinkState fragment allocated by the sender on the
  // NodeLink which sends this descriptor. The sublink may be used either as
  // a peripheral link (the default case) or the route's new central link in the
  // optimized case where `proxy_already_bypassed` is true below. Only in the
  // latter case is the RouterLinkState fragment used.
  SublinkId new_sublink;
  FragmentDescriptor new_link_state_fragment;

  // When `proxy_already_bypassed` is true, this is another new sublink
  // allocated by the sender on the NodeLink which sends this descriptor. This
  // sublink is used as peripheral link to the new router's outward peer back
  // on the sending node, as a way for that router to forward any inbound
  // parcels that were still queued or in flight when this router was
  // serialized.
  SublinkId new_decaying_sublink;

  // The SequenceNumber of the next outbound parcel which can be produced by
  // this router.
  SequenceNumber next_outgoing_sequence_number;

  // The SequenceNumber of the next inbound parcel expected by this router.
  SequenceNumber next_incoming_sequence_number;

  // The total length of the sequence of parcels expected on the decaying link
  // established by `new_decaying_sublink`, if and only if
  // `proxy_already_bypassed` is true. The decaying link is expected to receive
  // only parcels between `next_incoming_sequence_number` (inclusive) and
  // `decaying_incoming_sequence_length` (exclusive). If those fields are equal
  // then the decaying link should be ignored and `new_decaying_sublink` may
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

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_ROUTER_DESCRIPTOR_H_
