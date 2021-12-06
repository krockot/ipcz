// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/local_router_link.h"

#include <string>
#include <utility>

#include "core/router.h"
#include "core/router_link_state.h"
#include "core/side.h"
#include "debug/log.h"
#include "mem/ref_counted.h"
#include "util/two_mutex_lock.h"

namespace ipcz {
namespace core {

class LocalRouterLink::SharedState : public mem::RefCounted {
 public:
  SharedState(RouterLinkState::Status initial_link_status,
              mem::Ref<Router> left,
              mem::Ref<Router> right)
      : sides_(std::move(left), std::move(right)) {
    state_.status = initial_link_status;
  }

  RouterLinkState& state() { return state_; }
  const mem::Ref<Router>& side(Side side) const { return sides_[side]; }

 private:
  ~SharedState() override = default;

  RouterLinkState state_;
  const TwoSided<mem::Ref<Router>> sides_;
};

// static
TwoSided<mem::Ref<RouterLink>> LocalRouterLink::CreatePair(
    RouterLinkState::Status initial_link_status,
    const TwoSided<mem::Ref<Router>>& routers) {
  auto state = mem::MakeRefCounted<SharedState>(
      initial_link_status, routers.left(), routers.right());
  return {mem::WrapRefCounted(new LocalRouterLink(Side::kLeft, state)),
          mem::WrapRefCounted(new LocalRouterLink(Side::kRight, state))};
}

LocalRouterLink::LocalRouterLink(Side side, mem::Ref<SharedState> state)
    : side_(side), state_(std::move(state)) {}

LocalRouterLink::~LocalRouterLink() = default;

void LocalRouterLink::Deactivate() {
  mem::Ref<Router> left = state_->side(Side::kLeft);
  mem::Ref<Router> right = state_->side(Side::kRight);
  TwoMutexLock lock(&left->mutex_, &right->mutex_);
  if (!left->outward_.link || left->outward_.link->GetLocalTarget() != right ||
      !right->outward_.link || right->outward_.link->GetLocalTarget() != left) {
    return;
  }

  left->outward_.link = nullptr;
  right->outward_.link = nullptr;
}

RouterLinkState& LocalRouterLink::GetLinkState() {
  return state_->state();
}

mem::Ref<Router> LocalRouterLink::GetLocalTarget() {
  return state_->side(side_.opposite());
}

bool LocalRouterLink::IsRemoteLinkTo(NodeLink& node_link,
                                     RoutingId routing_id) {
  return false;
}

bool LocalRouterLink::IsLinkToOtherSide() {
  return true;
}

bool LocalRouterLink::WouldParcelExceedLimits(size_t data_size,
                                              const IpczPutLimits& limits) {
  return state_->side(side_.opposite())
      ->WouldInboundParcelExceedLimits(data_size, limits);
}

void LocalRouterLink::AcceptParcel(Parcel& parcel) {
  if (!state_->side(side_.opposite())->AcceptInboundParcel(parcel)) {
    DLOG(ERROR) << "Rejecting unexpected " << parcel.Describe() << " on "
                << Describe();
  }
}

void LocalRouterLink::AcceptRouteClosure(Side side,
                                         SequenceNumber sequence_length) {
  state_->side(side_.opposite())->AcceptRouteClosure(side, sequence_length);
}

void LocalRouterLink::StopProxying(SequenceNumber inbound_sequence_length,
                                   SequenceNumber outbound_sequence_length) {
  // Local links are never proxying links.
  ABSL_ASSERT(false);
}

void LocalRouterLink::RequestProxyBypassInitiation(
    const NodeName& to_new_peer,
    RoutingId proxy_peer_routing_id,
    const absl::uint128& bypass_key) {
  ABSL_ASSERT(false);
}

void LocalRouterLink::BypassProxyToSameNode(RoutingId new_routing_id,
                                            SequenceNumber sequence_length) {
  ABSL_ASSERT(false);
}

void LocalRouterLink::StopProxyingToLocalPeer(SequenceNumber sequence_length) {
  ABSL_ASSERT(false);
}

void LocalRouterLink::ProxyWillStop(SequenceNumber sequence_length) {
  ABSL_ASSERT(false);
}

void LocalRouterLink::DecayUnblocked() {
  state_->side(side_.opposite())->OnDecayUnblocked();
}

std::string LocalRouterLink::Describe() const {
  return side_.ToString() + "-side link to local peer on " +
         side_.opposite().ToString() + " side";
}

void LocalRouterLink::LogRouteTrace(Side toward_side) {
  state_->side(toward_side)->LogRouteTrace(toward_side);
}

}  // namespace core
}  // namespace ipcz
