// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_PORTAL_IN_TRANSIT_H_
#define IPCZ_SRC_CORE_PORTAL_IN_TRANSIT_H_

#include "core/route_id.h"
#include "core/sequence_number.h"
#include "core/side.h"
#include "mem/ref_counted.h"
#include "os/memory.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace ipcz {
namespace core {

class Portal;
class PortalLink;

struct PortalInTransit {
  PortalInTransit();
  PortalInTransit(PortalInTransit&&);
  PortalInTransit& operator=(PortalInTransit&&);
  ~PortalInTransit();

  mem::Ref<Portal> portal;

  // The side of its portal pair to which this portal belongs.
  Side side;

  // Indicates whether the peer is known to be closed already.
  bool peer_closed;

  // If the peer portal is closed, this is the total number of parcels sent from
  // its side. The moved portal can use this to know when it's received its last
  // parcel it can ever receive.
  //
  // If `peer_closed` is false, this field should be ignored.
  SequenceNumber peer_sequence_length;

  // The sequence number of the next incoming parcel expected on this side of
  // the portal pair.
  SequenceNumber next_incoming_sequence_number;

  // The sequence number to use for the next outgoing parcel transmitted from
  // this side of the portal.
  SequenceNumber next_outgoing_sequence_number;

  // The route assigned to this portal along the transmitting NodeLink, if the
  // parcel carring this portal was actually transmitted. On the receiving side
  // of portal transit this is deserialized as the peer link.
  //
  // On the sending side, if this portal's peer was local to the same node then
  // this link becomes that portal's peer link. Otherwise it becomes the moved
  // portal's forwarding link.
  mem::Ref<PortalLink> link;

  // The portal's local peer prior to initiating transit. Transit may be
  // cancelled before the containing parcel is shipped off, and if this portal
  // was part of a local pair prior to the transit attempt, we use this link to
  // restore both portals to a working state.
  mem::Ref<Portal> local_peer_before_transit;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_PORTAL_IN_TRANSIT_H_
