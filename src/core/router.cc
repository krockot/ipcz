// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/router.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include "core/local_router_link.h"
#include "core/node.h"
#include "core/node_link.h"
#include "core/node_name.h"
#include "core/parcel.h"
#include "core/portal.h"
#include "core/remote_router_link.h"
#include "core/router_descriptor.h"
#include "core/router_link.h"
#include "core/router_link_state.h"
#include "core/router_tracker.h"
#include "core/routing_id.h"
#include "core/sequence_number.h"
#include "core/trap.h"
#include "core/trap_event_dispatcher.h"
#include "debug/log.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "util/mutex_locks.h"
#include "util/random.h"

namespace ipcz {
namespace core {

namespace {

std::string DescribeLink(const mem::Ref<RouterLink>& link) {
  if (!link) {
    return "no link";
  }

  return link->Describe();
}

}  // namespace

Router::Router() {
  RouterTracker::Track(this);
}

Router::~Router() {
  RouterTracker::Untrack(this);
}

bool Router::IsPeerClosed() {
  absl::MutexLock lock(&mutex_);
  return status_.flags & IPCZ_PORTAL_STATUS_PEER_CLOSED;
}

bool Router::IsRouteDead() {
  absl::MutexLock lock(&mutex_);
  return status_.flags & IPCZ_PORTAL_STATUS_DEAD;
}

void Router::QueryStatus(IpczPortalStatus& status) {
  absl::MutexLock lock(&mutex_);
  const size_t size = std::min(status.size, status_.size);
  memcpy(&status, &status_, size);
  status.size = size;
}

bool Router::HasLocalPeer(const mem::Ref<Router>& other) {
  absl::MutexLock lock(&mutex_);
  return outward_edge_.GetLocalPeer() == other;
}

bool Router::WouldOutboundParcelExceedLimits(size_t data_size,
                                             const IpczPutLimits& limits) {
  mem::Ref<RouterLink> link;
  {
    absl::MutexLock lock(&mutex_);
    if (outbound_parcels_.GetNumAvailableParcels() >=
        limits.max_queued_parcels) {
      return true;
    }
    if (outbound_parcels_.GetNumAvailableBytes() > limits.max_queued_bytes) {
      return true;
    }
    const size_t available_capacity =
        limits.max_queued_bytes - outbound_parcels_.GetNumAvailableBytes();
    if (data_size > available_capacity) {
      return true;
    }

    link = outward_edge_.primary_link();
    if (!link) {
      return false;
    }
  }

  return link->WouldParcelExceedLimits(data_size, limits);
}

bool Router::WouldInboundParcelExceedLimits(size_t data_size,
                                            const IpczPutLimits& limits) {
  absl::MutexLock lock(&mutex_);
  if (inbound_parcels_.GetNumAvailableBytes() > limits.max_queued_bytes) {
    return true;
  }

  const size_t available_capacity =
      limits.max_queued_bytes - inbound_parcels_.GetNumAvailableBytes();
  return data_size > available_capacity ||
         inbound_parcels_.GetNumAvailableParcels() >= limits.max_queued_parcels;
}

IpczResult Router::SendOutboundParcel(absl::Span<const uint8_t> data,
                                      Parcel::PortalVector& portals,
                                      std::vector<os::Handle>& os_handles) {
  Parcel parcel;
  parcel.SetData(std::vector<uint8_t>(data.begin(), data.end()));
  parcel.SetPortals(std::move(portals));
  parcel.SetOSHandles(std::move(os_handles));

  {
    absl::MutexLock lock(&mutex_);
    parcel.set_sequence_number(outbound_parcels_.GetCurrentSequenceLength());

    // TODO: pushing and then immediately popping a parcel is a waste of time.
    // optimize this out when we know we're a terminal router.
    DVLOG(4) << "Queuing outbound " << parcel.Describe();

    const bool push_ok = outbound_parcels_.Push(std::move(parcel));
    ABSL_ASSERT(push_ok);
  }

  Flush();
  return IPCZ_RESULT_OK;
}

void Router::CloseRoute() {
  SequenceNumber final_sequence_length;
  mem::Ref<RouterLink> forwarding_link;
  mem::Ref<RouterLink> dead_outward_link;
  {
    absl::MutexLock lock(&mutex_);
    ABSL_ASSERT(!inward_edge_);
    traps_.DisableAllAndClear();

    final_sequence_length = outbound_parcels_.GetCurrentSequenceLength();
    outbound_parcels_.SetFinalSequenceLength(final_sequence_length);

    forwarding_link = outward_edge_.primary_link();
    if (!forwarding_link) {
      return;
    }

    if (outbound_parcels_.IsDead()) {
      dead_outward_link = outward_edge_.ReleasePrimaryLink();
    }
  }

  forwarding_link->AcceptRouteClosure(final_sequence_length);

  if (dead_outward_link) {
    dead_outward_link->Deactivate();
  }
}

IpczResult Router::Merge(mem::Ref<Router> other) {
  if (HasLocalPeer(other) || other == this) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  {
    TwoMutexLock lock(&mutex_, &other->mutex_);
    if (inward_edge_ || other->inward_edge_ || bridge_ || other->bridge_) {
      // It's not legit to call this on non-terminal routers.
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    if (inbound_parcels_.current_sequence_number() > 0 ||
        outbound_parcels_.GetCurrentSequenceLength() > 0 ||
        other->inbound_parcels_.current_sequence_number() > 0 ||
        other->outbound_parcels_.GetCurrentSequenceLength() > 0) {
      return IPCZ_RESULT_FAILED_PRECONDITION;
    }

    bridge_ = std::make_unique<RouteEdge>();
    other->bridge_ = std::make_unique<RouteEdge>();

    RouterLink::Pair links = LocalRouterLink::CreatePair(
        LinkType::kBridge, LocalRouterLink::InitialState::kCannotBypass,
        Router::Pair(mem::WrapRefCounted(this), other));
    bridge_->SetPrimaryLink(std::move(links.first));
    other->bridge_->SetPrimaryLink(std::move(links.second));
  }

  Flush();
  return IPCZ_RESULT_OK;
}

void Router::SetOutwardLink(mem::Ref<RouterLink> link) {
  bool can_lock_link = false;
  bool attempt_removal = false;
  {
    absl::MutexLock lock(&mutex_);
    outward_edge_.SetPrimaryLink(link);
    if (link->GetType().is_central() && outward_edge_.is_stable() &&
        (!inward_edge_ || inward_edge_->is_stable())) {
      outward_edge_.SetPrimaryLinkCanSupportBypass();
      can_lock_link = outward_edge_.CanLockPrimaryLinkForBypass();
      attempt_removal = can_lock_link && inward_edge_;
    }
  }

  Flush();

  if (attempt_removal && MaybeInitiateSelfRemoval()) {
    return;
  }

  if (can_lock_link) {
    // TODO: the other side might not be a proxy, so this notification may be
    // redundant. we should be able to leverage some new shared link state to
    // avoid this redundancy.
    link->NotifyBypassPossible();
  }
}

bool Router::StopProxying(SequenceNumber proxy_inbound_sequence_length,
                          SequenceNumber proxy_outbound_sequence_length) {
  mem::Ref<Router> bridge_peer;
  {
    absl::MutexLock lock(&mutex_);
    if (!outward_edge_.is_decaying()) {
      return false;
    }

    if (bridge_) {
      bridge_peer = bridge_->GetDecayingLocalPeer();
      if (!bridge_peer) {
        return false;
      }
    } else if (!inward_edge_ || !inward_edge_->is_decaying()) {
      return false;
    } else {
      inward_edge_->set_length_to_and_from_decaying_link(
          proxy_inbound_sequence_length, proxy_outbound_sequence_length);
      outward_edge_.set_length_to_and_from_decaying_link(
          proxy_outbound_sequence_length, proxy_inbound_sequence_length);
    }
  }

  if (bridge_peer) {
    TwoMutexLock lock(&mutex_, &bridge_peer->mutex_);
    if (!bridge_ || !bridge_->is_decaying() || !bridge_peer->bridge_ ||
        !bridge_peer->bridge_->is_decaying()) {
      return true;
    }

    outward_edge_.set_length_to_and_from_decaying_link(
        proxy_outbound_sequence_length, proxy_inbound_sequence_length);
    bridge_peer->outward_edge_.set_length_to_and_from_decaying_link(
        proxy_inbound_sequence_length, proxy_outbound_sequence_length);
    bridge_->set_length_to_and_from_decaying_link(
        proxy_inbound_sequence_length, proxy_outbound_sequence_length);
    bridge_peer->bridge_->set_length_to_and_from_decaying_link(
        proxy_outbound_sequence_length, proxy_inbound_sequence_length);
  }

  Flush();
  if (bridge_peer) {
    bridge_peer->Flush();
  }
  return true;
}

bool Router::AcceptInboundParcel(Parcel& parcel) {
  TrapEventDispatcher dispatcher;
  {
    absl::MutexLock lock(&mutex_);
    if (!inbound_parcels_.Push(std::move(parcel))) {
      return false;
    }

    if (!inward_edge_) {
      status_.num_local_parcels = inbound_parcels_.GetNumAvailableParcels();
      status_.num_local_bytes = inbound_parcels_.GetNumAvailableBytes();
      traps_.UpdatePortalStatus(status_, dispatcher);
    }
  }

  Flush();
  return true;
}

bool Router::AcceptOutboundParcel(Parcel& parcel) {
  {
    absl::MutexLock lock(&mutex_);

    // Proxied outbound parcels are always queued in a ParcelQueue even if they
    // will be forwarded immediately. This allows us to track the full sequence
    // of forwarded parcels so we can know with certainty when we're done
    // forwarding.
    //
    // TODO: Using a queue here may increase latency along the route, because it
    // it unnecessarily forces in-order forwarding. We could use an unordered
    // queue for forwarding, but we'd still need some lighter-weight abstraction
    // that tracks complete sequences from potentially fragmented contributions.
    if (!outbound_parcels_.Push(std::move(parcel))) {
      return false;
    }
  }

  Flush();
  return true;
}

void Router::AcceptRouteClosureFrom(Direction source,
                                    SequenceNumber sequence_length) {
  TrapEventDispatcher dispatcher;
  mem::Ref<RouterLink> forwarding_link;
  mem::Ref<RouterLink> dead_primary_link;
  mem::Ref<RouterLink> dead_decaying_link;
  mem::Ref<RouterLink> bridge_link;
  if (source.is_inward()) {
    // If we're being notified of our own side's closure, we want to propagate
    // this outward toward the other side.
    absl::MutexLock lock(&mutex_);
    outbound_parcels_.SetFinalSequenceLength(sequence_length);
    forwarding_link = outward_edge_.GetLinkToPropagateRouteClosure();
    if (!outbound_parcels_.IsExpectingMoreParcels()) {
      dead_primary_link = outward_edge_.ReleasePrimaryLink();
      dead_decaying_link = outward_edge_.ReleaseDecayingLink();
    }

    // If we're receiving this, it's coming from the other side of the bridge
    // which is already reset by now.
    bridge_.reset();
  } else {
    // We're being notified of the other side's closure, so we want to propagate
    // this inward toward our own terminal router. If that's us, update portal
    // status and traps.
    absl::MutexLock lock(&mutex_);
    inbound_parcels_.SetFinalSequenceLength(sequence_length);
    if (inward_edge_) {
      forwarding_link = inward_edge_->GetLinkToPropagateRouteClosure();
    } else if (bridge_) {
      forwarding_link = bridge_->GetLinkToPropagateRouteClosure();
      dead_primary_link = bridge_->ReleasePrimaryLink();
      dead_decaying_link = bridge_->ReleaseDecayingLink();
    } else {
      status_.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
      if (inbound_parcels_.IsDead()) {
        status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
      }
      traps_.UpdatePortalStatus(status_, dispatcher);

      if (!inbound_parcels_.IsExpectingMoreParcels()) {
        // We can drop our outward link if we know there are no more in-flight
        // parcels coming our way. Otherwise it'll be dropped as soon as that's
        // the case.
        dead_primary_link = outward_edge_.ReleasePrimaryLink();
        dead_decaying_link = outward_edge_.ReleaseDecayingLink();
      }
    }
  }

  if (forwarding_link) {
    forwarding_link->AcceptRouteClosure(sequence_length);
  }

  if (dead_primary_link) {
    dead_primary_link->Deactivate();
  }

  if (dead_decaying_link) {
    dead_decaying_link->Deactivate();
  }

  Flush();
}

IpczResult Router::GetNextIncomingParcel(void* data,
                                         uint32_t* num_bytes,
                                         IpczHandle* portals,
                                         uint32_t* num_portals,
                                         IpczOSHandle* os_handles,
                                         uint32_t* num_os_handles) {
  TrapEventDispatcher dispatcher;
  absl::MutexLock lock(&mutex_);
  if (inward_edge_) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (!inbound_parcels_.HasNextParcel()) {
    if (inbound_parcels_.IsDead()) {
      return IPCZ_RESULT_NOT_FOUND;
    }
    return IPCZ_RESULT_UNAVAILABLE;
  }

  Parcel& p = inbound_parcels_.NextParcel();
  const uint32_t data_capacity = num_bytes ? *num_bytes : 0;
  const uint32_t data_size = static_cast<uint32_t>(p.data_view().size());
  const uint32_t portals_capacity = num_portals ? *num_portals : 0;
  const uint32_t portals_size = static_cast<uint32_t>(p.portals_view().size());
  const uint32_t os_handles_capacity = num_os_handles ? *num_os_handles : 0;
  const uint32_t os_handles_size =
      static_cast<uint32_t>(p.os_handles_view().size());
  if (num_bytes) {
    *num_bytes = data_size;
  }
  if (num_portals) {
    *num_portals = portals_size;
  }
  if (num_os_handles) {
    *num_os_handles = os_handles_size;
  }
  if (data_capacity < data_size || portals_capacity < portals_size ||
      os_handles_capacity < os_handles_size) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  }

  Parcel parcel;
  inbound_parcels_.Pop(parcel);
  memcpy(data, parcel.data_view().data(), parcel.data_view().size());
  parcel.Consume(portals, os_handles);

  status_.num_local_parcels = inbound_parcels_.GetNumAvailableParcels();
  status_.num_local_bytes = inbound_parcels_.GetNumAvailableBytes();
  if (inbound_parcels_.IsDead()) {
    status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
    traps_.UpdatePortalStatus(status_, dispatcher);
  }

  return IPCZ_RESULT_OK;
}

IpczResult Router::BeginGetNextIncomingParcel(const void** data,
                                              uint32_t* num_data_bytes,
                                              uint32_t* num_portals,
                                              uint32_t* num_os_handles) {
  absl::MutexLock lock(&mutex_);
  if (inward_edge_) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (!inbound_parcels_.HasNextParcel()) {
    return IPCZ_RESULT_UNAVAILABLE;
  }

  Parcel& p = inbound_parcels_.NextParcel();
  const uint32_t data_size = static_cast<uint32_t>(p.data_view().size());
  const uint32_t portals_size = static_cast<uint32_t>(p.portals_view().size());
  const uint32_t os_handles_size =
      static_cast<uint32_t>(p.os_handles_view().size());
  if (num_data_bytes) {
    *num_data_bytes = data_size;
  } else if (data_size) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  }

  if (num_portals) {
    *num_portals = portals_size;
  }

  if (num_os_handles) {
    *num_os_handles = os_handles_size;
  }

  return IPCZ_RESULT_OK;
}

IpczResult Router::CommitGetNextIncomingParcel(uint32_t num_data_bytes_consumed,
                                               IpczHandle* portals,
                                               uint32_t* num_portals,
                                               IpczOSHandle* os_handles,
                                               uint32_t* num_os_handles) {
  TrapEventDispatcher dispatcher;
  absl::MutexLock lock(&mutex_);
  if (inward_edge_) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (!inbound_parcels_.HasNextParcel()) {
    // If ipcz is used correctly this is impossible.
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  Parcel& p = inbound_parcels_.NextParcel();
  const uint32_t portals_capacity = num_portals ? *num_portals : 0;
  const uint32_t os_handles_capacity = num_os_handles ? *num_os_handles : 0;
  if (num_portals) {
    *num_portals = static_cast<uint32_t>(p.portals_view().size());
  }
  if (num_os_handles) {
    *num_os_handles = static_cast<uint32_t>(p.os_handles_view().size());
  }
  if (portals_capacity < p.portals_view().size() ||
      os_handles_capacity < p.os_handles_view().size()) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  }

  if (num_data_bytes_consumed > p.data_view().size()) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (num_data_bytes_consumed < p.data_view().size()) {
    p.ConsumePartial(num_data_bytes_consumed, portals, os_handles);
  } else {
    p.Consume(portals, os_handles);
  }

  Parcel consumed_parcel;
  bool ok = inbound_parcels_.Pop(consumed_parcel);
  ABSL_ASSERT(ok);

  status_.num_local_parcels = inbound_parcels_.GetNumAvailableParcels();
  status_.num_local_bytes = inbound_parcels_.GetNumAvailableBytes();
  if (inbound_parcels_.IsDead()) {
    status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
    traps_.UpdatePortalStatus(status_, dispatcher);
  }
  return IPCZ_RESULT_OK;
}

void Router::AddTrap(mem::Ref<Trap> trap) {
  absl::MutexLock lock(&mutex_);
  traps_.Add(std::move(trap));
}

void Router::RemoveTrap(Trap& trap) {
  absl::MutexLock lock(&mutex_);
  traps_.Remove(trap);
}

void Router::SerializeNewRouter(NodeLink& to_node_link,
                                RouterDescriptor& descriptor) {
  mem::Ref<Router> local_peer;
  bool initiate_proxy_bypass = false;
  {
    absl::MutexLock lock(&mutex_);
    traps_.DisableAllAndClear();
    local_peer = outward_edge_.GetLocalPeer();
    initiate_proxy_bypass = outward_edge_.TryToLockPrimaryLinkForBypass(
        to_node_link.remote_node_name());
  }

  if (local_peer && initiate_proxy_bypass &&
      SerializeNewRouterWithLocalPeer(to_node_link, descriptor, local_peer)) {
    return;
  }

  SerializeNewRouterAndConfigureProxy(to_node_link, descriptor,
                                      initiate_proxy_bypass);
}

void Router::BeginProxyingToNewRouter(NodeLink& to_node_link,
                                      const RouterDescriptor& descriptor) {
  const absl::optional<NodeLink::Route> new_route =
      to_node_link.GetRoute(descriptor.new_routing_id);
  if (!new_route) {
    // The route's been torn down, presumably because of node disconnection.
    return;
  }

  const absl::optional<NodeLink::Route> new_decaying_route =
      to_node_link.GetRoute(descriptor.new_decaying_routing_id);

  mem::Ref<Router> local_peer;
  {
    absl::MutexLock lock(&mutex_);
    ABSL_ASSERT(inward_edge_);
    if (descriptor.proxy_already_bypassed) {
      mem::Ref<RouterLink> local_peer_link = outward_edge_.ReleasePrimaryLink();
      local_peer = local_peer_link->GetLocalTarget();
      ABSL_ASSERT(local_peer);
      ABSL_ASSERT(new_decaying_route);
      inward_edge_->SetPrimaryLink(new_decaying_route->link);
    } else {
      inward_edge_->SetPrimaryLink(new_route->link);
    }
  }

  if (local_peer) {
    local_peer->SetOutwardLink(new_route->link);
  }

  Flush();
}

// static
mem::Ref<Router> Router::Deserialize(const RouterDescriptor& descriptor,
                                     NodeLink& from_node_link) {
  auto router = mem::MakeRefCounted<Router>();
  {
    absl::MutexLock lock(&router->mutex_);
    router->outbound_parcels_.ResetInitialSequenceNumber(
        descriptor.next_outgoing_sequence_number);
    router->inbound_parcels_.ResetInitialSequenceNumber(
        descriptor.next_incoming_sequence_number);
    if (descriptor.peer_closed) {
      router->status_.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
      router->inbound_parcels_.SetFinalSequenceLength(
          descriptor.closed_peer_sequence_length);
      if (router->inbound_parcels_.IsDead()) {
        router->status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
      }
    }

    if (descriptor.proxy_already_bypassed) {
      // When split from a local peer, our remote counterpart (our remote peer's
      // former local peer) will use this link to forward parcels it already
      // received from our peer. This link decays like any other decaying link
      // once its usefulness has expired.
      //
      // The sequence length toward this link is the current outbound sequence
      // length, which is to say, we will not be sending any parcels that way.
      // The sequence length from the link is whatever had already been sent
      // to our counterpart back on the peer's node.
      router->outward_edge_.SetPrimaryLink(from_node_link.AddRoute(
          descriptor.new_decaying_routing_id, kNullNodeLinkAddress,
          LinkType::kPeripheralOutward, LinkSide::kB, router));
      router->outward_edge_.StartDecaying(
          router->outbound_parcels_.current_sequence_number(),
          descriptor.decaying_incoming_sequence_length > 0
              ? descriptor.decaying_incoming_sequence_length
              : descriptor.next_incoming_sequence_number);

      router->outward_edge_.SetPrimaryLink(from_node_link.AddRoute(
          descriptor.new_routing_id, descriptor.new_link_state_address,
          LinkType::kCentral, LinkSide::kB, router));

      DVLOG(4) << "Route moved from split pair on "
               << from_node_link.remote_node_name().ToString() << " to "
               << from_node_link.local_node_name().ToString()
               << " via routing ID " << descriptor.new_routing_id
               << " and decaying routing ID "
               << descriptor.new_decaying_routing_id;
    } else {
      router->outward_edge_.SetPrimaryLink(from_node_link.AddRoute(
          descriptor.new_routing_id, kNullNodeLinkAddress,
          LinkType::kPeripheralOutward, LinkSide::kB, router));

      DVLOG(4) << "Route extended from "
               << from_node_link.remote_node_name().ToString() << " to "
               << from_node_link.local_node_name().ToString()
               << " via routing ID " << descriptor.new_routing_id;
    }
  }

  if (descriptor.proxy_peer_node_name.is_valid()) {
    // Our predecessor has given us the means to initiate its own bypass.
    router->InitiateProxyBypass(from_node_link, descriptor.new_routing_id,
                                descriptor.proxy_peer_node_name,
                                descriptor.proxy_peer_routing_id);
  }

  router->Flush();
  return router;
}

bool Router::InitiateProxyBypass(NodeLink& requesting_node_link,
                                 RoutingId requesting_routing_id,
                                 const NodeName& proxy_peer_node_name,
                                 RoutingId proxy_peer_routing_id) {
  {
    absl::MutexLock lock(&mutex_);
    if (!outward_edge_.primary_link()) {
      // Must have been disconnected already.
      return true;
    }

    if (!outward_edge_.primary_link()->IsRemoteLinkTo(requesting_node_link,
                                                      requesting_routing_id)) {
      // Authenticate that the request to bypass our outward peer is actually
      // coming from our outward peer.
      DLOG(ERROR) << "Rejecting InitiateProxyBypass from "
                  << requesting_node_link.remote_node_name().ToString()
                  << " on routing ID " << requesting_routing_id
                  << " with existing outward "
                  << DescribeLink(outward_edge_.primary_link());
      return false;
    }
  }

  if (proxy_peer_node_name != requesting_node_link.local_node_name()) {
    // Common case: the proxy's outward peer is NOT on the same node as we are.
    // In this case we send a BypassProxy request to that node, which may
    // require an introduction first.
    SequenceNumber proxy_outbound_sequence_length;
    {
      // Begin decaying our outward link. The sequence length to this link is
      // trivially known here, and the sequence length expected *from* the link
      // will be passed to us soon in a ProxyWillStop message.
      absl::MutexLock lock(&mutex_);
      proxy_outbound_sequence_length =
          outbound_parcels_.current_sequence_number();
      if (!outward_edge_.StartDecaying(proxy_outbound_sequence_length)) {
        return false;
      }
    }

    mem::Ref<NodeLink> new_peer_node =
        requesting_node_link.node()->GetLink(proxy_peer_node_name);
    if (new_peer_node) {
      new_peer_node->BypassProxy(
          requesting_node_link.remote_node_name(), proxy_peer_routing_id,
          proxy_outbound_sequence_length, mem::WrapRefCounted(this));
      return true;
    }

    requesting_node_link.node()->EstablishLink(
        proxy_peer_node_name,
        [requesting_node_name = requesting_node_link.remote_node_name(),
         proxy_peer_routing_id, proxy_outbound_sequence_length,
         self = mem::WrapRefCounted(this)](NodeLink* new_link) {
          if (!new_link) {
            // TODO: failure to connect to a node here should result in route
            // destruction. This is not the same as closure since we can't
            // guarantee any sequence length.
            return;
          }

          new_link->BypassProxy(requesting_node_name, proxy_peer_routing_id,
                                proxy_outbound_sequence_length, self);
        });
    return true;
  }

  // The proxy's outward peer lives on the same node as this router, so we can
  // skip some messaging and locally link the two routers together right now.

  mem::Ref<Router> new_local_peer =
      requesting_node_link.GetRouter(proxy_peer_routing_id);
  mem::Ref<RouterLink> previous_outward_link_from_new_local_peer;
  SequenceNumber proxy_inbound_sequence_length;
  SequenceNumber proxy_outbound_sequence_length;
  if (!new_local_peer) {
    // The peer may have been explicitly closed by the time this message
    // arrives.
    return true;
  }

  {
    TwoMutexLock lock(&mutex_, &new_local_peer->mutex_);
    proxy_inbound_sequence_length =
        new_local_peer->outbound_parcels_.current_sequence_number();
    proxy_outbound_sequence_length =
        outbound_parcels_.current_sequence_number();

    DVLOG(4) << "Initiating proxy bypass with new local peer on "
             << proxy_peer_node_name.ToString() << " and proxy links to "
             << requesting_node_link.remote_node_name().ToString()
             << " on routing IDs " << proxy_peer_routing_id << " and "
             << requesting_routing_id << "; inbound length "
             << proxy_inbound_sequence_length << " and outbound length "
             << proxy_outbound_sequence_length;

    // We can immediately begin decaying our own link to the proxy, and the
    // link's sequence length in both directions is already known.
    if (!outward_edge_.StartDecaying(proxy_outbound_sequence_length,
                                     proxy_inbound_sequence_length)) {
      return false;
    }

    // Our new local peer gets a decaying outward link to the proxy, only to
    // forward outbound parcels already expected by the proxy.
    previous_outward_link_from_new_local_peer =
        new_local_peer->outward_edge_.primary_link();
    if (!new_local_peer->outward_edge_.StartDecaying(
            proxy_inbound_sequence_length,
            proxy_outbound_sequence_length)) {
      return false;
    }

    // Finally, create a new LocalRouterLink and use it to replace both our
    // own outward link and our new local peer's outward link. The new link is
    // not ready to support bypass until all the above links have fully decayed.
    RouterLink::Pair links = LocalRouterLink::CreatePair(
        LinkType::kCentral, LocalRouterLink::InitialState::kCannotBypass,
        Router::Pair(mem::WrapRefCounted(this), new_local_peer));
    outward_edge_.SetPrimaryLink(std::move(links.first));
    new_local_peer->outward_edge_.SetPrimaryLink(std::move(links.second));
  }

  if (previous_outward_link_from_new_local_peer) {
    previous_outward_link_from_new_local_peer->StopProxying(
        proxy_inbound_sequence_length, proxy_outbound_sequence_length);
  } else {
    // TODO: The local peer must have been closed. Tear down the route.
  }

  Flush();
  new_local_peer->Flush();
  return true;
}

bool Router::BypassProxyWithNewRemoteLink(
    mem::Ref<RemoteRouterLink> new_peer,
    SequenceNumber proxy_outbound_sequence_length) {
  SequenceNumber proxy_inbound_sequence_length;
  mem::Ref<RouterLink> decaying_outward_link_to_proxy;
  {
    absl::MutexLock lock(&mutex_);
    if (!outward_edge_.primary_link()) {
      // TODO: terminate the route. not the same as closure.
      return true;
    }

    if (!outward_edge_.CanNodeRequestBypassOfPrimaryLink(
            new_peer->node_link()->remote_node_name())) {
      return false;
    }

    proxy_inbound_sequence_length = outbound_parcels_.current_sequence_number();

    RemoteRouterLink& remote_proxy =
        static_cast<RemoteRouterLink&>(*outward_edge_.primary_link());
    RemoteRouterLink& remote_peer = static_cast<RemoteRouterLink&>(*new_peer);
    const mem::Ref<NodeLink> node_link_to_proxy = remote_proxy.node_link();
    const mem::Ref<NodeLink> node_link_to_peer = remote_peer.node_link();
    DVLOG(4) << "Bypassing proxy at "
             << node_link_to_proxy->remote_node_name().ToString()
             << " on routing ID " << remote_proxy.routing_id() << " from "
             << node_link_to_proxy->local_node_name().ToString()
             << " with new link to "
             << node_link_to_peer->remote_node_name().ToString()
             << " on routing ID " << remote_peer.routing_id()
             << "; inbound sequence length " << proxy_inbound_sequence_length
             << " and outbound sequence length "
             << proxy_outbound_sequence_length;

    decaying_outward_link_to_proxy = outward_edge_.primary_link();
    if (!outward_edge_.StartDecaying(proxy_inbound_sequence_length,
                                     proxy_outbound_sequence_length)) {
      return false;
    }
    outward_edge_.SetPrimaryLink(new_peer);
  }

  decaying_outward_link_to_proxy->StopProxying(proxy_inbound_sequence_length,
                                               proxy_outbound_sequence_length);
  new_peer->ProxyWillStop(proxy_inbound_sequence_length);

  Flush();
  return true;
}

bool Router::BypassProxyWithNewLinkToSameNode(
    mem::Ref<RouterLink> new_peer,
    SequenceNumber proxy_inbound_sequence_length) {
  mem::Ref<RouterLink> decaying_proxy;
  SequenceNumber proxy_outbound_sequence_length;
  {
    absl::MutexLock lock(&mutex_);
    if (!outward_edge_.primary_link()) {
      // Not necessarily a validation failure if the link was severed due to a
      // remote crash.
      //
      // TODO: propagate route teardown?
      return true;
    }

    if (outward_edge_.GetLocalPeer()) {
      // Bogus request, we obviously don't have a link to a proxy on `new_peer`s
      // node, because our outward link is local.
      return false;
    }

    RemoteRouterLink& old_remote_peer =
        static_cast<RemoteRouterLink&>(*outward_edge_.primary_link());
    RemoteRouterLink& new_remote_peer =
        static_cast<RemoteRouterLink&>(*new_peer);
    const mem::Ref<NodeLink> remote_node_link = old_remote_peer.node_link();
    if (new_remote_peer.node_link() != remote_node_link) {
      // Bogus request: our outward link does not go to the same node as
      // `new_peer`.
      return false;
    }

    DVLOG(4) << "Bypassing proxy at "
             << remote_node_link->remote_node_name().ToString()
             << " on routing ID " << old_remote_peer.routing_id() << " from "
             << remote_node_link->local_node_name().ToString()
             << " with new routing ID " << new_remote_peer.routing_id();

    proxy_outbound_sequence_length =
        outbound_parcels_.current_sequence_number();

    decaying_proxy = outward_edge_.primary_link();
    if (!outward_edge_.StartDecaying(proxy_outbound_sequence_length,
                                     proxy_inbound_sequence_length)) {
      return false;
    }
    outward_edge_.SetPrimaryLink(std::move(new_peer));
  }

  ABSL_ASSERT(decaying_proxy);
  decaying_proxy->StopProxyingToLocalPeer(proxy_outbound_sequence_length);
  return true;
}

bool Router::StopProxyingToLocalPeer(
    SequenceNumber proxy_outbound_sequence_length) {
  mem::Ref<Router> local_peer;
  mem::Ref<Router> bridge_peer;
  {
    absl::MutexLock lock(&mutex_);
    if (bridge_) {
      bridge_peer = bridge_->GetDecayingLocalPeer();
    } else if (outward_edge_.is_decaying()) {
      local_peer = outward_edge_.GetDecayingLocalPeer();
    } else {
      return false;
    }
  }

  if (local_peer && !bridge_peer) {
    // Typical case, where no bridge link is present.
    TwoMutexLock lock(&mutex_, &local_peer->mutex_);
    if (local_peer->outward_edge_.is_stable() || !outward_edge_.is_decaying() ||
        !inward_edge_ || !inward_edge_->is_decaying()) {
      return false;
    }

    DVLOG(4) << "Stopping proxy with inward decaying "
             << DescribeLink(inward_edge_->decaying_link()) << " and outward "
             << "decaying " << DescribeLink(outward_edge_.decaying_link());

    local_peer->outward_edge_.set_length_from_decaying_link(
        proxy_outbound_sequence_length);
    outward_edge_.set_length_to_decaying_link(proxy_outbound_sequence_length);
    inward_edge_->set_length_from_decaying_link(proxy_outbound_sequence_length);
  } else if (bridge_peer) {
    // Here the proxy is actually a pair of bridge routers linking two routes
    // together as a result of a prior Merge() operation. Just means we have to
    // update three routers instead of two.
    {
      absl::MutexLock lock(&bridge_peer->mutex_);
      if (!bridge_peer->outward_edge_.is_decaying()) {
        return false;
      }
      local_peer = bridge_peer->outward_edge_.GetDecayingLocalPeer();
      if (!local_peer) {
        return false;
      }
    }

    ThreeMutexLock lock(&mutex_, &local_peer->mutex_, &bridge_peer->mutex_);
    if (local_peer->outward_edge_.is_stable() || !outward_edge_.is_decaying() ||
        !bridge_peer->outward_edge_.is_decaying()) {
      return false;
    }

    local_peer->outward_edge_.set_length_from_decaying_link(
        proxy_outbound_sequence_length);
    outward_edge_.set_length_from_decaying_link(proxy_outbound_sequence_length);
    bridge_->set_length_to_decaying_link(proxy_outbound_sequence_length);
    bridge_peer->outward_edge_.set_length_to_decaying_link(
        proxy_outbound_sequence_length);
    bridge_peer->bridge_->set_length_from_decaying_link(
        proxy_outbound_sequence_length);
  } else {
    return false;
  }

  Flush();
  local_peer->Flush();
  if (bridge_peer) {
    bridge_peer->Flush();
  }
  return true;
}

bool Router::OnProxyWillStop(SequenceNumber proxy_inbound_sequence_length) {
  {
    absl::MutexLock lock(&mutex_);
    if (outward_edge_.is_stable()) {
      return true;
    }

    DVLOG(4) << "Bypassed proxy has finalized its inbound sequence length at "
             << proxy_inbound_sequence_length << " for "
             << DescribeLink(outward_edge_.decaying_link());

    outward_edge_.set_length_from_decaying_link(proxy_inbound_sequence_length);
  }

  Flush();
  return true;
}

bool Router::OnBypassPossible() {
  MaybeInitiateSelfRemoval();
  return true;
}

void Router::LogDescription() {
  absl::MutexLock lock(&mutex_);
  DLOG(INFO) << "## router [" << this << "]";
  DLOG(INFO) << " - status flags: " << status_.flags;
  DLOG(INFO) << " - outward edge:";
  outward_edge_.LogDescription();
  if (inward_edge_) {
    DLOG(INFO) << " - inward edge:";
    inward_edge_->LogDescription();
  } else {
    DLOG(INFO) << " - no inward edge";
  }
  if (bridge_) {
    DLOG(INFO) << " - bridge:";
    bridge_->LogDescription();
  } else {
    DLOG(INFO) << " - no bridge";
  }
}

void Router::LogRouteTrace() {
  LogDescription();

  mem::Ref<RouterLink> next_link;
  {
    absl::MutexLock lock(&mutex_);
    next_link = outward_edge_.primary_link();
  }
  if (next_link) {
    next_link->LogRouteTrace();
  }
}

void Router::AcceptLogRouteTraceFrom(Direction source) {
  LogDescription();

  mem::Ref<RouterLink> next_link;
  {
    absl::MutexLock lock(&mutex_);
    if (source.is_outward()) {
      next_link = bridge_->primary_link() ? bridge_->primary_link()
                                          : bridge_->decaying_link();
    } else {
      next_link = outward_edge_.primary_link();
    }
  }
  if (next_link) {
    next_link->LogRouteTrace();
  }
}

void Router::Flush() {
  mem::Ref<RouterLink> inward_link;
  mem::Ref<RouterLink> outward_link;
  mem::Ref<RouterLink> decaying_inward_link;
  mem::Ref<RouterLink> decaying_outward_link;
  mem::Ref<RouterLink> inward_link_for_closure_propagation;
  mem::Ref<RouterLink> outward_link_for_closure_propagation;
  mem::Ref<RouterLink> dead_inward_link;
  mem::Ref<RouterLink> dead_outward_link;
  mem::Ref<RouterLink> bridge_link;
  absl::InlinedVector<Parcel, 2> outbound_parcels;
  absl::InlinedVector<Parcel, 2> outbound_parcels_to_proxy;
  absl::InlinedVector<Parcel, 2> inbound_parcels;
  absl::InlinedVector<Parcel, 2> inbound_parcels_to_proxy;
  absl::InlinedVector<Parcel, 2> bridge_parcels;
  bool inward_link_decayed = false;
  bool outward_link_decayed = false;
  SequenceNumber final_inward_sequence_length;
  SequenceNumber final_outward_sequence_length;
  {
    absl::MutexLock lock(&mutex_);
    inward_link = inward_edge_ ? inward_edge_->primary_link() : nullptr;
    outward_link = outward_edge_.primary_link();
    decaying_inward_link =
        inward_edge_ ? inward_edge_->decaying_link() : nullptr;
    decaying_outward_link = outward_edge_.decaying_link();
    if (bridge_) {
      bridge_link = bridge_->primary_link() ? bridge_->primary_link()
                                            : bridge_->decaying_link();
    }

    outward_edge_.FlushParcelsFromQueue(
        outbound_parcels_, outbound_parcels_to_proxy, outbound_parcels);
    const SequenceNumber outbound_sequence_length_sent =
        outbound_parcels_.current_sequence_number();
    const SequenceNumber inbound_sequence_length_received =
        inbound_parcels_.GetCurrentSequenceLength();
    if (outward_edge_.TryToFinishDecaying(outbound_sequence_length_sent,
                                          inbound_sequence_length_received)) {
      DVLOG(4) << "Outward " << DescribeLink(decaying_outward_link)
               << " fully decayed at " << outbound_sequence_length_sent
               << " sent and " << inbound_sequence_length_received
               << " received";
      outward_link_decayed = true;
    }

    if (inward_edge_) {
      inward_edge_->FlushParcelsFromQueue(
          inbound_parcels_, inbound_parcels_to_proxy, inbound_parcels);
      const SequenceNumber inbound_sequence_length_sent =
          inbound_parcels_.current_sequence_number();
      const SequenceNumber outbound_sequence_length_received =
          outbound_parcels_.GetCurrentSequenceLength();
      if (inward_edge_->TryToFinishDecaying(
            inbound_sequence_length_sent,
            outbound_sequence_length_received)) {
        DVLOG(4) << "Inward " << DescribeLink(decaying_inward_link)
                 << " fully decayed at " << inbound_sequence_length_sent
                 << " sent and " << outbound_sequence_length_received
                 << " received";
        inward_link_decayed = true;
      }
    }

    Parcel parcel;
    while (bridge_link && inbound_parcels_.Pop(parcel)) {
      DVLOG(4) << "Forwarding inbound " << parcel.Describe() << " over bridge "
               << DescribeLink(bridge_link);
      bridge_parcels.push_back(std::move(parcel));
    }

    if (bridge_ && bridge_->is_decaying() &&
        bridge_->TryToFinishDecaying(
            inbound_parcels_.current_sequence_number(),
            outbound_parcels_.current_sequence_number())) {
      bridge_.reset();
    }

    // If the inbound sequence is dead, the other side of the route is gone and
    // we have received all the parcels it sent. We can drop the outward link.
    if (outbound_parcels_.final_sequence_length()) {
      outward_link_for_closure_propagation =
          outward_edge_.GetLinkToPropagateRouteClosure();
    }

    if (outward_link_for_closure_propagation) {
      final_outward_sequence_length =
          *outbound_parcels_.final_sequence_length();

      ABSL_ASSERT(outbound_parcels_.IsEmpty());
      if (!dead_outward_link) {
        dead_outward_link = outward_edge_.ReleasePrimaryLink();
      }
    }

    if (inbound_parcels_.IsDead() && !dead_outward_link) {
      dead_outward_link = outward_edge_.ReleasePrimaryLink();
    }

    if (inward_edge_ && inbound_parcels_.final_sequence_length()) {
      inward_link_for_closure_propagation =
          inward_edge_->GetLinkToPropagateRouteClosure();
    } else if (bridge_ && inbound_parcels_.final_sequence_length()) {
      inward_link_for_closure_propagation =
          bridge_->GetLinkToPropagateRouteClosure();
    }

    if (inward_link_for_closure_propagation) {
      final_inward_sequence_length = *inbound_parcels_.final_sequence_length();
      if (inbound_parcels_.IsDead() && inward_edge_) {
        dead_inward_link = inward_edge_->ReleasePrimaryLink();
      } else if (inbound_parcels_.IsDead() && bridge_) {
        bridge_.reset();
      }
    }
  }

  if (outward_link) {
    outward_link->Flush();
  }

  for (Parcel& parcel : outbound_parcels_to_proxy) {
    decaying_outward_link->AcceptParcel(parcel);
  }

  for (Parcel& parcel : outbound_parcels) {
    outward_link->AcceptParcel(parcel);
  }

  for (Parcel& parcel : inbound_parcels_to_proxy) {
    decaying_inward_link->AcceptParcel(parcel);
  }

  for (Parcel& parcel : inbound_parcels) {
    inward_link->AcceptParcel(parcel);
  }

  for (Parcel& parcel : bridge_parcels) {
    bridge_link->AcceptParcel(parcel);
  }

  if (outward_link_decayed) {
    decaying_outward_link->Deactivate();
    decaying_outward_link.reset();
  }

  if (inward_link_decayed) {
    decaying_inward_link->Deactivate();
    decaying_inward_link.reset();
  }

  if (outward_link && outward_link->GetType().is_central()) {
    if ((inward_link_decayed || outward_link_decayed) &&
        (!decaying_inward_link && !decaying_outward_link)) {
      DVLOG(4) << "Router with fully decayed links may be eligible for "
               << "self-removal with outward " << DescribeLink(outward_link);
      outward_link->SetSideCanSupportBypass();
    }

    if (inward_link) {
      MaybeInitiateSelfRemoval();
    }
  }

  if (bridge_link && outward_link && !inward_link && !decaying_inward_link &&
      !decaying_outward_link) {
    MaybeInitiateBridgeBypass();
  }

  if (inward_link_for_closure_propagation) {
    inward_link_for_closure_propagation->AcceptRouteClosure(
        final_inward_sequence_length);
  }

  if (outward_link_for_closure_propagation) {
    outward_link_for_closure_propagation->AcceptRouteClosure(
        final_outward_sequence_length);
  }

  if (dead_inward_link) {
    dead_inward_link->Deactivate();
  }

  if (dead_outward_link) {
    dead_outward_link->Deactivate();
  }
}

bool Router::MaybeInitiateSelfRemoval() {
  NodeName peer_node_name;
  RoutingId routing_id_to_peer;
  mem::Ref<RemoteRouterLink> successor;
  mem::Ref<Router> local_peer;
  {
    absl::MutexLock lock(&mutex_);
    if (!inward_edge_ || !inward_edge_->is_stable() ||
        !outward_edge_.CanLockPrimaryLinkForBypass()) {
      return false;
    }

    ABSL_ASSERT(!inward_edge_->GetLocalPeer());
    successor = mem::WrapRefCounted(
        static_cast<RemoteRouterLink*>(inward_edge_->primary_link().get()));

    if (!outward_edge_.TryToLockPrimaryLinkForBypass(
            successor->node_link()->remote_node_name())) {
      DVLOG(4) << "Proxy self-removal blocked by busy "
               << DescribeLink(outward_edge_.primary_link());
      return false;
    }

    local_peer = outward_edge_.GetLocalPeer();
    if (!local_peer) {
      auto& remote_peer =
          static_cast<RemoteRouterLink&>(*outward_edge_.primary_link());
      peer_node_name = remote_peer.node_link()->remote_node_name();
      routing_id_to_peer = remote_peer.routing_id();
    }
  }

  if (!local_peer) {
    {
      absl::MutexLock lock(&mutex_);
      outward_edge_.StartDecaying();
      inward_edge_->StartDecaying();
    }
    DVLOG(4) << "Proxy at "
             << successor->node_link()->local_node_name().ToString()
             << " initiating its own bypass with link to successor "
             << successor->node_link()->remote_node_name().ToString()
             << " on routing ID " << successor->routing_id()
             << " and link to peer " << peer_node_name.ToString()
             << " on routing ID " << routing_id_to_peer;
    successor->RequestProxyBypassInitiation(peer_node_name, routing_id_to_peer);
    return true;
  }

  SequenceNumber sequence_length;
  const RoutingId new_routing_id =
      successor->node_link()->memory().AllocateRoutingIds(1);
  const NodeLinkAddress new_link_state_address =
      successor->node_link()->memory().AllocateRouterLinkState();
  mem::Ref<RouterLink> new_link = successor->node_link()->AddRoute(
      new_routing_id, new_link_state_address, LinkType::kCentral, LinkSide::kA,
      local_peer);

  {
    TwoMutexLock lock(&mutex_, &local_peer->mutex_);

    // It's possible that the local peer has been closed, in which case its
    // closure will have already propagated to us and there's no bypass work to
    // be done.
    if (!local_peer->outward_edge_.primary_link()) {
      ABSL_ASSERT(status_.flags & IPCZ_PORTAL_STATUS_PEER_CLOSED);
      DVLOG(4) << "Proxy self-removal blocked by peer closure.";
      return false;
    }

    DVLOG(4) << "Proxy initiating its own bypass from "
             << successor->node_link()->remote_node_name().ToString() << " to "
             << "a local peer.";

    // Otherwise it should definitely still be linked to us, because we locked
    // in our own decaying state above.
    ABSL_ASSERT(outward_edge_.GetLocalPeer() == local_peer);
    ABSL_ASSERT(local_peer->outward_edge_.GetLocalPeer() == this);

    sequence_length = local_peer->outbound_parcels_.current_sequence_number();

    local_peer->outward_edge_.StartDecaying(sequence_length);
    outward_edge_.StartDecaying(/*length_to_decaying_link=*/absl::nullopt,
                                sequence_length);
    inward_edge_->StartDecaying(sequence_length);
  }

  successor->BypassProxyToSameNode(new_routing_id, new_link_state_address,
                                   sequence_length);
  local_peer->SetOutwardLink(std::move(new_link));
  return true;
}

void Router::MaybeInitiateBridgeBypass() {
  mem::Ref<Router> first_bridge = mem::WrapRefCounted(this);
  mem::Ref<Router> second_bridge;
  {
    absl::MutexLock lock(&mutex_);
    if (!bridge_ || bridge_->is_decaying()) {
      return;
    }

    second_bridge = bridge_->GetLocalPeer();
    if (!second_bridge) {
      return;
    }
  }

  mem::Ref<Router> first_local_peer;
  mem::Ref<Router> second_local_peer;
  mem::Ref<RouterLink> link_to_first_peer;
  mem::Ref<RouterLink> link_to_second_peer;
  mem::Ref<RemoteRouterLink> remote_link_to_second_peer;
  {
    TwoMutexLock lock(&mutex_, &second_bridge->mutex_);
    link_to_first_peer = outward_edge_.primary_link();
    link_to_second_peer = second_bridge->outward_edge_.primary_link();
    if (!link_to_first_peer || !link_to_second_peer) {
      return;
    }

    first_local_peer = link_to_first_peer->GetLocalTarget();
    second_local_peer = link_to_second_peer->GetLocalTarget();

    if (!first_local_peer && second_local_peer) {
      // Only one of the bridges has a local peer. Make it the first one to
      // reduce the number of cases below. Bridges are symmetrical, so it
      // doesn't matter that we swap identities here.
      std::swap(first_bridge, second_bridge);
      std::swap(first_local_peer, second_local_peer);
      std::swap(link_to_first_peer, link_to_second_peer);
    }

    NodeName second_peer_node_name;
    if (!second_local_peer) {
      remote_link_to_second_peer = mem::WrapRefCounted(
          static_cast<RemoteRouterLink*>(link_to_second_peer.get()));
      second_peer_node_name =
          remote_link_to_second_peer->node_link()->remote_node_name();
    }

    if (!link_to_first_peer->TryToLockForBypass(second_peer_node_name)) {
      return;
    }
    if (!link_to_second_peer->TryToLockForBypass()) {
      // Cancel the decay on this bridge's side, because we couldn't decay the
      // other side of the bridge yet.
      link_to_first_peer->CancelBypassLock();
      return;
    }
  }

  // At this point both outward links from each bridge router have been locked
  // in for decay. We'll use the usual proxy bypass mechanism. From the
  // perspective of the two peers along each route, the bridging local pair
  // might just as well be a single proxying router.
  //
  // Now have 3 possible cases:
  // - neither peer is local to the bridge
  // - only the first peer is local to the bridge
  // - both peers are local to the bridge

  if (!first_local_peer && !second_local_peer) {
    // This case is equivalent to basic proxy bypass initiation.
    {
      TwoMutexLock lock(&first_bridge->mutex_, &second_bridge->mutex_);
      first_bridge->outward_edge_.StartDecaying();
      second_bridge->outward_edge_.StartDecaying();
      first_bridge->bridge_->StartDecaying();
      second_bridge->bridge_->StartDecaying();
    }

    link_to_second_peer->RequestProxyBypassInitiation(
        remote_link_to_second_peer->node_link()->remote_node_name(),
        remote_link_to_second_peer->routing_id());
    return;
  }

  if (!second_local_peer) {
    // This case is equivalent to basic proxy bypass when the proxy and its
    // outward peer are local to the same node. In this case we only need to
    // lock all the local routers to set up decaying links and use the same
    // BypassProxyToSameNode message used in the non-bridging case. As with
    // StopProxying() above, StopProxyingToLocalPeer() has logic to handle the
    // case where the receiving proxy is a route bridge.

    const mem::Ref<NodeLink> node_link_to_second_peer =
        remote_link_to_second_peer->node_link();
    SequenceNumber length_from_local_peer;
    const RoutingId bypass_routing_id =
        node_link_to_second_peer->memory().AllocateRoutingIds(1);
    const NodeLinkAddress bypass_link_state_address =
        node_link_to_second_peer->memory().AllocateRouterLinkState();
    mem::Ref<RouterLink> new_link = node_link_to_second_peer->AddRoute(
        bypass_routing_id, bypass_link_state_address, LinkType::kCentral,
        LinkSide::kA, first_local_peer);
    {
      ThreeMutexLock lock(&first_bridge->mutex_, &second_bridge->mutex_,
                          &first_local_peer->mutex_);

      length_from_local_peer =
          first_local_peer->outbound_parcels_.current_sequence_number();

      first_local_peer->outward_edge_.StartDecaying(length_from_local_peer);
      second_bridge->outward_edge_.StartDecaying(length_from_local_peer);
      first_bridge->bridge_->StartDecaying(length_from_local_peer);
      first_bridge->outward_edge_.StartDecaying(absl::nullopt,
                                                length_from_local_peer);
      second_bridge->bridge_->StartDecaying(absl::nullopt,
                                            length_from_local_peer);
    }

    link_to_second_peer->BypassProxyToSameNode(
        bypass_routing_id, bypass_link_state_address, length_from_local_peer);
    first_local_peer->SetOutwardLink(std::move(new_link));
    first_bridge->Flush();
    second_bridge->Flush();
    first_local_peer->Flush();
    return;
  }

  // Finally the all-local case, where we swap around a bunch of link pointers
  // and sequence lengths and let the links and routers decay from there.
  {
    FourMutexLock lock(&first_bridge->mutex_, &second_bridge->mutex_,
                       &first_local_peer->mutex_, &second_local_peer->mutex_);
    const SequenceNumber length_from_first_peer =
        first_local_peer->outbound_parcels_.current_sequence_number();
    const SequenceNumber length_from_second_peer =
        second_local_peer->outbound_parcels_.current_sequence_number();

    first_local_peer->outward_edge_.StartDecaying(length_from_first_peer,
                                                  length_from_second_peer);
    second_local_peer->outward_edge_.StartDecaying(length_from_second_peer,
                                                   length_from_first_peer);
    first_bridge->outward_edge_.StartDecaying(length_from_second_peer,
                                              length_from_first_peer);
    second_bridge->outward_edge_.StartDecaying(length_from_first_peer,
                                               length_from_second_peer);
    first_bridge->bridge_->StartDecaying(length_from_first_peer,
                                         length_from_second_peer);
    second_bridge->bridge_->StartDecaying(length_from_second_peer,
                                          length_from_first_peer);

    RouterLink::Pair links = LocalRouterLink::CreatePair(
        LinkType::kCentral, LocalRouterLink::InitialState::kCannotBypass,
        Router::Pair(first_local_peer, second_local_peer));
    first_local_peer->outward_edge_.SetPrimaryLink(std::move(links.first));
    second_local_peer->outward_edge_.SetPrimaryLink(std::move(links.second));
  }

  first_bridge->Flush();
  second_bridge->Flush();
  first_local_peer->Flush();
  second_local_peer->Flush();
}

bool Router::SerializeNewRouterWithLocalPeer(NodeLink& to_node_link,
                                             RouterDescriptor& descriptor,
                                             mem::Ref<Router> local_peer) {
  SequenceNumber proxy_inbound_sequence_length;
  {
    TwoMutexLock lock(&mutex_, &local_peer->mutex_);
    if (local_peer->outward_edge_.GetLocalPeer() != this) {
      // If the peer was closed, its link to us may already be invalidated.
      return false;
    }

    proxy_inbound_sequence_length =
        local_peer->outbound_parcels_.current_sequence_number();

    // The local peer no longer needs its link to us. We'll give it a new
    // outward link in BeginProxyingToNewRouter(), after this descriptor is
    // transmitted.
    local_peer->outward_edge_.ReleasePrimaryLink();
  }

  // The primary new routing ID to the destination node will act as the route's
  // new central link, between our local peer and the new remote router.
  //
  // An additional route is allocated to act as a decaying inward link between
  // us and the new router, to forward any parcels already queued here or
  // in-flight from our local peer.
  const RoutingId new_routing_id = to_node_link.memory().AllocateRoutingIds(2);
  const NodeLinkAddress new_link_state_address =
      to_node_link.memory().AllocateRouterLinkState();
  const RoutingId decaying_routing_id = new_routing_id + 1;

  // Register the new routes on the NodeLink. Note that we don't provide them to
  // any routers yet since we don't want the routers using them until this
  // descriptor is transmitted to its destination node. The links will be
  // adopted after transmission in BeginProxyingToNewRouter().
  mem::Ref<RouterLink> new_link =
      to_node_link.AddRoute(new_routing_id, new_link_state_address,
                            LinkType::kCentral, LinkSide::kA, local_peer);

  // The local peer's side of the link has nothing to decay, so it can
  // immediately raise its support for bypass.
  new_link->SetSideCanSupportBypass();

  to_node_link.AddRoute(decaying_routing_id, kNullNodeLinkAddress,
                        LinkType::kPeripheralInward, LinkSide::kA,
                        mem::WrapRefCounted(this));

  descriptor.new_routing_id = new_routing_id;
  descriptor.new_link_state_address = new_link_state_address;
  descriptor.new_decaying_routing_id = decaying_routing_id;
  descriptor.proxy_already_bypassed = true;

  TwoMutexLock lock(&mutex_, &local_peer->mutex_);
  descriptor.next_outgoing_sequence_number =
      outbound_parcels_.current_sequence_number();
  descriptor.next_incoming_sequence_number =
      inbound_parcels_.current_sequence_number();
  descriptor.decaying_incoming_sequence_length = proxy_inbound_sequence_length;

  DVLOG(4) << "Splitting local pair to move router with outbound sequence "
           << "length " << descriptor.next_outgoing_sequence_number
           << " and current inbound sequence number "
           << descriptor.next_incoming_sequence_number;

  if (status_.flags & IPCZ_PORTAL_STATUS_PEER_CLOSED) {
    ABSL_ASSERT(inbound_parcels_.final_sequence_length());
    descriptor.peer_closed = true;
    descriptor.closed_peer_sequence_length =
        *inbound_parcels_.final_sequence_length();
  }

  // Initialize an inward edge that will immediately begin decaying once it has
  // a link (established in BeginProxyingToNewRouter()).
  inward_edge_.emplace();
  inward_edge_->StartDecaying(proxy_inbound_sequence_length,
                              outbound_parcels_.current_sequence_number());
  return true;
}

void Router::SerializeNewRouterAndConfigureProxy(NodeLink& to_node_link,
                                                 RouterDescriptor& descriptor,
                                                 bool initiate_proxy_bypass) {
  {
    absl::MutexLock lock(&mutex_);

    descriptor.proxy_already_bypassed = false;
    descriptor.next_outgoing_sequence_number =
        outbound_parcels_.current_sequence_number();
    descriptor.next_incoming_sequence_number =
        inbound_parcels_.current_sequence_number();

    DVLOG(4) << "Extending route to new router with outbound sequence length "
             << descriptor.next_outgoing_sequence_number
             << " and current inbound sequence number "
             << descriptor.next_incoming_sequence_number;

    // A new link will be assigned to this edge in BeginProxyingToNewRouter().
    inward_edge_.emplace();

    if (status_.flags & IPCZ_PORTAL_STATUS_PEER_CLOSED) {
      descriptor.peer_closed = true;
      descriptor.closed_peer_sequence_length =
          *inbound_parcels_.final_sequence_length();

      // Ensure that the new edge is decayed immediately since we know it won't
      // be used.
      inward_edge_->StartDecaying(*inbound_parcels_.final_sequence_length(),
                                  outbound_parcels_.current_sequence_number());
    } else if (initiate_proxy_bypass && !outward_edge_.GetLocalPeer()) {
      RemoteRouterLink& remote_link =
          static_cast<RemoteRouterLink&>(*outward_edge_.primary_link());
      descriptor.proxy_peer_node_name =
          remote_link.node_link()->remote_node_name();
      descriptor.proxy_peer_routing_id = remote_link.routing_id();
      DVLOG(4) << "Will initiate proxy bypass immediately on deserialization "
               << "with peer at " << descriptor.proxy_peer_node_name.ToString()
               << " and peer route to proxy on routing ID "
               << descriptor.proxy_peer_routing_id;

      // Immediately begin decaying inward and outward edges. They'll get
      // concrete links to decay in BeginProxyingToNewRouter().
      inward_edge_->StartDecaying();
      outward_edge_.StartDecaying();
    }
  }

  const RoutingId new_routing_id = to_node_link.memory().AllocateRoutingIds(1);
  descriptor.new_routing_id = new_routing_id;

  // Register the new route with the NodeLink. We don't provide this to the
  // router yet because it's not safe to use until this descriptor has been
  // transmitted to its destination node. The link will be adopted after
  // transmission, in BeginProxyingToNewRouter().
  to_node_link.AddRoute(new_routing_id, kNullNodeLinkAddress,
                        LinkType::kPeripheralInward, LinkSide::kA,
                        mem::WrapRefCounted(this));
}

}  // namespace core
}  // namespace ipcz
