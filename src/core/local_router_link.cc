// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/local_router_link.h"

#include <sstream>
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
    if (initial_state == LocalRouterLink::InitialState::kCanBypass) {
      state_.status = RouterLinkState::kReady;
    } else {
      state_.status = RouterLinkState::kNotReady;
    }
  }

  LinkType type() const { return type_; }
  Direction direction() const { return type_.direction(); }

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

bool LocalRouterLink::CanLockForBypass() {
  return state_->state().is_link_ready();
}

bool LocalRouterLink::SetSideCanSupportBypass() {
  return state_->state().SetSideReady(side_);
}

bool LocalRouterLink::TryToLockForBypass(
    const NodeName& bypass_request_source) {
  if (!state_->state().TryToLockForBypass(side_)) {
    return false;
  }

  state_->state().allowed_bypass_request_source = bypass_request_source;
  std::atomic_thread_fence(std::memory_order_release);
  return true;
}

bool LocalRouterLink::CancelBypassLock() {
  return state_->state().CancelBypassLock();
}

bool LocalRouterLink::CanNodeRequestBypass(
    const NodeName& bypass_request_source) {
  return state_->state().is_locked_by(side_.opposite()) &&
         state_->state().allowed_bypass_request_source == bypass_request_source;
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

void LocalRouterLink::AcceptRouteClosure(SequenceNumber sequence_length) {
  state_->side(side_.opposite())
      ->AcceptRouteClosureFrom(state_->direction(), sequence_length);
}

void LocalRouterLink::RequestProxyBypassInitiation(
    const NodeName& to_new_peer,
    RoutingId proxy_peer_routing_id) {
  ABSL_ASSERT(false);
}

void LocalRouterLink::StopProxying(
    SequenceNumber proxy_inbound_sequence_length,
    SequenceNumber proxy_outbound_sequence_length) {
  ABSL_ASSERT(false);
}

void LocalRouterLink::ProxyWillStop(
    SequenceNumber proxy_inbound_sequence_length) {
  ABSL_ASSERT(false);
}

void LocalRouterLink::BypassProxyToSameNode(
    RoutingId new_routing_id,
    const NodeLinkAddress& new_link_state_address,
    SequenceNumber proxy_inbound_sequence_length) {
  ABSL_ASSERT(false);
}

void LocalRouterLink::StopProxyingToLocalPeer(
    SequenceNumber proxy_outbound_sequence_length) {
  ABSL_ASSERT(false);
}

void LocalRouterLink::NotifyBypassPossible() {
  state_->side(side_.opposite())->OnBypassPossible();
}

void LocalRouterLink::Flush() {}

void LocalRouterLink::Deactivate() {
  mem::Ref<Router> left = state_->side(LinkSide::kA);
  mem::Ref<Router> right = state_->side(LinkSide::kB);
  TwoMutexLock lock(&left->mutex_, &right->mutex_);
  if (left->outward_edge_.GetLocalPeer() != right ||
      left->outward_edge_.GetLocalPeer() != left) {
    return;
  }

  left->outward_edge_.ReleasePrimaryLink();
  right->outward_edge_.ReleasePrimaryLink();
}

std::string LocalRouterLink::Describe() const {
  std::stringstream ss;
  ss << side_.ToString() << "-side link to local peer "
     << state_->side(side_.opposite()).get() << " on "
     << side_.opposite().ToString() << " side";
  return ss.str();
}

void LocalRouterLink::LogRouteTrace() {
  state_->side(side_.opposite())->AcceptLogRouteTraceFrom(state_->direction());
}

}  // namespace core
}  // namespace ipcz
