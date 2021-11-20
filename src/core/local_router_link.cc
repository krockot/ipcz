// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/local_router_link.h"

#include <utility>

#include "core/router.h"
#include "core/router_link_state.h"
#include "core/side.h"
#include "mem/ref_counted.h"

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

RouterLinkState& LocalRouterLink::GetLinkState() {
  return state_->state();
}

bool LocalRouterLink::IsLocalLinkTo(Router& router) {
  return state_->side(Opposite(side_)).get() == &router;
}

bool LocalRouterLink::IsRemoteLinkTo(NodeLink& node_link,
                                     RoutingId routing_id) {
  return false;
}

bool LocalRouterLink::WouldParcelExceedLimits(size_t data_size,
                                              const IpczPutLimits& limits) {
  return state_->side(Opposite(side_))
      ->WouldIncomingParcelExceedLimits(data_size, limits);
}

void LocalRouterLink::AcceptParcel(Parcel& parcel) {
  state_->side(Opposite(side_))->AcceptIncomingParcel(parcel);
}

void LocalRouterLink::AcceptRouteClosure(Side side,
                                         SequenceNumber sequence_length) {
  state_->side(Opposite(side_))->AcceptRouteClosure(side, sequence_length);
}

}  // namespace core
}  // namespace ipcz
