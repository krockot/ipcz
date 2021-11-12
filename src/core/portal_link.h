// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_PORTAL_LINK_H_
#define IPCZ_SRC_CORE_PORTAL_LINK_H_

#include "core/node_link.h"
#include "core/portal_link_state.h"
#include "core/routing_id.h"
#include "core/sequence_number.h"
#include "mem/ref_counted.h"
#include "os/memory.h"

namespace ipcz {
namespace core {

class Parcel;

// PortalLink owns a routing ID used to communicate between two portals on
// opposite ends of a NodeLink. A PortalLink may be used as a peer, to both send
// and receive parcels to and from the remote node, or it may be used as a
// successor or predecessor link.
class PortalLink : public mem::RefCounted {
 public:
  PortalLink(mem::Ref<NodeLink> node,
             RoutingId routing_id,
             os::Memory::Mapping link_state);

  NodeLink& node() const { return *node_; }
  RoutingId routing_id() const { return routing_id_; }
  PortalLinkState& state() const {
    return *state_mapping_.As<PortalLinkState>();
  }

  void Disconnect();
  void SendParcel(Parcel& parcel);
  void NotifyClosed(SequenceNumber sequence_length);
  void StopProxyingTowardSide(Side side, SequenceNumber sequence_length);
  void InitiateProxyBypass(const NodeName& proxy_peer_name,
                           RoutingId proxy_peer_routing_id,
                           absl::uint128 bypass_key);

 private:
  ~PortalLink() override;

  const mem::Ref<NodeLink> node_;
  const RoutingId routing_id_;
  const os::Memory::Mapping state_mapping_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_PORTAL_LINK_H_
