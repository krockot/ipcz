// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/remote_router_link.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <utility>
#include <vector>

#include "ipcz/box.h"
#include "ipcz/fragment_ref.h"
#include "ipcz/handle_descriptor.h"
#include "ipcz/ipcz.h"
#include "ipcz/node_link.h"
#include "ipcz/node_messages.h"
#include "ipcz/parcel.h"
#include "ipcz/portal.h"
#include "ipcz/router.h"
#include "ipcz/router_descriptor.h"
#include "ipcz/router_link_state.h"
#include "ipcz/sublink_id.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/log.h"
#include "util/random.h"
#include "util/ref_counted.h"

namespace ipcz {

RemoteRouterLink::RemoteRouterLink(
    Ref<NodeLink> node_link,
    SublinkId sublink,
    FragmentRef<RouterLinkState> link_state_fragment,
    LinkType type,
    LinkSide side)
    : node_link_(std::move(node_link)),
      sublink_(sublink),
      type_(type),
      side_(side) {
  if (link_state_fragment.is_addressable()) {
    link_state_fragment_ = link_state_fragment;
    link_state_ = link_state_fragment_.get();
  }
}

RemoteRouterLink::~RemoteRouterLink() = default;

// static
Ref<RemoteRouterLink> RemoteRouterLink::Create(
    Ref<NodeLink> node_link,
    SublinkId sublink,
    FragmentRef<RouterLinkState> link_state_fragment,
    LinkType type,
    LinkSide side) {
  auto link = WrapRefCounted(new RemoteRouterLink(
      std::move(node_link), sublink, link_state_fragment, type, side));
  if (link_state_fragment.is_pending() && type.is_central() &&
      side.is_side_b()) {
    link->SetLinkState(std::move(link_state_fragment));
  } else if (link_state_fragment.is_null() && type.is_central() &&
             side.is_side_a()) {
    // If this link needs a shared RouterLinkState but one could not be provided
    // at construction time, kick off an asynchronous allocation request for
    // more link memory capacity.
    link->must_share_link_state_fragment_ = true;
    link->AllocateLinkState();
  }
  return link;
}

void RemoteRouterLink::SetLinkState(FragmentRef<RouterLinkState> state) {
  ABSL_ASSERT(type_.is_central());
  if (state.is_pending()) {
    Ref<NodeLinkMemory> memory = WrapRefCounted(&node_link()->memory());
    FragmentDescriptor descriptor = state.fragment().descriptor();
    memory->OnBufferAvailable(descriptor.buffer_id(), [self =
                                                           WrapRefCounted(this),
                                                       memory, descriptor] {
      self->SetLinkState(memory->AdoptFragmentRef<RouterLinkState>(descriptor));
    });
    return;
  }

  LinkStatePhase expected_phase = LinkStatePhase::kNotPresent;
  if (!link_state_phase_.compare_exchange_strong(
          expected_phase, LinkStatePhase::kBusy, std::memory_order_acquire)) {
    return;
  }

  link_state_fragment_ = std::move(state);
  RouterLinkState* expected_state = nullptr;
  if (!link_state_.compare_exchange_strong(expected_state,
                                           link_state_fragment_.get(),
                                           std::memory_order_release)) {
    return;
  }

  expected_phase = LinkStatePhase::kBusy;
  bool ok = link_state_phase_.compare_exchange_strong(
      expected_phase, LinkStatePhase::kPresent, std::memory_order_release);
  ABSL_ASSERT(ok);

  if (side_is_stable_.load(std::memory_order_relaxed)) {
    MarkSideStable();
  }

  Ref<Router> router = node_link()->GetRouter(sublink_);
  if (router) {
    router->Flush(/*force_bypass_attempt=*/true);
  }
}

LinkType RemoteRouterLink::GetType() const {
  return type_;
}

Ref<Router> RemoteRouterLink::GetLocalTarget() {
  return nullptr;
}

bool RemoteRouterLink::IsRemoteLinkTo(const NodeLink& node_link,
                                      SublinkId sublink) const {
  return node_link_.get() == &node_link && sublink_ == sublink;
}

RouterLinkState::QueueState RemoteRouterLink::GetPeerQueueState() {
  if (RouterLinkState* state = GetLinkState()) {
    return state->GetQueueState(side_.opposite());
  }

  return {.num_inbound_bytes_queued = 0, .num_inbound_parcels_queued = 0};
}

bool RemoteRouterLink::UpdateInboundQueueState(size_t num_bytes,
                                               size_t num_parcels) {
  if (RouterLinkState* state = GetLinkState()) {
    return state->UpdateQueueState(side_, num_bytes, num_parcels);
  }

  return false;
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

  msg::FlushRouter flush;
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
                                               const IpczPutLimits& limits,
                                               size_t* max_data_size) {
  RouterLinkState* state = GetLinkState();
  if (!state) {
    // This is only a best-effort query. If there's no RouterLinkState yet, err
    // on the side of less caution and more data flow.
    return false;
  }

  const RouterLinkState::QueueState peer_queue =
      state->GetQueueState(side_.opposite());
  if (peer_queue.num_inbound_bytes_queued >= limits.max_queued_bytes ||
      peer_queue.num_inbound_parcels_queued >= limits.max_queued_parcels) {
    return true;
  }

  const uint32_t byte_capacity =
      limits.max_queued_bytes - peer_queue.num_inbound_bytes_queued;
  const uint32_t parcel_capacity =
      limits.max_queued_parcels - peer_queue.num_inbound_parcels_queued;
  return data_size > byte_capacity || parcel_capacity == 0;
}

void RemoteRouterLink::AcceptParcel(Parcel& parcel) {
  const absl::Span<Ref<APIObject>> objects = parcel.objects_view();

  msg::AcceptParcel accept;
  accept.params().sublink = sublink_;
  accept.params().sequence_number = parcel.sequence_number();

  size_t num_portals = 0;
  absl::InlinedVector<DriverObject, 2> driver_objects;
  bool driver_objects_will_relay = false;
  for (Ref<APIObject>& object : objects) {
    switch (object->object_type()) {
      case APIObject::kPortal:
        ++num_portals;
        break;

      case APIObject::kBox: {
        Box& box = object->As<Box>();
        if (!box.object().CanTransmitOn(*node_link()->transport())) {
          driver_objects_will_relay = true;
        }
        driver_objects.push_back(std::move(box.object()));
        break;
      }

      default:
        LOG(FATAL) << "Attempted to transmit an invalid object.";
    }
  }

  // If driver objects will require relaying through the broker, we must split
  // this into two messages: one (AcceptParcel) to send directly with just the
  // data and router descriptors, and another (AcceptParcelDriverObjects) to
  // send only the driver objects. These are used by the receiver to reconstruct
  // and accept the full parcel only once both have arrived. This ensures that
  // two important constraints are met:
  //
  //   - a parcel with driver objects, targeting a router recently transmitted
  //     to the receiver, must not be accepted by the receiver until the
  //     receiving router has itself been accepted. Requiring part of the parcel
  //     to go over the direct NodeLink ensures that the complete parcel can't
  //     be received until its destination router has been received.
  //
  //   - a parcel with routers must arrive before any parcel targeting it.
  //     Transmitting the driver objects in a separate message ensures that the
  //     router descriptor transmission itself is not relayed and is thus
  //     ordered with other transmissions on the same link.
  //
  // All of this is in light of the fact that we cannot guarantee any kind of
  // ordering between messages sent directly on this NodeLink vs messages
  // relayed through the broker. The latter may arrive before or after the
  // former, regardless of when either was sent.
  //
  // TODO: It's unfortunate that *every* parcel with driver objects must be
  // split in this way. We could relay a whole parcel as-is IF we knew it had no
  // routers attached (which we can know trivially) AND we knew the receiving
  // router was already bound to the targeted sublink (which we don't.)
  const bool must_split_parcel = driver_objects_will_relay;

  // Allocate all the arrays in the message. Note that each allocation may
  // relocate the parcel data in memory, so views into these arrays should not
  // be acquired until all allocations are complete.
  accept.params().parcel_data =
      accept.AllocateArray<uint8_t>(parcel.data_view().size());
  accept.params().handle_descriptors =
      accept.AllocateArray<HandleDescriptor>(objects.size());
  accept.params().new_routers =
      accept.AllocateArray<RouterDescriptor>(num_portals);

  const absl::Span<uint8_t> parcel_data =
      accept.GetArrayView<uint8_t>(accept.params().parcel_data);
  const absl::Span<HandleDescriptor> handle_descriptors =
      accept.GetArrayView<HandleDescriptor>(accept.params().handle_descriptors);
  const absl::Span<RouterDescriptor> new_routers =
      accept.GetArrayView<RouterDescriptor>(accept.params().new_routers);

  memcpy(parcel_data.data(), parcel.data_view().data(), parcel.data_size());

  absl::InlinedVector<Ref<Router>, 4> routers;
  absl::Span<RouterDescriptor> remaining_routers = new_routers;
  for (size_t i = 0; i < objects.size(); ++i) {
    APIObject& object = *objects[i];
    HandleDescriptor& descriptor = handle_descriptors[i];
    switch (object.object_type()) {
      case APIObject::kPortal: {
        descriptor.type = HandleDescriptor::kPortal;
        ABSL_ASSERT(!remaining_routers.empty());

        Ref<Router> router = object.As<Portal>().router();
        router->SerializeNewRouter(*node_link(), remaining_routers[0]);
        routers.push_back(std::move(router));
        remaining_routers.remove_prefix(1);
        break;
      }

      case APIObject::kBox:
        descriptor.type = must_split_parcel ? HandleDescriptor::kBoxRelayed
                                            : HandleDescriptor::kBox;
        break;

      default:
        ABSL_ASSERT(false);
        break;
    }
  }

  if (must_split_parcel) {
    msg::AcceptParcelDriverObjects accept_objects;
    accept_objects.params().sublink = sublink_;
    accept_objects.params().sequence_number = parcel.sequence_number();
    accept_objects.params().driver_objects =
        accept_objects.AppendDriverObjects(absl::MakeSpan(driver_objects));

    DVLOG(4) << "Transmitting objects for " << parcel.Describe() << " over "
             << Describe();
    node_link()->Transmit(accept_objects);
  } else {
    accept.params().driver_objects =
        accept.AppendDriverObjects(absl::MakeSpan(driver_objects));
  }

  DVLOG(4) << "Transmitting " << parcel.Describe() << " over " << Describe();

  node_link()->Transmit(accept);

  // It's important to release references to any transferred objects, because
  // when a parcel is destroyed it will attempt to close any non-null objects
  // it's still referencing.
  for (Ref<APIObject>& object : objects) {
    Ref<APIObject> released_object = std::move(object);
  }

  // Now that the parcel has been transmitted, it's safe to start proxying from
  // any routers who sent a new successor.
  ABSL_ASSERT(routers.size() == new_routers.size());
  for (size_t i = 0; i < routers.size(); ++i) {
    routers[i]->BeginProxyingToNewRouter(*node_link(), new_routers[i]);
  }
}

void RemoteRouterLink::AcceptRouteClosure(SequenceNumber sequence_length) {
  msg::RouteClosed route_closed;
  route_closed.params().sublink = sublink_;
  route_closed.params().sequence_length = sequence_length;
  node_link()->Transmit(route_closed);
}

void RemoteRouterLink::AcceptRouteDisconnection() {
  msg::RouteDisconnected disconnect;
  disconnect.params().sublink = sublink_;
  node_link()->Transmit(disconnect);
}

void RemoteRouterLink::NotifyDataConsumed() {
  msg::NotifyDataConsumed notify;
  notify.params().sublink = sublink_;
  node_link()->Transmit(notify);
}

bool RemoteRouterLink::SetSignalOnDataConsumed(bool signal) {
  if (RouterLinkState* state = GetLinkState()) {
    return state->SetSignalOnDataConsumedBy(side_.opposite(), signal);
  }

  return false;
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
    FragmentRef<RouterLinkState> new_link_state,
    SequenceNumber proxy_inbound_sequence_length) {
  msg::BypassProxyToSameNode bypass;
  bypass.params().sublink = sublink_;
  bypass.params().new_sublink = new_sublink;
  bypass.params().new_link_state_fragment =
      new_link_state.release().descriptor();
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

  FragmentRef<RouterLinkState> new_state_ref = link_state_fragment_;
  msg::SetRouterLinkStateFragment set;
  set.params().sublink = sublink_;
  set.params().descriptor = new_state_ref.release().descriptor();
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
     << " with link state @" << link_state_fragment_.fragment().ToString();
  return ss.str();
}

void RemoteRouterLink::LogRouteTrace() {
  msg::LogRouteTrace log_request;
  log_request.params().sublink = sublink_;
  node_link()->Transmit(log_request);
}

void RemoteRouterLink::AllocateLinkState() {
  node_link()->memory().AllocateRouterLinkStateAsync(
      [self = WrapRefCounted(this)](FragmentRef<RouterLinkState> state) {
        if (state.is_null()) {
          DLOG(ERROR) << "Unable to allocate RouterLinkState.";
          return;
        }

        ABSL_ASSERT(state.is_addressable());
        self->SetLinkState(std::move(state));
      });
}

RouterLinkState* RemoteRouterLink::GetLinkState() const {
  return link_state_.load(std::memory_order_acquire);
}

}  // namespace ipcz
