// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_LOCAL_ROUTER_LINK_H_
#define IPCZ_SRC_CORE_LOCAL_ROUTER_LINK_H_

#include <utility>

#include "core/router_link.h"
#include "core/side.h"
#include "mem/ref_counted.h"

namespace ipcz {
namespace core {

class Router;

// Local link between routers. A LocalRouterLink can only ever serve as a peer
// link. Predecessor and successor links are only created when extending a route
// across a NodeLink, which means they are always RemoteRouterLinks.
class LocalRouterLink : public RouterLink {
 public:
  static std::pair<mem::Ref<LocalRouterLink>, mem::Ref<LocalRouterLink>>
  CreatePair(mem::Ref<Router> left_router, mem::Ref<Router> right_router);

  // RouterLink:
  void Deactivate() override;
  RouterLinkState& GetLinkState() override;
  mem::Ref<Router> GetLocalTarget() override;
  bool IsRemoteLinkTo(NodeLink& node_link, RoutingId routing_id) override;
  bool IsLinkToOtherSide() override;
  bool WouldParcelExceedLimits(size_t data_size,
                               const IpczPutLimits& limits) override;
  void AcceptParcel(Parcel& parcel) override;
  void AcceptRouteClosure(Side side, SequenceNumber sequence_length) override;
  void StopProxying(SequenceNumber inbound_sequence_length,
                    SequenceNumber outbound_sequence_length) override;
  void RequestProxyBypassInitiation(const NodeName& to_new_peer,
                                    RoutingId proxy_peer_routing_id,
                                    const absl::uint128& bypass_key) override;
  void BypassProxyToSameNode(RoutingId new_routing_id,
                             SequenceNumber sequence_length) override;
  void StopProxyingToLocalPeer(SequenceNumber sequence_length) override;
  void ProxyWillStop(SequenceNumber sequence_length) override;
  void LogRouteTrace(Side toward_side) override;

 private:
  class SharedState;

  LocalRouterLink(Side side, mem::Ref<SharedState> state);
  ~LocalRouterLink() override;

  const Side side_;
  const mem::Ref<SharedState> state_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_LOCAL_ROUTER_LINK_H_
