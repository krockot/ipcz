// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/local_router_link.h"

#include <string>
#include <utility>

#include "core/link_side.h"
#include "core/router.h"
#include "core/router_link_state.h"
#include "debug/log.h"
#include "mem/ref_counted.h"
#include "util/two_mutex_lock.h"

namespace ipcz {
namespace core {

class LocalRouterLink::SharedState : public mem::RefCounted {
 public:
  SharedState(RouterLinkState::Status initial_link_status,
              mem::Ref<Router> router_a,
              mem::Ref<Router> router_b)
      : router_a_(std::move(router_a)), router_b_(std::move(router_b)) {
    state_.status = initial_link_status;
  }

  RouterLinkState& state() { return state_; }
  const mem::Ref<Router>& side(LinkSide side) const {
    if (side == LinkSide::kA) {
      return router_a_;
    }
    return router_b_;
  }

 private:
  ~SharedState() override = default;

  RouterLinkState state_;
  const mem::Ref<Router> router_a_;
  const mem::Ref<Router> router_b_;
};

// static
RouterLink::Pair LocalRouterLink::CreatePair(
    RouterLinkState::Status initial_link_status,
    const Router::Pair& routers) {
  auto state = mem::MakeRefCounted<SharedState>(initial_link_status,
                                                routers.first, routers.second);
  return {mem::WrapRefCounted(new LocalRouterLink(LinkSide::kA, state)),
          mem::WrapRefCounted(new LocalRouterLink(LinkSide::kB, state))};
}

LocalRouterLink::LocalRouterLink(LinkSide link_side,
                                 mem::Ref<SharedState> state)
    : link_side_(link_side), state_(std::move(state)) {}

LocalRouterLink::~LocalRouterLink() = default;

LinkSide LocalRouterLink::GetLinkSide() const {
  return link_side_;
}

RouteSide LocalRouterLink::GetTargetRouteSide() const {
  return RouteSide::kOther;
}

RouterLinkState& LocalRouterLink::GetLinkState() {
  return state_->state();
}

mem::Ref<Router> LocalRouterLink::GetLocalTarget() {
  return state_->side(link_side_.opposite());
}

bool LocalRouterLink::IsRemoteLinkTo(NodeLink& node_link,
                                     RoutingId routing_id) {
  return false;
}

bool LocalRouterLink::WouldParcelExceedLimits(size_t data_size,
                                              const IpczPutLimits& limits) {
  return state_->side(link_side_.opposite())
      ->WouldInboundParcelExceedLimits(data_size, limits);
}

void LocalRouterLink::AcceptParcel(Parcel& parcel) {
  if (!state_->side(link_side_.opposite())->AcceptInboundParcel(parcel)) {
    DLOG(ERROR) << "Rejecting unexpected " << parcel.Describe() << " on "
                << Describe();
  }
}

void LocalRouterLink::AcceptRouteClosure(RouteSide route_side,
                                         SequenceNumber sequence_length) {
  // We flip `route_side` because we're a transverse link.
  state_->side(link_side_.opposite())
      ->AcceptRouteClosure(route_side.opposite(), sequence_length);
}

void LocalRouterLink::RequestProxyBypassInitiation(
    const NodeName& to_new_peer,
    RoutingId proxy_peer_routing_id,
    const absl::uint128& bypass_key) {
  ABSL_ASSERT(false);
}

void LocalRouterLink::StopProxying(SequenceNumber inbound_sequence_length,
                                   SequenceNumber outbound_sequence_length) {
  // Local links are never proxying links.
  ABSL_ASSERT(false);
}

void LocalRouterLink::ProxyWillStop(SequenceNumber sequence_length) {
  ABSL_ASSERT(false);
}

void LocalRouterLink::BypassProxyToSameNode(RoutingId new_routing_id,
                                            SequenceNumber sequence_length) {
  ABSL_ASSERT(false);
}

void LocalRouterLink::StopProxyingToLocalPeer(SequenceNumber sequence_length) {
  ABSL_ASSERT(false);
}

void LocalRouterLink::DecayUnblocked() {
  state_->side(link_side_.opposite())->OnDecayUnblocked();
}

void LocalRouterLink::Deactivate() {
  mem::Ref<Router> left = state_->side(LinkSide::kA);
  mem::Ref<Router> right = state_->side(LinkSide::kB);
  TwoMutexLock lock(&left->mutex_, &right->mutex_);
  if (!left->outward_.link || left->outward_.link->GetLocalTarget() != right ||
      !right->outward_.link || right->outward_.link->GetLocalTarget() != left) {
    return;
  }

  left->outward_.link = nullptr;
  right->outward_.link = nullptr;
}

std::string LocalRouterLink::Describe() const {
  return link_side_.ToString() + "-side link to local peer on " +
         link_side_.opposite().ToString() + " side";
}

void LocalRouterLink::LogRouteTrace(RouteSide toward_route_side) {
  // We flip `toward_route_side` because we're a transverse link.
  state_->side(link_side_.opposite())
      ->LogRouteTrace(toward_route_side.opposite());
}

}  // namespace core
}  // namespace ipcz
