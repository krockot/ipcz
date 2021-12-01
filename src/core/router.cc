// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/router.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <forward_list>
#include <utility>
#include <vector>

#include "core/local_router_link.h"
#include "core/node.h"
#include "core/node_link.h"
#include "core/node_name.h"
#include "core/parcel.h"
#include "core/portal_descriptor.h"
#include "core/remote_router_link.h"
#include "core/router_link.h"
#include "core/router_link_state.h"
#include "core/routing_id.h"
#include "core/sequence_number.h"
#include "core/trap.h"
#include "core/trap_event_dispatcher.h"
#include "debug/log.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/numeric/int128.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "util/random.h"
#include "util/two_mutex_lock.h"

namespace ipcz {
namespace core {

Router::Router(Side side) : side_(side) {}

Router::~Router() = default;

void Router::PauseOutboundTransmission(bool paused) {
  {
    absl::MutexLock lock(&mutex_);
    ABSL_ASSERT(outbound_transmission_paused_ != paused);
    if (paused && outward_.link && !inward_.link) {
      // If we're pausing and we currently have an outward link but no inward
      // link, the outbound ParcelQueue is not currently in use. Since we may
      // begin queuing outbound parcels there while paused, update its base
      // SequenceNumber with that of the next expected parcel.
      ABSL_ASSERT(outward_.parcels.IsEmpty());
      outward_.parcels.ResetBaseSequenceNumber(outbound_sequence_length_);
    }
    outbound_transmission_paused_ = paused;
  }

  if (!paused) {
    Flush();
  }
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

bool Router::HasLocalPeer(const mem::Ref<Router>& router) {
  absl::MutexLock lock(&mutex_);
  return outward_.link && outward_.link->GetLocalTarget() == router;
}

bool Router::WouldOutboundParcelExceedLimits(size_t data_size,
                                             const IpczPutLimits& limits) {
  mem::Ref<RouterLink> link;
  {
    absl::MutexLock lock(&mutex_);
    link = outward_.link;
    if (!link) {
      return outward_.parcels.GetNumAvailableParcels() <
                 limits.max_queued_parcels &&
             outward_.parcels.GetNumAvailableBytes() <=
                 limits.max_queued_bytes &&
             limits.max_queued_bytes -
                     outward_.parcels.GetNumAvailableBytes() <=
                 data_size;
    }
  }

  return link->WouldParcelExceedLimits(data_size, limits);
}

bool Router::WouldInboundParcelExceedLimits(size_t data_size,
                                            const IpczPutLimits& limits) {
  absl::MutexLock lock(&mutex_);
  return inward_.parcels.GetNumAvailableBytes() + data_size >
             limits.max_queued_bytes &&
         inward_.parcels.GetNumAvailableParcels() >= limits.max_queued_parcels;
}

IpczResult Router::SendOutboundParcel(absl::Span<const uint8_t> data,
                                      Parcel::PortalVector& portals,
                                      std::vector<os::Handle>& os_handles) {
  Parcel parcel;
  parcel.SetData(std::vector<uint8_t>(data.begin(), data.end()));
  parcel.SetPortals(std::move(portals));
  parcel.SetOSHandles(std::move(os_handles));

  mem::Ref<RouterLink> link;
  {
    absl::MutexLock lock(&mutex_);
    parcel.set_sequence_number(outbound_sequence_length_++);
    link = outward_.link;
    if (!link || outbound_transmission_paused_) {
      const bool ok = outward_.parcels.Push(std::move(parcel));
      ABSL_ASSERT(ok);
      return IPCZ_RESULT_OK;
    }
  }

  link->AcceptParcel(parcel);
  return IPCZ_RESULT_OK;
}

void Router::CloseRoute() {
  SequenceNumber final_sequence_length;
  mem::Ref<RouterLink> forwarding_link;
  {
    absl::MutexLock lock(&mutex_);

    // We can't have an inward link, because CloseRoute() must only be called on
    // a terminal Router; that is, a Router directly controlled by a Portal.
    ABSL_ASSERT(!inward_.link);

    // The final outbound sequence length is the current outbound sequence
    // length.
    outward_.parcels.SetPeerSequenceLength(outbound_sequence_length_);
    if (!outward_.link || outbound_transmission_paused_) {
      return;
    }

    forwarding_link = outward_.link;
    final_sequence_length = outbound_sequence_length_;
  }

  forwarding_link->AcceptRouteClosure(side_, final_sequence_length);
}

SequenceNumber Router::SetOutwardLink(mem::Ref<RouterLink> link) {
  SequenceNumber first_sequence_number_on_new_link;
  {
    absl::MutexLock lock(&mutex_);
    ABSL_ASSERT(!outward_.link);
    outward_.link = std::move(link);
    if (outward_.decaying_proxy_link) {
      ABSL_ASSERT(outward_.sequence_length_to_decaying_link);
      first_sequence_number_on_new_link =
          *outward_.sequence_length_to_decaying_link;
    } else if (!outward_.parcels.IsEmpty()) {
      first_sequence_number_on_new_link =
          outward_.parcels.current_sequence_number();
    } else {
      first_sequence_number_on_new_link = outbound_sequence_length_;
    }

    if (outbound_transmission_paused_) {
      return first_sequence_number_on_new_link;
    }
  }

  Flush();
  return first_sequence_number_on_new_link;
}

void Router::BeginProxying(const PortalDescriptor& descriptor,
                           mem::Ref<RouterLink> link,
                           mem::Ref<RouterLink> decaying_link) {
  mem::Ref<RouterLink> outward_link;
  {
    absl::MutexLock lock(&mutex_);
    ABSL_ASSERT(!inward_.link);
    if (descriptor.route_is_peer) {
      // When `route_is_peer` is true, this means we have two local Routers --
      // let's call them P and Q -- who were each other's local peer, and who
      // were just split apart by one of them (`this`, call it Q) serializing
      // its portal and extending the route to another node. In this case,
      // `link` is a link to the new router, and the local target of
      // `outward_link` is our former local peer (P) who must now use `link` as
      // its own outward link.
      //
      // We set up Q with an inward link here to `link` so it can forward any
      // incoming parcels -- already received from or in flight from P -- to the
      // new router. Below we will fix up P with the new outward link to `link`
      // as well.
      //
      // This is an optimization for the common case of a local pair being split
      // across nodes, where we have enough information at serialiation and
      // deserialiation time to avoid all of overhead of the usual asynchronous
      // procedure for bypassing a proxy.
      std::swap(outward_.link, outward_link);
    }
  }

  mem::Ref<Router> local_peer;
  if (descriptor.route_is_peer) {
    ABSL_ASSERT(outward_link);
    local_peer = outward_link->GetLocalTarget();
    ABSL_ASSERT(local_peer);

    TwoMutexLock lock(&mutex_, &local_peer->mutex_);
    if (decaying_link) {
      inward_.decaying_proxy_link = std::move(decaying_link);
      inward_.sequence_length_to_decaying_link =
          local_peer->outbound_sequence_length_;
      inward_.sequence_length_from_decaying_link = outbound_sequence_length_;
    }

    // Safeguard: this router should no longer deal with outbound parcels. It
    // has no outward link, and this ensures that any attempt to queue an
    // outbound parcel for later transmission will fail.
    ABSL_ASSERT(outward_.parcels.IsEmpty());
    outward_.parcels.SetPeerSequenceLength(0);

    local_peer->outward_.link = std::move(link);
  } else {
    absl::MutexLock lock(&mutex_);
    // In the case where `route_is_peer` is false, this Router is becoming a
    // bidirectional proxy. We need to set its inward link accordingly.
    inward_.link = std::move(link);

    // We must also configure its outward ParcelQueue. Note that this Router
    // has definitely never been a proxy until now, so it either remains
    // without an outward link (e.g. because it's still waiting for a Node
    // connection or introduction) and is already queueing outbound parcels,
    // or it has already transmitted some parcels and we must configure the
    // outward queue to begin queueing at the next outbound SequenceNumber.
    ABSL_ASSERT(!outward_.link || outward_.parcels.IsEmpty());
    if (!outward_.link) {
      outward_.parcels.ResetBaseSequenceNumber(outbound_sequence_length_);
    }
  }

  Flush();
  if (local_peer) {
    local_peer->Flush();
  }
}

bool Router::StopProxying(SequenceNumber inbound_sequence_length,
                          SequenceNumber outbound_sequence_length) {
  {
    absl::MutexLock lock(&mutex_);
    inward_.decaying_proxy_link = std::move(inward_.link);
    inward_.sequence_length_to_decaying_link = inbound_sequence_length;
    inward_.sequence_length_from_decaying_link = outbound_sequence_length;
    outward_.decaying_proxy_link = std::move(outward_.link);
    outward_.sequence_length_to_decaying_link = outbound_sequence_length;
    outward_.sequence_length_from_decaying_link = inbound_sequence_length;
  }

  Flush();
  return true;
}

bool Router::AcceptParcelFrom(NodeLink& link,
                              RoutingId routing_id,
                              Parcel& parcel) {
  bool is_inbound;
  {
    absl::MutexLock lock(&mutex_);
    // Inbound parcels arrive from outward links and outbound parcels arrive
    // from inward links.
    if (outward_.link && outward_.link->IsRemoteLinkTo(link, routing_id)) {
      DVLOG(4) << "Inbound parcel received by "
               << link.node()->name().ToString() << " on outward route "
               << routing_id << " to " << link.remote_node_name().ToString();

      is_inbound = true;
    } else if (outward_.decaying_proxy_link &&
               outward_.decaying_proxy_link->IsRemoteLinkTo(link, routing_id)) {
      DVLOG(4) << "Inbound parcel received by "
               << link.node()->name().ToString()
               << " on decaying outward route " << routing_id << " to "
               << link.remote_node_name().ToString();

      is_inbound = true;
    } else if (inward_.link && inward_.link->IsRemoteLinkTo(link, routing_id)) {
      DVLOG(4) << "Outbound parcel received by "
               << link.node()->name().ToString() << " on inward route "
               << routing_id << " to " << link.remote_node_name().ToString();

      is_inbound = false;
    } else if (inward_.decaying_proxy_link &&
               inward_.decaying_proxy_link->IsRemoteLinkTo(link, routing_id)) {
      DVLOG(4) << "Outbound parcel received by "
               << link.node()->name().ToString() << " on decaying inward route "
               << routing_id << " to " << link.remote_node_name().ToString();

      is_inbound = false;
    } else {
      DVLOG(4) << "Rejecting unexpected parcel at "
               << link.node()->name().ToString() << " from route " << routing_id
               << " to " << link.remote_node_name().ToString();
      return false;
    }
  }

  if (is_inbound) {
    return AcceptInboundParcel(parcel);
  }

  return AcceptOutboundParcel(parcel);
}

bool Router::AcceptInboundParcel(Parcel& parcel) {
  TrapEventDispatcher dispatcher;
  {
    absl::MutexLock lock(&mutex_);
    if (!inward_.parcels.Push(std::move(parcel))) {
      return false;
    }

    if (!inward_.link && !inward_.decaying_proxy_link) {
      status_.num_local_parcels = inward_.parcels.GetNumAvailableParcels();
      status_.num_local_bytes = inward_.parcels.GetNumAvailableBytes();
      traps_.MaybeNotify(dispatcher, status_);
      return true;
    }
  }

  Flush();
  return true;
}

bool Router::AcceptOutboundParcel(Parcel& parcel) {
  {
    absl::MutexLock lock(&mutex_);

    // We must have an inward link if we're accepting an outbound parcel,
    // because we only accept outbound parcels from inward links.
    ABSL_ASSERT(inward_.link || inward_.decaying_proxy_link);

    // Proxied outbound parcels are always queued in a ParcelQueue even if they
    // will be immediately forwarded. This allows us to track the full sequence
    // of forwarded parcels so we can know with certainty when we're done
    // forwarding.
    if (!outward_.parcels.Push(std::move(parcel))) {
      return false;
    }

    if (!outward_.link || outbound_transmission_paused_) {
      return true;
    }
  }

  Flush();
  return true;
}

void Router::AcceptRouteClosure(Side side, SequenceNumber sequence_length) {
  TrapEventDispatcher dispatcher;
  mem::Ref<RouterLink> forwarding_link;
  if (side == side_) {
    // If we're being notified of our own side's closure, we want to propagate
    // this outward toward the other side.
    absl::MutexLock lock(&mutex_);
    ABSL_ASSERT(inward_.link);
    outward_.parcels.SetPeerSequenceLength(sequence_length);
    if (!outward_.closure_propagated && !outbound_transmission_paused_ &&
        outward_.link) {
      forwarding_link = outward_.link;
      outward_.closure_propagated = true;
    }
  } else {
    // We're being notified of the other side's clsoure, so we want to propagate
    // this inward toward our own terminal router. If that's us, update portal
    // status and traps.
    absl::MutexLock lock(&mutex_);
    inward_.parcels.SetPeerSequenceLength(sequence_length);
    if (!inward_.closure_propagated && inward_.link) {
      forwarding_link = inward_.link;
      inward_.closure_propagated = true;
    } else if (!inward_.link) {
      status_.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
      if (inward_.parcels.IsDead()) {
        status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
      }
      traps_.MaybeNotify(dispatcher, status_);
    }
  }

  if (forwarding_link) {
    forwarding_link->AcceptRouteClosure(side, sequence_length);
  }
}

IpczResult Router::GetNextIncomingParcel(void* data,
                                         uint32_t* num_bytes,
                                         IpczHandle* portals,
                                         uint32_t* num_portals,
                                         IpczOSHandle* os_handles,
                                         uint32_t* num_os_handles) {
  TrapEventDispatcher dispatcher;
  absl::MutexLock lock(&mutex_);
  if (!inward_.parcels.HasNextParcel()) {
    if (inward_.parcels.IsDead()) {
      return IPCZ_RESULT_NOT_FOUND;
    }
    return IPCZ_RESULT_UNAVAILABLE;
  }

  Parcel& p = inward_.parcels.NextParcel();
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
  inward_.parcels.Pop(parcel);
  memcpy(data, parcel.data_view().data(), parcel.data_view().size());
  parcel.Consume(portals, os_handles);

  status_.num_local_parcels = inward_.parcels.GetNumAvailableParcels();
  status_.num_local_bytes = inward_.parcels.GetNumAvailableBytes();
  if (inward_.parcels.IsDead()) {
    status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
    traps_.MaybeNotify(dispatcher, status_);
  }

  return IPCZ_RESULT_OK;
}

IpczResult Router::BeginGetNextIncomingParcel(const void** data,
                                              uint32_t* num_data_bytes,
                                              uint32_t* num_portals,
                                              uint32_t* num_os_handles) {
  absl::MutexLock lock(&mutex_);
  if (!inward_.parcels.HasNextParcel()) {
    return IPCZ_RESULT_UNAVAILABLE;
  }

  Parcel& p = inward_.parcels.NextParcel();
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
  if (!inward_.parcels.HasNextParcel()) {
    // If ipcz is used correctly this is impossible.
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  Parcel& p = inward_.parcels.NextParcel();
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
  bool ok = inward_.parcels.Pop(consumed_parcel);
  ABSL_ASSERT(ok);

  status_.num_local_parcels = inward_.parcels.GetNumAvailableParcels();
  status_.num_local_bytes = inward_.parcels.GetNumAvailableBytes();
  if (inward_.parcels.IsDead()) {
    status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
    traps_.MaybeNotify(dispatcher, status_);
  }
  return IPCZ_RESULT_OK;
}

IpczResult Router::AddTrap(std::unique_ptr<Trap> trap) {
  absl::MutexLock lock(&mutex_);
  return traps_.Add(std::move(trap));
}

IpczResult Router::ArmTrap(Trap& trap,
                           IpczTrapConditionFlags& satistfied_conditions,
                           IpczPortalStatus* status) {
  absl::MutexLock lock(&mutex_);
  IpczResult result = trap.Arm(status_, satistfied_conditions);
  if (result != IPCZ_RESULT_OK && status) {
    const size_t size = std::min(status_.size, status->size);
    memcpy(status, &status_, size);
    status->size = size;
  }
  return result;
}

IpczResult Router::RemoveTrap(Trap& trap) {
  absl::MutexLock lock(&mutex_);
  return traps_.Remove(trap);
}

mem::Ref<Router> Router::Serialize(PortalDescriptor& descriptor) {
  descriptor.side = side_;

  for (;;) {
    // The fast path for a local pair being split is to directly establish a new
    // outward link to the destination, rather than proxying. First we acquire a
    // ref to the local peer Router, if there is one.
    mem::Ref<Router> local_peer;
    {
      absl::MutexLock lock(&mutex_);
      traps_.Clear();
      if (outward_.link) {
        local_peer = outward_.link->GetLocalTarget();
      }
    }

    TwoMutexLock lock(&mutex_, local_peer ? &local_peer->mutex_ : nullptr);
    if (local_peer) {
      // The outward link has already changed. Loop back and try again,
      if (!outward_.link || outward_.link->GetLocalTarget() != local_peer) {
        continue;
      }

      local_peer->mutex_.AssertHeld();

      // Note that by the time we acquire both locks above, the pair may have
      // been split by another thread serializing the peer for transmission
      // elsewhere, so we need to first verify that the pair is still intact. If
      // it's not we fall back to the normal proxying path below.
      if (local_peer->outward_.link &&
          local_peer->outward_.link->GetLocalTarget() == this) {
        // Remove the peer's outward link so that it doesn't transmit more
        // parcels directly to us. It will instead queue them for later
        // transmission.
        ABSL_ASSERT(local_peer->outward_.parcels.IsEmpty());
        local_peer->outward_.parcels.ResetBaseSequenceNumber(
            local_peer->outbound_sequence_length_);
        local_peer->outward_.link.reset();

        if (inward_.parcels.peer_sequence_length()) {
          descriptor.peer_closed = true;
          descriptor.closed_peer_sequence_length =
              *inward_.parcels.peer_sequence_length();
          inward_.closure_propagated = true;
        } else {
          // Fix the incoming sequence length at the last transmitted parcel, so
          // this router can be destroyed once incoming parcels up to this point
          // have been forwarded to the new peer.
          inward_.parcels.SetPeerSequenceLength(
              local_peer->outbound_sequence_length_);
        }
        descriptor.route_is_peer = true;
        descriptor.next_outgoing_sequence_number = outbound_sequence_length_;
        descriptor.next_incoming_sequence_number =
            inward_.parcels.current_sequence_number();

        // The local peer will listen on the new link, rather than this router.
        // This router will only persist to forward its queued inbound parcels
        // on to the new remote router.
        return local_peer;
      }
    } else if (outward_.link && outward_.link->GetLocalTarget()) {
      // We didn't have a local peer before, but now we do. Try again.
      continue;
    }

    // In this case, we're not splitting a local pair, but merely extending this
    // Router's side of the route to another node. This Router will become a
    // proxy, forwarding inbound parcels across its new inward link (to the
    // new Router we're currently serializing) and forwarding outbound parcels
    // from its inward link to its outward link.
    descriptor.route_is_peer = false;
    descriptor.next_outgoing_sequence_number = outbound_sequence_length_;
    descriptor.next_incoming_sequence_number =
        inward_.parcels.current_sequence_number();
    bool immediate_bypass = false;
    NodeName proxy_peer_node_name;
    RoutingId proxy_peer_routing_id;
    absl::uint128 bypass_key;
    if (inward_.parcels.peer_sequence_length()) {
      descriptor.peer_closed = true;
      descriptor.closed_peer_sequence_length =
          *inward_.parcels.peer_sequence_length();
      inward_.closure_propagated = true;
    } else if (outward_.link && !outward_.link->GetLocalTarget() &&
               outward_.link->IsLinkToOtherSide() && !inward_.link &&
               !outward_.decaying_proxy_link && !inward_.decaying_proxy_link) {
      // If we're becoming a proxy under some common special conditions, we can
      // actually roll the first step of the proxy bypass procedure into this
      // serialized transmission. If all such conditions are met, we'll set
      // `immediate_bypass` to true and include extra data in the descriptor
      // below. This tells the new Router on deserialization to immediately
      // initiate our bypass.
      RemoteRouterLink& remote_link =
          *static_cast<RemoteRouterLink*>(outward_.link.get());
      proxy_peer_node_name = remote_link.node_link()->remote_node_name();
      proxy_peer_routing_id = remote_link.routing_id();
      bypass_key = RandomUint128();
      RouterLinkState::Locked locked(outward_.link->GetLinkState(), side_);
      if (!locked.other_side().is_decaying) {
        immediate_bypass = true;
        locked.this_side().is_decaying = true;
        locked.this_side().bypass_key = bypass_key;
      }
    }

    if (immediate_bypass) {
      descriptor.proxy_peer_node_name = proxy_peer_node_name;
      descriptor.proxy_peer_routing_id = proxy_peer_routing_id;
      descriptor.bypass_key = bypass_key;
    }

    return mem::WrapRefCounted(this);
  }
}

// static
mem::Ref<Router> Router::Deserialize(const PortalDescriptor& descriptor,
                                     NodeLink& from_node_link) {
  auto router = mem::MakeRefCounted<Router>(descriptor.side);
  {
    absl::MutexLock lock(&router->mutex_);
    router->outbound_sequence_length_ =
        descriptor.next_outgoing_sequence_number;
    router->inward_.parcels.ResetBaseSequenceNumber(
        descriptor.next_incoming_sequence_number);
    if (descriptor.peer_closed) {
      router->status_.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
      router->inward_.parcels.SetPeerSequenceLength(
          descriptor.closed_peer_sequence_length);
      if (router->inward_.parcels.IsDead()) {
        router->status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
      }
    }

    router->outward_.link = from_node_link.AddRoute(
        descriptor.new_routing_id, descriptor.new_routing_id, router,
        descriptor.route_is_peer ? RemoteRouterLink::Type::kToOtherSide
                                 : RemoteRouterLink::Type::kToSameSide);
    if (descriptor.route_is_peer) {
      // When split from a local peer, our remote counterpart (our remote peer's
      // former local peer) will use this link to forward parcels it already
      // received from our peer. This link decays like any other decaying link
      // once its usefulness has expired.
      router->outward_.decaying_proxy_link =
          from_node_link.AddRoute(descriptor.new_decaying_routing_id,
                                  descriptor.new_decaying_routing_id, router,
                                  RemoteRouterLink::Type::kToSameSide);

      // The sequence length toward this link is the current outbound sequence
      // length, which is to say, we will not be sending any parcels that way.
      router->outward_.sequence_length_to_decaying_link =
          router->outbound_sequence_length_;

      // As soon as we have every parcel that had been sent locally to our
      // remote counterpair, this proxy will decay.
      router->outward_.sequence_length_from_decaying_link =
          descriptor.next_incoming_sequence_number;

      DVLOG(4) << "Route side " << static_cast<int>(router->side_)
               << " moved from split pair on "
               << from_node_link.remote_node_name().ToString() << " to "
               << from_node_link.node()->name().ToString() << " via routing ID "
               << descriptor.new_routing_id << " and decaying routing ID "
               << descriptor.new_decaying_routing_id;
    } else {
      DVLOG(4) << "Route side " << static_cast<int>(router->side_)
               << " extended from "
               << from_node_link.remote_node_name().ToString() << " to "
               << from_node_link.node()->name().ToString() << " via routing ID "
               << descriptor.new_routing_id;
    }
  }

  if (descriptor.proxy_peer_node_name.is_valid()) {
    // The predecessor is already a half-proxy and has given us the means to
    // initiate its own bypass.
    router->InitiateProxyBypass(from_node_link, descriptor.new_routing_id,
                                descriptor.proxy_peer_node_name,
                                descriptor.proxy_peer_routing_id,
                                descriptor.bypass_key);
  }

  return router;
}

bool Router::InitiateProxyBypass(NodeLink& requesting_node_link,
                                 RoutingId requesting_routing_id,
                                 const NodeName& proxy_peer_node_name,
                                 RoutingId proxy_peer_routing_id,
                                 absl::uint128 bypass_key) {
  {
    absl::MutexLock lock(&mutex_);
    if (!outward_.link) {
      // Must have been disconnected already.
      return true;
    }

    if (!outward_.link->IsRemoteLinkTo(requesting_node_link,
                                       requesting_routing_id)) {
      // Authenticate that the request to bypass our outward peer is actually
      // coming from our outward peer.
      return false;
    }
  }

  if (proxy_peer_node_name == requesting_node_link.node()->name()) {
    // A special case where we have 3 portals P Q and R, with Q proxying between
    // P and R, and with P and R living on the same node. In this case,
    // bypassing Q will result in P and R becoming directly linked local peers.
    // In this case we set up R with a decaying outward proxy link to Q, from
    // whom R may receive inbound parcels already sent by P to Q. R will not use
    // this link to convey any proxied parcels to R. P also temporarily retains
    // its its outward link to Q as a decaying link until P has forwarded to Q
    // any parcels up to and including the highest sequence number already
    // transmitted to Q: note that P itself may be a proxy, so there's no
    // guarantee that it's transmitting outbound parcels to Q in strict sequence
    // order.
    mem::Ref<RouterLink> previous_outward_link_from_new_local_peer;
    mem::Ref<Router> new_local_peer =
        requesting_node_link.GetRouter(proxy_peer_routing_id);
    SequenceNumber proxied_inbound_sequence_length;
    SequenceNumber proxied_outbound_sequence_length;
    ABSL_ASSERT(new_local_peer);
    ABSL_ASSERT(new_local_peer->side_ == Opposite(side_));
    {
      TwoMutexLock lock(&mutex_, &new_local_peer->mutex_);

      proxied_inbound_sequence_length =
          new_local_peer->outbound_sequence_length_;
      proxied_outbound_sequence_length = outbound_sequence_length_;
      previous_outward_link_from_new_local_peer =
          std::move(new_local_peer->outward_.link);

      DVLOG(4) << "Initiating proxy bypass with new local peer on "
               << proxy_peer_node_name.ToString() << " and proxy links to "
               << requesting_node_link.remote_node_name().ToString()
               << " on routing IDs" << proxy_peer_routing_id << " and "
               << requesting_routing_id;

      // We get a decaying outward link to the proxy, only to accept inbound
      // parcels already sent to it by our new local peer.
      outward_.decaying_proxy_link = std::move(outward_.link);
      outward_.sequence_length_from_decaying_link =
          proxied_inbound_sequence_length;
      outward_.sequence_length_to_decaying_link =
          proxied_outbound_sequence_length;

      // Our new local peer gets a decaying outward link to the proxy, only to
      // forward outbound parcels already expected by the proxy.
      new_local_peer->outward_.decaying_proxy_link =
          previous_outward_link_from_new_local_peer;
      new_local_peer->outward_.sequence_length_to_decaying_link =
          proxied_inbound_sequence_length;
      new_local_peer->outward_.sequence_length_from_decaying_link =
          proxied_outbound_sequence_length;

      // Finally, create a new LocalRouterLink and use it to replace both our
      // own outward link and our new local peer's outward link.
      mem::Ref<Router> left =
          side_ == Side::kLeft ? mem::WrapRefCounted(this) : new_local_peer;
      mem::Ref<Router> right =
          side_ == Side::kLeft ? new_local_peer : mem::WrapRefCounted(this);
      ABSL_ASSERT(left != right);
      left->mutex_.AssertHeld();
      right->mutex_.AssertHeld();
      mem::Ref<RouterLink> left_link;
      mem::Ref<RouterLink> right_link;
      std::tie(left_link, right_link) =
          LocalRouterLink::CreatePair(left, right);
      left->outward_.link = std::move(left_link);
      right->outward_.link = std::move(right_link);
    }

    if (!previous_outward_link_from_new_local_peer) {
      // TODO: should we do something here? probably means the route is toast.
    } else {
      previous_outward_link_from_new_local_peer->StopProxying(
          proxied_inbound_sequence_length, proxied_outbound_sequence_length);
    }
    return true;
  }

  {
    // Ensure no more parcels get sent to the current outward link.
    absl::MutexLock lock(&mutex_);
    outward_.decaying_proxy_link = std::move(outward_.link);
    outward_.sequence_length_to_decaying_link = outbound_sequence_length_;
  }
  mem::Ref<NodeLink> new_peer_node =
      requesting_node_link.node()->GetLink(proxy_peer_node_name);
  if (new_peer_node) {
    new_peer_node->BypassProxy(requesting_node_link.remote_node_name(),
                               proxy_peer_routing_id, bypass_key,
                               mem::WrapRefCounted(this));
    return true;
  }

  requesting_node_link.node()->EstablishLink(
      proxy_peer_node_name,
      [requesting_node_name = requesting_node_link.remote_node_name(),
       proxy_peer_routing_id, bypass_key,
       self = mem::WrapRefCounted(this)](NodeLink* new_link) {
        if (!new_link) {
          // TODO: failure to connect to a node here should result in route
          // destruction. This is not the same as closure since we can't make
          // any guarantees about parcel delivery up to the last transmitted
          // sequence number.
          return;
        }

        new_link->BypassProxy(requesting_node_name, proxy_peer_routing_id,
                              bypass_key, self);
      });
  return true;
}

bool Router::BypassProxyTo(mem::Ref<RouterLink> new_peer,
                           absl::uint128 bypass_key,
                           SequenceNumber proxy_outbound_sequence_length) {
  SequenceNumber proxy_inbound_sequence_length;
  mem::Ref<RouterLink> decaying_outward_link_to_proxy;
  {
    absl::MutexLock lock(&mutex_);
    if (!outward_.link) {
      // TODO: terminate the route. not the same as closure.
      return true;
    }

    RouterLinkState::Locked state(outward_.link->GetLinkState(), side_);
    if (state.other_side().bypass_key != bypass_key ||
        !state.other_side().is_decaying) {
      return false;
    }

    decaying_outward_link_to_proxy = std::move(outward_.link);
    outward_.decaying_proxy_link = decaying_outward_link_to_proxy;
    outward_.sequence_length_to_decaying_link = outbound_sequence_length_;
    outward_.sequence_length_from_decaying_link =
        proxy_outbound_sequence_length;
    outward_.link = std::move(new_peer);

    proxy_inbound_sequence_length = outbound_sequence_length_;
  }

  decaying_outward_link_to_proxy->StopProxying(proxy_inbound_sequence_length,
                                               proxy_outbound_sequence_length);
  return true;
}

void Router::Flush() {
  mem::Ref<RouterLink> inward_link;
  mem::Ref<RouterLink> outward_link;
  mem::Ref<RouterLink> decaying_inward_proxy;
  mem::Ref<RouterLink> decaying_outward_proxy;
  absl::InlinedVector<Parcel, 2> outbound_parcels;
  absl::InlinedVector<Parcel, 2> outbound_parcels_to_proxy;
  absl::InlinedVector<Parcel, 2> inbound_parcels;
  absl::InlinedVector<Parcel, 2> inbound_parcels_to_proxy;
  bool inward_proxy_decayed = false;
  bool outward_proxy_decayed = false;
  {
    Parcel parcel;
    absl::MutexLock lock(&mutex_);
    inward_link = inward_.link;
    outward_link = outward_.link;
    decaying_inward_proxy = inward_.decaying_proxy_link;
    decaying_outward_proxy = outward_.decaying_proxy_link;

    // Flush any outbound parcels destined for a decaying proxy.
    while (outward_.parcels.HasNextParcel() && outward_.decaying_proxy_link &&
           outward_.parcels.current_sequence_number() <
               *outward_.sequence_length_to_decaying_link) {
      bool ok = outward_.parcels.Pop(parcel);
      ABSL_ASSERT(ok);
      outbound_parcels_to_proxy.push_back(std::move(parcel));
    }

    // Check now if we can wipe out our decaying outward link.
    const bool still_sending_to_outward_proxy =
        decaying_outward_proxy &&
        outward_.parcels.current_sequence_number() <
            *outward_.sequence_length_to_decaying_link;
    if (decaying_outward_proxy && !still_sending_to_outward_proxy) {
      const bool still_receiving_from_outward_proxy =
          (!outward_.sequence_length_from_decaying_link ||
           inward_.parcels.GetAvailableSequenceLength() <
               *outward_.sequence_length_from_decaying_link);
      if (!still_receiving_from_outward_proxy) {
        outward_proxy_decayed = true;
        outward_.decaying_proxy_link.reset();
        outward_.sequence_length_to_decaying_link.reset();
        outward_.sequence_length_from_decaying_link.reset();
      }
    }

    // Though we may or may not still have a decaying outward link, if our
    // outbound parcel queue is no longer routing parcels there, we may proceed
    // to forward outbound parcels to our current outward link.
    if (outward_link && !outbound_transmission_paused_ &&
        (!decaying_outward_proxy || !still_sending_to_outward_proxy)) {
      while (outward_.parcels.Pop(parcel)) {
        outbound_parcels.push_back(std::move(parcel));
      }
    }

    // Now flush any outbound parcels destined for a decaying proxy.
    while (inward_.parcels.HasNextParcel() && inward_.decaying_proxy_link &&
           inward_.parcels.current_sequence_number() <
               *inward_.sequence_length_to_decaying_link) {
      bool ok = inward_.parcels.Pop(parcel);
      ABSL_ASSERT(ok);
      inbound_parcels_to_proxy.push_back(std::move(parcel));
    }

    // Check now if we can wipe out our decaying inward link.
    const bool still_sending_to_inward_proxy =
        decaying_inward_proxy && inward_.parcels.current_sequence_number() <
                                     *inward_.sequence_length_to_decaying_link;
    if (decaying_inward_proxy && !still_sending_to_inward_proxy) {
      const bool still_receiving_from_inward_proxy =
          outward_.parcels.GetAvailableSequenceLength() <
          *inward_.sequence_length_from_decaying_link;
      if (!still_receiving_from_inward_proxy) {
        inward_proxy_decayed = true;
        inward_.decaying_proxy_link.reset();
        inward_.sequence_length_to_decaying_link.reset();
        inward_.sequence_length_from_decaying_link.reset();
      }
    }

    // Finally, although we may or may not still have a decaying inward link, if
    // our inbound parcel queue is no longer routing parcels there we may
    // proceed to forward inbound parcels to our current inward link.
    if (inward_link &&
        (!decaying_inward_proxy || !still_sending_to_inward_proxy)) {
      while (inward_.parcels.Pop(parcel)) {
        inbound_parcels.push_back(std::move(parcel));
      }
    }
  }

  for (Parcel& parcel : outbound_parcels_to_proxy) {
    decaying_outward_proxy->AcceptParcel(parcel);
  }

  for (Parcel& parcel : outbound_parcels) {
    outward_link->AcceptParcel(parcel);
  }

  for (Parcel& parcel : inbound_parcels_to_proxy) {
    decaying_inward_proxy->AcceptParcel(parcel);
  }

  for (Parcel& parcel : inbound_parcels) {
    inward_link->AcceptParcel(parcel);
  }

  if (outward_proxy_decayed) {
    // May delete `this`, as the route binding for `decaying_outward_proxy` may
    // constitute the last reference to this Router.
    decaying_outward_proxy->Deactivate();
  }

  if (inward_proxy_decayed) {
    // May delete `this`, as the route binding for `decaying_inward_proxy` may
    // constitute the last reference to this Router.
    decaying_inward_proxy->Deactivate();
  }
}

Router::RouterSide::RouterSide() = default;

Router::RouterSide::~RouterSide() = default;

}  // namespace core
}  // namespace ipcz
