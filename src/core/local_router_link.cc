// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/local_router_link.h"

#include <utility>

#include "core/router.h"
#include "core/router_link_state.h"
#include "core/side.h"
#include "mem/ref_counted.h"
#include "util/two_mutex_lock.h"

namespace ipcz {
namespace core {

class LocalRouterLink::SharedState : public mem::RefCounted {
 public:
  SharedState(mem::Ref<Router> left, mem::Ref<Router> right)
      : sides_(std::move(left), std::move(right)) {}

  RouterLinkState& state() { return state_; }
  const mem::Ref<Router>& side(Side side) const { return sides_[side]; }

 private:
  ~SharedState() override = default;

  RouterLinkState state_;
  const TwoSided<mem::Ref<Router>> sides_;
};

// static
std::pair<mem::Ref<LocalRouterLink>, mem::Ref<LocalRouterLink>>
LocalRouterLink::CreatePair(mem::Ref<Router> left_router,
                            mem::Ref<Router> right_router) {
  auto state = mem::MakeRefCounted<SharedState>(std::move(left_router),
                                                std::move(right_router));
  auto left = mem::WrapRefCounted(new LocalRouterLink(Side::kLeft, state));
  auto right =
      mem::WrapRefCounted(new LocalRouterLink(Side::kRight, std::move(state)));
  return {std::move(left), std::move(right)};
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
  return state_->side(Opposite(side_));
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
  return state_->side(Opposite(side_))
      ->WouldInboundParcelExceedLimits(data_size, limits);
}

void LocalRouterLink::AcceptParcel(Parcel& parcel) {
  state_->side(Opposite(side_))->AcceptInboundParcel(parcel);
}

void LocalRouterLink::AcceptRouteClosure(Side side,
                                         SequenceNumber sequence_length) {
  state_->side(Opposite(side_))->AcceptRouteClosure(side, sequence_length);
}

void LocalRouterLink::StopProxying(SequenceNumber inbound_sequence_length,
                                   SequenceNumber outbound_sequence_length) {
  // Local links are never proxying links.
  ABSL_ASSERT(false);
}

}  // namespace core
}  // namespace ipcz
