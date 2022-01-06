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
#include "core/routing_id.h"
#include "debug/log.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/random.h"

namespace ipcz {
namespace core {

RemoteRouterLink::RemoteRouterLink(mem::Ref<NodeLink> node_link,
                                   RoutingId routing_id,
                                   const NodeLinkAddress& link_state_address,
                                   LinkType type,
                                   LinkSide side)
    : node_link_(std::move(node_link)),
      routing_id_(routing_id),
      type_(type),
      side_(side),
      link_state_address_(link_state_address) {}

RemoteRouterLink::~RemoteRouterLink() = default;

// static
mem::Ref<RemoteRouterLink> RemoteRouterLink::Create(
    mem::Ref<NodeLink> node_link,
    RoutingId routing_id,
    const NodeLinkAddress& link_state_address,
    LinkType type,
    LinkSide side) {
  auto link = mem::WrapRefCounted(new RemoteRouterLink(
      std::move(node_link), routing_id, link_state_address, type, side));
  if (!link_state_address.is_null()) {
    link->link_state_ = link->node_link()->memory().GetMapped<RouterLinkState>(
        link_state_address);
  } else if (type == LinkType::kCentral && side == LinkSide::kA) {
    // If this link needs a shared RouterLinkState but one could not be provided
    // at construction time, kick off an asynchronous allocation request for
    // more link memory capacity.
    link->must_share_link_state_address_ = true;
    link->AllocateLinkState();
  }
  return link;
}

void RemoteRouterLink::SetLinkStateAddress(const NodeLinkAddress& address) {
  ABSL_ASSERT(type_ == LinkType::kCentral);
  RouterLinkState* const state =
      node_link()->memory().GetMapped<RouterLinkState>(address);
  if (!state) {
    node_link()->memory().OnBufferAvailable(
        address.buffer_id(), [self = mem::WrapRefCounted(this), address] {
          self->SetLinkStateAddress(address);
        });
    return;
  }

  LinkStatePhase expected_phase = LinkStatePhase::kNotPresent;
  if (!link_state_phase_.compare_exchange_strong(
          expected_phase, LinkStatePhase::kBusy, std::memory_order_relaxed)) {
    return;
  }

  link_state_address_ = address;
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

  if (side_can_support_bypass_.load(std::memory_order_acquire)) {
    SetSideCanSupportBypass();
  }

  mem::Ref<Router> router = node_link()->GetRouter(routing_id_);
  if (router) {
    router->Flush();
  }
}

LinkType RemoteRouterLink::GetType() const {
  return type_;
}

mem::Ref<Router> RemoteRouterLink::GetLocalTarget() {
  return nullptr;
}

bool RemoteRouterLink::IsRemoteLinkTo(NodeLink& node_link,
                                      RoutingId routing_id) {
  return node_link_.get() == &node_link && routing_id_ == routing_id;
}

bool RemoteRouterLink::CanLockForBypass() {
  RouterLinkState* state = GetLinkState();
  return state && state->is_link_ready();
}

bool RemoteRouterLink::SetSideCanSupportBypass() {
  side_can_support_bypass_.store(true, std::memory_order_release);
  RouterLinkState* state = GetLinkState();
  return state && state->SetSideReady(side_);
}

bool RemoteRouterLink::TryToLockForBypass(
    const NodeName& bypass_request_source) {
  RouterLinkState* state = GetLinkState();
  if (!state || !state->TryToLockForBypass(side_)) {
    return false;
  }

  state->allowed_bypass_request_source = bypass_request_source;
  std::atomic_thread_fence(std::memory_order_release);
  return true;
}

bool RemoteRouterLink::CancelBypassLock() {
  RouterLinkState* state = GetLinkState();
  return state && state->CancelBypassLock();
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
  absl::InlinedVector<uint8_t, 256> serialized_data;
  const size_t num_portals = parcel.portals_view().size();
  const size_t num_os_handles = parcel.os_handles_view().size();
  const size_t serialized_size =
      sizeof(msg::AcceptParcel) + IPCZ_ALIGNED(parcel.data_view().size(), 16) +
      num_portals * sizeof(RouterDescriptor) +
      num_os_handles * sizeof(internal::OSHandleData);
  serialized_data.resize(serialized_size);

  auto& accept = *reinterpret_cast<msg::AcceptParcel*>(serialized_data.data());
  accept.message_header.size = sizeof(accept.message_header);
  accept.message_header.message_id = msg::AcceptParcel::kId;
  accept.routing_id = routing_id_;
  accept.sequence_number = parcel.sequence_number();
  accept.num_bytes = parcel.data_view().size();
  accept.num_portals = static_cast<uint32_t>(num_portals);
  accept.num_os_handles = static_cast<uint32_t>(num_os_handles);
  auto* data = reinterpret_cast<uint8_t*>(&accept + 1);
  memcpy(data, parcel.data_view().data(), parcel.data_view().size());
  auto* descriptors = reinterpret_cast<RouterDescriptor*>(
      data + IPCZ_ALIGNED(accept.num_bytes, 16));

  const absl::Span<mem::Ref<Portal>> portals = parcel.portals_view();
  std::vector<os::Handle> os_handles;
  os_handles.reserve(num_os_handles);
  for (size_t i = 0; i < num_portals; ++i) {
    portals[i]->router()->SerializeNewRouter(*node_link(), descriptors[i]);
  }

  DVLOG(4) << "Transmitting " << parcel.Describe() << " over " << Describe();

  node_link()->Transmit(absl::MakeSpan(serialized_data),
                        parcel.os_handles_view());

  for (size_t i = 0; i < num_portals; ++i) {
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
  route_closed.params.routing_id = routing_id_;
  route_closed.params.sequence_length = sequence_length;
  node_link()->Transmit(route_closed);
}

void RemoteRouterLink::StopProxying(
    SequenceNumber proxy_inbound_sequence_length,
    SequenceNumber proxy_outbound_sequence_length) {
  msg::StopProxying stop;
  stop.params.routing_id = routing_id_;
  stop.params.proxy_inbound_sequence_length = proxy_inbound_sequence_length;
  stop.params.proxy_outbound_sequence_length = proxy_outbound_sequence_length;
  node_link()->Transmit(stop);
}

void RemoteRouterLink::RequestProxyBypassInitiation(
    const NodeName& to_new_peer,
    RoutingId proxy_peer_routing_id) {
  msg::InitiateProxyBypass request;
  request.params.routing_id = routing_id_;
  request.params.proxy_peer_name = to_new_peer;
  request.params.proxy_peer_routing_id = proxy_peer_routing_id;
  node_link()->Transmit(request);
}

void RemoteRouterLink::BypassProxyToSameNode(
    RoutingId new_routing_id,
    const NodeLinkAddress& new_link_state_address,
    SequenceNumber proxy_inbound_sequence_length) {
  msg::BypassProxyToSameNode bypass;
  bypass.params.routing_id = routing_id_;
  bypass.params.new_routing_id = new_routing_id;
  bypass.params.new_link_state_address = new_link_state_address;
  bypass.params.proxy_inbound_sequence_length = proxy_inbound_sequence_length;
  node_link()->Transmit(bypass);
}

void RemoteRouterLink::StopProxyingToLocalPeer(
      SequenceNumber proxy_outbound_sequence_length) {
  msg::StopProxyingToLocalPeer stop;
  stop.params.routing_id = routing_id_;
  stop.params.proxy_outbound_sequence_length = proxy_outbound_sequence_length;
  node_link()->Transmit(stop);
}

void RemoteRouterLink::ProxyWillStop(
    SequenceNumber proxy_inbound_sequence_length) {
  msg::ProxyWillStop will_stop;
  will_stop.params.routing_id = routing_id_;
  will_stop.params.proxy_inbound_sequence_length =
      proxy_inbound_sequence_length;
  node_link()->Transmit(will_stop);
}

void RemoteRouterLink::NotifyBypassPossible() {
  msg::NotifyBypassPossible unblocked;
  unblocked.params.routing_id = routing_id_;
  node_link()->Transmit(unblocked);
}

void RemoteRouterLink::Flush() {
  if (!must_share_link_state_address_.load(std::memory_order_relaxed)) {
    return;
  }

  if (link_state_phase_.load(std::memory_order_acquire) !=
      LinkStatePhase::kPresent) {
    return;
  }

  bool expected = true;
  if (!must_share_link_state_address_.compare_exchange_strong(
          expected, false, std::memory_order_relaxed)) {
    return;
  }

  msg::SetRouterLinkStateAddress set;
  set.params.routing_id = routing_id_;
  set.params.address = link_state_address_;
  node_link()->Transmit(set);
}

void RemoteRouterLink::Deactivate() {
  node_link_->RemoveRoute(routing_id_);
}

std::string RemoteRouterLink::Describe() const {
  std::stringstream ss;
  ss << type_.ToString() << " link on "
     << node_link_->local_node_name().ToString() << " to "
     << node_link_->remote_node_name().ToString() << " via routing ID "
     << routing_id_ << " with link state @" << link_state_address_.ToString();
  return ss.str();
}

void RemoteRouterLink::LogRouteTrace() {
  msg::LogRouteTrace log_request;
  log_request.params.routing_id = routing_id_;
  node_link()->Transmit(log_request);
}

void RemoteRouterLink::AllocateLinkState() {
  node_link()->memory().RequestCapacity([self = mem::WrapRefCounted(this)]() {
    NodeLinkAddress address =
        self->node_link()->memory().AllocateRouterLinkState();
    if (address.is_null()) {
      // We got some new link memory capacity but it's already used up. Try
      // again.
      self->AllocateLinkState();
      return;
    }

    self->SetLinkStateAddress(address);
  });
}

RouterLinkState* RemoteRouterLink::GetLinkState() {
  return link_state_.load(std::memory_order_acquire);
}

}  // namespace core
}  // namespace ipcz
