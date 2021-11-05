// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_PORTAL_LINK_H_
#define IPCZ_SRC_CORE_PORTAL_LINK_H_

#include "core/node_link.h"
#include "core/portal_link_state.h"
#include "core/route_id.h"
#include "mem/ref_counted.h"
#include "os/memory.h"

namespace ipcz {
namespace core {

class Parcel;

// PortalLink owns a route between two portals on opposite ends of a NodeLink.
// A PortalLink may be used as a peer, to both send and receive parcels to and
// from the remote node, or it may be used as a forwarding link to forward along
// incoming parcels arriving at a portal that has moved to another node.
class PortalLink : public mem::RefCounted {
 public:
  PortalLink(mem::Ref<NodeLink> node,
             RouteId route,
             os::Memory::Mapping link_state);

  NodeLink& node() const { return *node_; }
  RouteId route() const { return route_; }
  const os::Memory::Mapping& link_state() const { return link_state_; }

  void SendParcel(Parcel& parcel);
  void NotifyClosed();

 private:
  ~PortalLink() override;

  const mem::Ref<NodeLink> node_;
  const RouteId route_;
  const os::Memory::Mapping link_state_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_PORTAL_LINK_H_
