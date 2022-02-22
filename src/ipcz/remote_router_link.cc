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
#include "util/os_handle.h"
#include "util/random.h"
#include "util/ref_counted.h"

namespace ipcz {

namespace {

constexpr size_t kAuxLinkStateBufferSize = 16384;

}  // namespace

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
          expected_phase, LinkStatePhase::kBusy, std::memory_order_relaxed)) {
    return;
  }

  link_state_fragment_ = std::move(state);
  RouterLinkState* expected_state = nullptr;
  if (!link_state_.compare_exchange_strong(expected_state,
                                           link_state_fragment_.get(),
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
                                               const IpczPutLimits& limits) {
  // TODO
  return false;
}

void RemoteRouterLink::AcceptParcel(Parcel& parcel) {
  const absl::Span<Ref<APIObject>> objects = parcel.objects_view();

  msg::AcceptParcel accept;
  accept.params().sublink = sublink_;
  accept.params().sequence_number = parcel.sequence_number();

  // The total number of OS handles may be extended by serialized driver
  // objects. This indicates how many OS handles belong to the body of the
  // parcel itself.
  accept.params().num_parcel_os_handles =
      static_cast<uint32_t>(parcel.num_os_handles());

  // The first pass over the parcel is just to compute dimensions of
  // variable-length arrays in the message.
  size_t num_portals = 0;
  size_t num_handle_bytes = 0;
  size_t num_os_handles = parcel.num_os_handles();
  for (Ref<APIObject>& object : objects) {
    switch (object->object_type()) {
      case APIObject::kPortal:
        ++num_portals;
        break;

      case APIObject::kBox: {
        Box& box = object->As<Box>();
        auto dimensions = box.object().GetSerializedDimensions();
        num_handle_bytes += dimensions.num_bytes;
        num_os_handles += dimensions.num_os_handles;
        break;
      }

      default:
        LOG(FATAL) << "Attempted to transmit an invalid object.";
    }
  }

  // Allocate all the arrays in the message. Note that each allocation may
  // relocate the parcel data in memory, so views into these arrays should not
  // be acquired until all allocations are complete.
  accept.params().parcel_data =
      accept.AllocateArray<uint8_t>(parcel.data_view().size());
  accept.params().handle_descriptors =
      accept.AllocateArray<HandleDescriptor>(objects.size());
  accept.params().new_routers =
      accept.AllocateArray<RouterDescriptor>(num_portals);
  accept.params().handle_data = accept.AllocateArray<uint8_t>(num_handle_bytes);

  const absl::Span<uint8_t> parcel_data =
      accept.GetArrayView<uint8_t>(accept.params().parcel_data);
  const absl::Span<HandleDescriptor> handle_descriptors =
      accept.GetArrayView<HandleDescriptor>(accept.params().handle_descriptors);
  const absl::Span<RouterDescriptor> new_routers =
      accept.GetArrayView<RouterDescriptor>(accept.params().new_routers);

  memcpy(parcel_data.data(), parcel.data_view().data(), parcel.data_size());

  absl::InlinedVector<OSHandle, 4> os_handles(num_os_handles);
  ABSL_ASSERT(os_handles.size() >= parcel.num_os_handles());
  for (size_t i = 0; i < parcel.num_os_handles(); ++i) {
    os_handles[i] = std::move(parcel.os_handles_view()[i]);
  }

  absl::Span<uint8_t> remaining_handle_data =
      accept.GetArrayView<uint8_t>(accept.params().handle_data);
  absl::Span<OSHandle> remaining_os_handles =
      absl::MakeSpan(os_handles).subspan(parcel.num_os_handles());

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

      case APIObject::kBox: {
        Box& box = object.As<Box>();
        auto dimensions = box.object().GetSerializedDimensions();

        ABSL_ASSERT(dimensions.num_bytes <= remaining_handle_data.size());
        ABSL_ASSERT(dimensions.num_os_handles <= remaining_os_handles.size());

        descriptor.type = HandleDescriptor::kBox;
        descriptor.num_bytes = dimensions.num_bytes;
        descriptor.num_os_handles = dimensions.num_os_handles;

        IpczResult result = box.object().Serialize(
            remaining_handle_data.first(dimensions.num_bytes),
            remaining_os_handles.first(dimensions.num_os_handles));
        ABSL_ASSERT(result == IPCZ_RESULT_OK);

        remaining_handle_data.remove_prefix(dimensions.num_bytes);
        remaining_os_handles.remove_prefix(dimensions.num_os_handles);
        break;
      }

      default:
        ABSL_ASSERT(false);
        break;
    }
  }

  accept.params().os_handles = accept.AppendHandles(absl::MakeSpan(os_handles));

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
  node_link()->memory().RequestFragmentCapacity(
      kAuxLinkStateBufferSize, sizeof(RouterLinkState),
      [self = WrapRefCounted(this)]() {
        FragmentRef<RouterLinkState> state =
            self->node_link()->memory().AllocateRouterLinkState();
        if (state.is_null()) {
          // We got some new allocator capacity but it's already used up. Try
          // again.
          self->AllocateLinkState();
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
