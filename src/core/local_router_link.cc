// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/local_router_link.h"

#include <string>
#include <utility>

#include "core/direction.h"
#include "core/link_side.h"
#include "core/link_type.h"
#include "core/router.h"
#include "core/router_link_state.h"
#include "debug/log.h"
#include "mem/ref_counted.h"
#include "util/mutex_locks.h"
#include "util/random.h"

namespace ipcz {
namespace core {

class LocalRouterLink::SharedState : public mem::RefCounted {
 public:
  SharedState(LinkType type,
              LocalRouterLink::InitialState initial_state,
              mem::Ref<Router> router_a,
              mem::Ref<Router> router_b)
      : type_(type),
        router_a_(std::move(router_a)),
        router_b_(std::move(router_b)) {
    if (initial_state == LocalRouterLink::InitialState::kCanDecay) {
      state_.status = RouterLinkState::kReady;
    } else {
      state_.status = RouterLinkState::kNotReady;
    }
  }

  LinkType type() const { return type_; }

  RouterLinkState& state() { return state_; }

  const mem::Ref<Router>& side(LinkSide side) const {
    if (side == LinkSide::kA) {
      return router_a_;
    }
    return router_b_;
  }

 private:
  ~SharedState() override = default;

  const LinkType type_;
  RouterLinkState state_;
  const mem::Ref<Router> router_a_;
  const mem::Ref<Router> router_b_;
};

// static
RouterLink::Pair LocalRouterLink::CreatePair(LinkType type,
                                             InitialState initial_state,
                                             const Router::Pair& routers) {
  auto state = mem::MakeRefCounted<SharedState>(type, initial_state,
                                                routers.first, routers.second);
  return {mem::WrapRefCounted(new LocalRouterLink(LinkSide::kA, state)),
          mem::WrapRefCounted(new LocalRouterLink(LinkSide::kB, state))};
}

LocalRouterLink::LocalRouterLink(LinkSide side, mem::Ref<SharedState> state)
    : side_(side), state_(std::move(state)) {}

LocalRouterLink::~LocalRouterLink() = default;

LinkType LocalRouterLink::GetType() const {
  return state_->type();
}

mem::Ref<Router> LocalRouterLink::GetLocalTarget() {
  return state_->side(side_.opposite());
}

bool LocalRouterLink::IsRemoteLinkTo(NodeLink& node_link,
                                     RoutingId routing_id) {
  return false;
}

bool LocalRouterLink::CanDecay() {
  return state_->state().is_link_ready();
}

bool LocalRouterLink::SetSideCanDecay() {
  return state_->state().SetSideReady(side_);
}

bool LocalRouterLink::MaybeBeginDecay(absl::uint128* bypass_key) {
  bool result = state_->state().TryToDecay(side_);
  if (result && bypass_key) {
    *bypass_key = RandomUint128();
    state_->state().bypass_key = *bypass_key;
  }
  return result;
}

bool LocalRouterLink::CancelDecay() {
  return state_->state().CancelDecay();
}

bool LocalRouterLink::CanBypassWithKey(const absl::uint128& bypass_key) {
  return state_->state().is_decaying(side_.opposite()) &&
         state_->state().bypass_key == bypass_key;
}

bool LocalRouterLink::WouldParcelExceedLimits(size_t data_size,
                                              const IpczPutLimits& limits) {
  return state_->side(side_.opposite())
      ->WouldInboundParcelExceedLimits(data_size, limits);
}

void LocalRouterLink::AcceptParcel(Parcel& parcel) {
  Router& receiver = *state_->side(side_.opposite());
  if (state_->type() == LinkType::kCentral) {
    receiver.AcceptInboundParcel(parcel);
  } else {
    ABSL_ASSERT(state_->type() == LinkType::kBridge);
    receiver.AcceptOutboundParcel(parcel);
  }
}

void LocalRouterLink::AcceptRouteClosure(RouteSide route_side,
                                         SequenceNumber sequence_length) {
  // We flip `route_side` because we're a transverse link.
  state_->side(side_.opposite())
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
  state_->side(side_.opposite())->OnDecayUnblocked();
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
  return side_.ToString() + "-side link to local peer on " +
         side_.opposite().ToString() + " side";
}

void LocalRouterLink::LogRouteTrace() {
  if (state_->type() == LinkType::kCentral) {
    state_->side(side_.opposite())
        ->AcceptLogRouteTraceFrom(Direction::kOutward);
  } else {
    ABSL_ASSERT(state_->type() == LinkType::kBridge);
    state_->side(side_.opposite())->AcceptLogRouteTraceFrom(Direction::kInward);
  }
}

}  // namespace core
}  // namespace ipcz
