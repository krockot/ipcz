// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/remote_router_link.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <utility>
#include <vector>

#include "core/node_link.h"
#include "core/node_messages.h"
#include "core/parcel.h"
#include "core/portal.h"
#include "core/router.h"
#include "core/router_descriptor.h"
#include "core/router_link_state.h"
#include "core/sublink_id.h"
#include "debug/log.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/random.h"

namespace ipcz {
namespace core {

namespace {

constexpr size_t kAuxLinkStateBufferSize = 16384;

}  // namespace

RemoteRouterLink::RemoteRouterLink(mem::Ref<NodeLink> node_link,
                                   SublinkId sublink,
                                   const Fragment& link_state_fragment,
                                   LinkType type,
                                   LinkSide side)
    : node_link_(std::move(node_link)),
      sublink_(sublink),
      type_(type),
      side_(side),
      link_state_fragment_(link_state_fragment) {}

RemoteRouterLink::~RemoteRouterLink() = default;

// static
mem::Ref<RemoteRouterLink> RemoteRouterLink::Create(
    mem::Ref<NodeLink> node_link,
    SublinkId sublink,
    const Fragment& link_state_fragment,
    LinkType type,
    LinkSide side) {
  auto link = mem::WrapRefCounted(new RemoteRouterLink(
      std::move(node_link), sublink, link_state_fragment, type, side));
  if (!link_state_fragment.is_null()) {
    link->link_state_ = link_state_fragment.As<RouterLinkState>();
  } else if (type == LinkType::kCentral && side == LinkSide::kA) {
    // If this link needs a shared RouterLinkState but one could not be provided
    // at construction time, kick off an asynchronous allocation request for
    // more link memory capacity.
    link->must_share_link_state_fragment_ = true;
    link->AllocateLinkState();
  }
  return link;
}

void RemoteRouterLink::SetLinkStateFragment(const Fragment& fragment) {
  ABSL_ASSERT(type_ == LinkType::kCentral);
  RouterLinkState* const state = fragment.As<RouterLinkState>();
  if (!state) {
    node_link()->memory().OnBufferAvailable(
        fragment.buffer_id(), [self = mem::WrapRefCounted(this), fragment] {
          self->SetLinkStateFragment(fragment);
        });
    return;
  }

  LinkStatePhase expected_phase = LinkStatePhase::kNotPresent;
  if (!link_state_phase_.compare_exchange_strong(
          expected_phase, LinkStatePhase::kBusy, std::memory_order_relaxed)) {
    return;
  }

  link_state_fragment_ = fragment;
  RouterLinkState* expected_state = nullptr;
  if (!link_state_.compare_exchange_strong(expected_state, state,
                                           std::memory_order_relaxed)) {
    return;
  }

  expected_phase = LinkStatePhase::kBusy;
  std::atomic_thread_fence(std::memory_order_release);
  bool ok = link_state_phase_.compare_exchange_strong(
      expected_phase, LinkStatePhase::kPresent, std::memory_order_relaxed);
  ABSL_ASSERT(ok);

  if (side_is_stable_.load(std::memory_order_acquire)) {
    MarkSideStable();
  }

  mem::Ref<Router> router = node_link()->GetRouter(sublink_);
  if (router) {
    router->Flush(/*force_bypass_attempt=*/true);
  }
}

LinkType RemoteRouterLink::GetType() const {
  return type_;
}

mem::Ref<Router> RemoteRouterLink::GetLocalTarget() {
  return nullptr;
}

bool RemoteRouterLink::IsRemoteLinkTo(NodeLink& node_link, SublinkId sublink) {
  return node_link_.get() == &node_link && sublink_ == sublink;
}

void RemoteRouterLink::MarkSideStable() {
  side_is_stable_.store(true, std::memory_order_release);
  if (RouterLinkState* state = GetLinkState()) {
    state->SetSideStable(side_);
  }
}

bool RemoteRouterLink::TryLockForBypass(const NodeName& bypass_request_source) {
  RouterLinkState* state = GetLinkState();
  if (!state || !state->TryLock(side_)) {
    return false;
  }

  state->allowed_bypass_request_source = bypass_request_source;
  std::atomic_thread_fence(std::memory_order_release);
  return true;
}

bool RemoteRouterLink::TryLockForClosure() {
  RouterLinkState* state = GetLinkState();
  return state && state->TryLock(side_);
}

void RemoteRouterLink::Unlock() {
  if (RouterLinkState* state = GetLinkState()) {
    state->Unlock(side_);
  }
}

void RemoteRouterLink::FlushOtherSideIfWaiting() {
  RouterLinkState* state = GetLinkState();
  if (!state || !state->ResetWaitingBit(side_.opposite())) {
    return;
  }

  msg::Flush flush;
  flush.params().sublink = sublink_;
  node_link()->Transmit(flush);
}

bool RemoteRouterLink::CanNodeRequestBypass(
    const NodeName& bypass_request_source) {
  RouterLinkState* state = GetLinkState();
  return state && state->is_locked_by(side_.opposite()) &&
         state->allowed_bypass_request_source == bypass_request_source;
}

bool RemoteRouterLink::WouldParcelExceedLimits(size_t data_size,
                                               const IpczPutLimits& limits) {
  // TODO
  return false;
}

void RemoteRouterLink::AcceptParcel(Parcel& parcel) {
  const absl::Span<mem::Ref<Portal>> portals = parcel.portals_view();

  msg::AcceptParcel accept;
  accept.params().sublink = sublink_;
  accept.params().sequence_number = parcel.sequence_number();
  accept.params().parcel_data =
      accept.AllocateArray<uint8_t>(parcel.data_view().size());
  accept.params().new_routers =
      accept.AllocateArray<RouterDescriptor>(portals.size());
  accept.params().os_handles = accept.AppendHandles(parcel.os_handles_view());

  memcpy(accept.GetArrayView<uint8_t>(accept.params().parcel_data).data(),
         parcel.data_view().data(), parcel.data_view().size());

  absl::Span<RouterDescriptor> descriptors =
      accept.GetArrayView<RouterDescriptor>(accept.params().new_routers);
  for (size_t i = 0; i < descriptors.size(); ++i) {
    portals[i]->router()->SerializeNewRouter(*node_link(), descriptors[i]);
  }

  DVLOG(4) << "Transmitting " << parcel.Describe() << " over " << Describe();

  node_link()->Transmit(accept);

  for (size_t i = 0; i < portals.size(); ++i) {
    // It's important to move out references to any transferred portals, because
    // when a parcel is destroyed, it will attempt to close any non-null portals
    // it has. Transferred portals should be forgotten, not closed.
    mem::Ref<Portal> doomed_portal = std::move(portals[i]);
    doomed_portal->router()->BeginProxyingToNewRouter(*node_link(),
                                                      descriptors[i]);
  }
}

void RemoteRouterLink::AcceptRouteClosure(SequenceNumber sequence_length) {
  msg::RouteClosed route_closed;
  route_closed.params().sublink = sublink_;
  route_closed.params().sequence_length = sequence_length;
  node_link()->Transmit(route_closed);
}

void RemoteRouterLink::StopProxying(
    SequenceNumber proxy_inbound_sequence_length,
    SequenceNumber proxy_outbound_sequence_length) {
  msg::StopProxying stop;
  stop.params().sublink = sublink_;
  stop.params().proxy_inbound_sequence_length = proxy_inbound_sequence_length;
  stop.params().proxy_outbound_sequence_length = proxy_outbound_sequence_length;
  node_link()->Transmit(stop);
}

void RemoteRouterLink::RequestProxyBypassInitiation(
    const NodeName& to_new_peer,
    SublinkId proxy_peer_sublink) {
  msg::InitiateProxyBypass request;
  request.params().sublink = sublink_;
  request.params().proxy_peer_name = to_new_peer;
  request.params().proxy_peer_sublink = proxy_peer_sublink;
  node_link()->Transmit(request);
}

void RemoteRouterLink::BypassProxyToSameNode(
    SublinkId new_sublink,
    const Fragment& new_link_state_fragment,
    SequenceNumber proxy_inbound_sequence_length) {
  msg::BypassProxyToSameNode bypass;
  bypass.params().sublink = sublink_;
  bypass.params().new_sublink = new_sublink;
  bypass.params().new_link_state_fragment =
      new_link_state_fragment.descriptor();
  bypass.params().proxy_inbound_sequence_length = proxy_inbound_sequence_length;
  node_link()->Transmit(bypass);
}

void RemoteRouterLink::StopProxyingToLocalPeer(
      SequenceNumber proxy_outbound_sequence_length) {
  msg::StopProxyingToLocalPeer stop;
  stop.params().sublink = sublink_;
  stop.params().proxy_outbound_sequence_length = proxy_outbound_sequence_length;
  node_link()->Transmit(stop);
}

void RemoteRouterLink::ProxyWillStop(
    SequenceNumber proxy_inbound_sequence_length) {
  msg::ProxyWillStop will_stop;
  will_stop.params().sublink = sublink_;
  will_stop.params().proxy_inbound_sequence_length =
      proxy_inbound_sequence_length;
  node_link()->Transmit(will_stop);
}

void RemoteRouterLink::ShareLinkStateMemoryIfNecessary() {
  if (!must_share_link_state_fragment_.load(std::memory_order_relaxed)) {
    return;
  }

  if (link_state_phase_.load(std::memory_order_acquire) !=
      LinkStatePhase::kPresent) {
    return;
  }

  bool expected = true;
  if (!must_share_link_state_fragment_.compare_exchange_strong(
          expected, false, std::memory_order_relaxed)) {
    return;
  }

  msg::SetRouterLinkStateFragment set;
  set.params().sublink = sublink_;
  set.params().descriptor = link_state_fragment_.descriptor();
  node_link()->Transmit(set);
}

void RemoteRouterLink::Deactivate() {
  node_link_->RemoveRemoteRouterLink(sublink_);
}

std::string RemoteRouterLink::Describe() const {
  std::stringstream ss;
  ss << type_.ToString() << " link on "
     << node_link_->local_node_name().ToString() << " to "
     << node_link_->remote_node_name().ToString() << " via sublink " << sublink_
     << " with link state @" << link_state_fragment_.ToString();
  return ss.str();
}

void RemoteRouterLink::LogRouteTrace() {
  msg::LogRouteTrace log_request;
  log_request.params().sublink = sublink_;
  node_link()->Transmit(log_request);
}

void RemoteRouterLink::AllocateLinkState() {
  node_link()->memory().RequestBlockAllocatorCapacity(
      kAuxLinkStateBufferSize, sizeof(RouterLinkState),
      [self = mem::WrapRefCounted(this)]() {
        Fragment fragment =
            self->node_link()->memory().AllocateRouterLinkState();
        if (fragment.is_null()) {
          // We got some new allocator capacity but it's already used up. Try
          // again.
          self->AllocateLinkState();
          return;
        }

        self->SetLinkStateFragment(fragment);
      });
}

RouterLinkState* RemoteRouterLink::GetLinkState() const {
  return link_state_.load(std::memory_order_acquire);
}

}  // namespace core
}  // namespace ipcz
