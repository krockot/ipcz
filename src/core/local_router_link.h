// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_LOCAL_ROUTER_LINK_H_
#define IPCZ_SRC_CORE_LOCAL_ROUTER_LINK_H_

#include <utility>

#include "core/link_side.h"
#include "core/router.h"
#include "core/router_link.h"
#include "core/router_link_state.h"
#include "mem/ref_counted.h"

namespace ipcz {
namespace core {

// Local link between two Routers on the same node. A LocalRouterLink is always
// connected to the other side of the route. Several RouterLink overrides are
// unimplemented by LocalRouterLink as they are unnecessary and unused for local
// links.
class LocalRouterLink : public RouterLink {
 public:
  enum class InitialState {
    kCannotDecay,
    kCanDecay,
  };

  // Creates a new pair of LocalRouterLinks with the given initial link status
  // and linking the given pair of Routers together. The Routers must not
  // currently have outward links.
  static RouterLink::Pair CreatePair(LinkType type,
                                     InitialState initial_state,
                                     const Router::Pair& routers);

  // RouterLink:
  LinkType GetType() const override;
  mem::Ref<Router> GetLocalTarget() override;
  bool IsRemoteLinkTo(NodeLink& node_link, RoutingId routing_id) override;
  bool CanDecay() override;
  bool SetSideCanDecay() override;
  bool MaybeBeginDecay(absl::uint128* bypass_key) override;
  bool CancelDecay() override;
  bool CanBypassWithKey(const absl::uint128& bypass_key) override;
  bool WouldParcelExceedLimits(size_t data_size,
                               const IpczPutLimits& limits) override;
  void AcceptParcel(Parcel& parcel) override;
  void AcceptRouteClosure(RouteSide route_side,
                          SequenceNumber sequence_length) override;
  void RequestProxyBypassInitiation(const NodeName& to_new_peer,
                                    RoutingId proxy_peer_routing_id,
                                    const absl::uint128& bypass_key) override;
  void StopProxying(SequenceNumber inbound_sequence_length,
                    SequenceNumber outbound_sequence_length) override;
  void ProxyWillStop(SequenceNumber sequence_length) override;
  void BypassProxyToSameNode(RoutingId new_routing_id,
                             SequenceNumber sequence_length) override;
  void StopProxyingToLocalPeer(SequenceNumber sequence_length) override;
  void DecayUnblocked() override;
  void Deactivate() override;
  std::string Describe() const override;
  void LogRouteTrace() override;

 private:
  class SharedState;

  LocalRouterLink(LinkSide side, mem::Ref<SharedState> state);
  ~LocalRouterLink() override;

  const LinkSide side_;
  const mem::Ref<SharedState> state_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_LOCAL_ROUTER_LINK_H_
