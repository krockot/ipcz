// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_REMOTE_ROUTER_LINK_H_
#define IPCZ_SRC_CORE_REMOTE_ROUTER_LINK_H_

#include <cstdint>

#include "core/router_link.h"
#include "core/routing_id.h"

namespace ipcz {
namespace core {

class NodeLink;

class RemoteRouterLink : public RouterLink {
 public:
  RemoteRouterLink(mem::Ref<NodeLink> node_link,
                   RoutingId routing_id,
                   uint32_t link_state_index);

  const mem::Ref<NodeLink>& node_link() const { return node_link_; }
  RoutingId routing_id() const { return routing_id_; }

  // RouterLink:
  void Deactivate() override;
  RouterLinkState& GetLinkState() override;
  mem::Ref<Router> GetLocalTarget() override;
  bool IsRemoteLinkTo(NodeLink& node_link, RoutingId routing_id) override;
  bool WouldParcelExceedLimits(size_t data_size,
                               const IpczPutLimits& limits) override;
  void AcceptParcel(Parcel& parcel) override;
  void AcceptRouteClosure(Side side, SequenceNumber sequence_length) override;

 private:
  ~RemoteRouterLink() override;

  const mem::Ref<NodeLink> node_link_;
  const RoutingId routing_id_;
  const uint32_t link_state_index_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_REMOTE_ROUTER_LINK_H_
