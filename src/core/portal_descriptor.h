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

  // The RoutingId assigned to this portal on serialization and deserialization.
  // Note that this field is controlled by the NodeLink that sends or receives
  // the descriptor, rather than by the serializing Portal.
  RoutingId routing_id;

  // Which side of its pair this portal falls on.
  Side side;

  // Whether or not the peer was known to be closed when the descriptor was
  // serialized.
  bool peer_closed : 1;

  // When this is false, the serialized portal is a successor to another portal
  // along the same side of the route, so the NodeLink's routing ID identified
  // by `routing_id` corresponds to the new portal's predecessor link. When this
  // field is true however, that indicates that the sent portal was split from a
  // local pair, and in that case the routing ID identified by `routing_id`
  // corresponds to the new portal's peer link.
  bool route_is_peer : 1;

  // The name of the sender's peer node, and the ID of the routing link between
  // that peer and the sender node. Valid if and only if the new portal must
  // negotiate its peer link with the named node. In this case `bypass_key` must
  // also be non-zero, and the peer node has a copy of the same key.
  //
  // When the new portal is created, it will send a request to the named peer
  // node including the name of its predecessor's node, along with
  // `peer_routing_id` and `bypass_key`. The peer uses this to validate the
  // request, and upon validation the peer accepts the new portal as its own
  // peer.
  NodeName peer_name;
  RoutingId peer_routing_id;
  absl::uint128 bypass_key;

  // The final length of the peer's outgoing parcel sequence. If the peer is
  // not yet closed, this value is ignored and must be zero.
  SequenceNumber peer_sequence_length;

  // The next incoming sequence number needed by the portal in order for it to
  // be readable. This is one more than the sequence number of the last parcel
  // actually read by the application from this portal's side, or zero if no
  // parcels have been read from this side yet.
  SequenceNumber next_incoming_sequence_number;

  // The sequence number to use for the next outgoing parcel leaving this
  // portal. This is one more than the sequence number of the last parcel
  // transmitted from this portal's side, or zero of no parcels have been
  // transmitted yet.
  SequenceNumber next_outgoing_sequence_number;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_PORTAL_DESCRIPTOR_H_
