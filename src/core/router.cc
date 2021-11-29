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
#include "core/outgoing_parcel_queue.h"
#include "core/parcel.h"
#include "core/portal_descriptor.h"
#include "core/remote_router_link.h"
#include "core/router_link.h"
#include "core/router_link_state.h"
#include "core/routing_id.h"
#include "core/routing_mode.h"
#include "core/sequence_number.h"
#include "core/trap.h"
#include "core/trap_event_dispatcher.h"
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
    ABSL_ASSERT(outward_transmission_paused_ != paused);
    outward_transmission_paused_ = paused;
  }

  if (!paused) {
    FlushParcels();
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
    parcel.set_sequence_number(outward_sequence_length_++);
    link = outward_.link;
    if (!link || outward_transmission_paused_) {
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
    outward_.parcels = IncomingParcelQueue(outward_sequence_length_);
    outward_.parcels.SetPeerSequenceLength(outward_sequence_length_);
    if (outward_transmission_paused_) {
      return;
    }
    forwarding_link = outward_.link;
    final_sequence_length = outward_sequence_length_;
  }

  forwarding_link->AcceptRouteClosure(side_, final_sequence_length);
}

void Router::SetOutwardLink(mem::Ref<RouterLink> link) {
  {
    absl::MutexLock lock(&mutex_);
    outward_.link = std::move(link);
  }

  FlushParcels();
}

void Router::BeginProxying(const PortalDescriptor& descriptor,
                           mem::Ref<RouterLink> link) {
  mem::Ref<RouterLink> outward_link;
  {
    absl::MutexLock lock(&mutex_);
    ABSL_ASSERT(!inward_.link);
    if (descriptor.route_is_peer) {
      std::swap(outward_.link, outward_link);
      if (!inward_.parcels.IsDead()) {
        inward_.link = link;
      }
    } else {
      inward_.link = link;
    }
  }

  if (descriptor.route_is_peer) {
    ABSL_ASSERT(outward_link);
    mem::Ref<Router> local_peer = outward_link->GetLocalTarget();
    local_peer->SetOutwardLink(link);
  }

  FlushParcels();
}

bool Router::StopProxying(SequenceNumber inward_sequence_length,
                          SequenceNumber outward_sequence_length) {
  {
    absl::MutexLock lock(&mutex_);
    inward_.parcels.SetPeerSequenceLength(inward_sequence_length);
    outward_.parcels.SetPeerSequenceLength(outward_sequence_length);
  }

  FlushParcels();
  return true;
}

bool Router::AcceptParcelFrom(NodeLink& link,
                              RoutingId routing_id,
                              Parcel& parcel) {
  bool is_outbound;
  {
    absl::MutexLock lock(&mutex_);
    if ((outward_.link && outward_.link->IsRemoteLinkTo(link, routing_id)) ||
        (outward_.decaying_link &&
         outward_.decaying_link->IsRemoteLinkTo(link, routing_id))) {
      is_outbound = true;
    } else if ((inward_.link &&
                inward_.link->IsRemoteLinkTo(link, routing_id)) ||
               (inward_.decaying_link &&
                inward_.decaying_link->IsRemoteLinkTo(link, routing_id))) {
      is_outbound = false;
    } else {
      return false;
    }
  }

  if (is_outbound) {
    return AcceptOutboundParcel(parcel);
  }

  return AcceptInboundParcel(parcel);
}

bool Router::AcceptInboundParcel(Parcel& parcel) {
  TrapEventDispatcher dispatcher;
  {
    absl::MutexLock lock(&mutex_);
    if (!inward_.parcels.Push(std::move(parcel))) {
      return false;
    }

    if (!inward_.link) {
      status_.num_local_parcels = inward_.parcels.GetNumAvailableParcels();
      status_.num_local_bytes = inward_.parcels.GetNumAvailableBytes();
      traps_.MaybeNotify(dispatcher, status_);
    }
  }

  FlushParcels();
  return true;
}

bool Router::AcceptOutboundParcel(Parcel& parcel) {
  {
    absl::MutexLock lock(&mutex_);
    if (!outward_.parcels.Push(std::move(parcel))) {
      return false;
    }

    if (!outward_.link || outward_transmission_paused_) {
      return true;
    }
  }

  FlushParcels();
  return true;
}

void Router::AcceptRouteClosure(Side side, SequenceNumber sequence_length) {
  TrapEventDispatcher dispatcher;
  mem::Ref<RouterLink> forwarding_link;
  if (side == side_) {
    absl::MutexLock lock(&mutex_);
    outward_.parcels.SetPeerSequenceLength(sequence_length);
    if (!outward_.closure_propagated && !outward_transmission_paused_ &&
        outward_.link) {
      forwarding_link = outward_.link;
      outward_.closure_propagated = true;
    }
  } else {
    absl::MutexLock lock(&mutex_);
    inward_.parcels.SetPeerSequenceLength(sequence_length);
    if (!inward_.closure_propagated && inward_.link) {
      forwarding_link = inward_.link;
      inward_.closure_propagated = true;
    }
    status_.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
    if (inward_.parcels.IsDead()) {
      status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
    }
    traps_.MaybeNotify(dispatcher, status_);
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
        // Temporarily remove the peer's outward link so that it doesn't
        // transmit any parcels to the new destination yet.
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
              local_peer->outward_sequence_length_);
        }
        descriptor.route_is_peer = true;
        descriptor.next_outgoing_sequence_number = outward_sequence_length_;
        descriptor.next_incoming_sequence_number =
            inward_.parcels.current_sequence_number();
        return local_peer;
      }
    } else if (outward_.link && outward_.link->GetLocalTarget()) {
      // We didn't have a local peer before, but now we do. Try again.
      continue;
    }

    descriptor.route_is_peer = false;
    descriptor.next_outgoing_sequence_number = outward_sequence_length_;
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
               !inward_.link && !outward_.decaying_link &&
               !inward_.decaying_link) {
      // We only need to prepare the new router for proxy bypass if our own peer
      // is remote to us.
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
mem::Ref<Router> Router::Deserialize(const PortalDescriptor& descriptor) {
  auto router = mem::MakeRefCounted<Router>(descriptor.side);
  absl::MutexLock lock(&router->mutex_);
  router->outward_sequence_length_ = descriptor.next_outgoing_sequence_number;
  router->inward_.parcels =
      IncomingParcelQueue(descriptor.next_incoming_sequence_number);
  if (descriptor.peer_closed) {
    router->status_.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
    router->inward_.parcels.SetPeerSequenceLength(
        descriptor.closed_peer_sequence_length);
    if (router->inward_.parcels.IsDead()) {
      router->status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
    }
  }

  return router;
}

bool Router::InitiateProxyBypass(NodeLink& requesting_node_link,
                                 RoutingId requesting_routing_id,
                                 const NodeName& proxy_peer_node_name,
                                 RoutingId proxy_peer_routing_id,
                                 absl::uint128 bypass_key) {
  SequenceNumber outward_sequence_length;
  mem::Ref<RouterLink> outward_link;
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

    outward_link = outward_.link;
    outward_sequence_length = outward_sequence_length_;
  }

  if (proxy_peer_node_name == requesting_node_link.node()->name()) {
    mem::Ref<RouterLink> old_peer;
    mem::Ref<Router> new_peer =
        requesting_node_link.GetRouter(proxy_peer_routing_id);
    SequenceNumber inward_sequence_length;
    ABSL_ASSERT(new_peer->side_ == Opposite(side_));
    {
      TwoMutexLock lock(&mutex_, &new_peer->mutex_);
      inward_sequence_length = new_peer->outward_sequence_length_;
      mem::Ref<Router> left =
          side_ == Side::kLeft ? mem::WrapRefCounted(this) : new_peer;
      mem::Ref<Router> right =
          side_ == Side::kLeft ? new_peer : mem::WrapRefCounted(this);
      ABSL_ASSERT(left != right);
      left->mutex_.AssertHeld();
      right->mutex_.AssertHeld();
      mem::Ref<RouterLink> left_link;
      mem::Ref<RouterLink> right_link;
      std::tie(left_link, right_link) =
          LocalRouterLink::CreatePair(left, right);
      old_peer = std::move(new_peer->outward_.link);
      left->outward_.link = std::move(left_link);
      right->outward_.link = std::move(right_link);
      outward_.decaying_link = old_peer;
      outward_.decaying_link_sequence_length = inward_sequence_length;
    }

    if (old_peer) {
      old_peer->StopProxying(inward_sequence_length, outward_sequence_length);
      old_peer->Deactivate();
    }
    return true;
  }

  mem::Ref<NodeLink> new_peer_node =
      requesting_node_link.node()->GetLink(proxy_peer_node_name);
  if (new_peer_node) {
    new_peer_node->BypassProxy(requesting_node_link.remote_node_name(),
                               proxy_peer_routing_id, outward_sequence_length,
                               bypass_key, mem::WrapRefCounted(this));
    return true;
  }

  requesting_node_link.node()->EstablishLink(
      proxy_peer_node_name,
      [requesting_node_name = requesting_node_link.remote_node_name(),
       proxy_peer_routing_id, outward_sequence_length, bypass_key,
       self = mem::WrapRefCounted(this)](NodeLink* new_link) {
        if (!new_link) {
          // TODO: failure to connect to a node here should result in route
          // destruction. This is not the same as closure since we can't make
          // any guarantees about parcel delivery up to the last transmitted
          // sequence number.
          return;
        }

        new_link->BypassProxy(requesting_node_name, proxy_peer_routing_id,
                              outward_sequence_length, bypass_key, self);
      });
  return true;
}

bool Router::BypassProxyTo(mem::Ref<RouterLink> new_peer,
                           absl::uint128 bypass_key,
                           SequenceNumber proxy_outward_sequence_length) {
  SequenceNumber proxy_inward_sequence_length;
  mem::Ref<RouterLink> old_peer;
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

    old_peer = std::move(outward_.link);
    outward_.decaying_link = old_peer;
    outward_.link = std::move(new_peer);
    proxy_inward_sequence_length = outward_sequence_length_;
  }

  old_peer->StopProxying(proxy_inward_sequence_length,
                         proxy_outward_sequence_length);
  return true;
}

void Router::FlushParcels() {
  mem::Ref<RouterLink> inward_forwarding_link;
  mem::Ref<RouterLink> outward_forwarding_link;
  absl::InlinedVector<Parcel, 4> outbound_parcels;
  absl::InlinedVector<Parcel, 4> inbound_parcels;
  bool outward_sequence_dead = false;
  bool inward_sequence_dead = false;
  {
    absl::MutexLock lock(&mutex_);
    inward_forwarding_link = inward_.link;
    outward_forwarding_link = outward_.link;
    if (outward_.parcels.HasNextParcel() && outward_forwarding_link &&
        !outward_transmission_paused_) {
      Parcel parcel;
      while (outward_.parcels.Pop(parcel)) {
        outbound_parcels.push_back(std::move(parcel));
      }
      outward_sequence_dead = outward_.parcels.IsDead();
    }

    if (inward_.parcels.HasNextParcel() && inward_forwarding_link) {
      Parcel parcel;
      while (inward_.parcels.Pop(parcel)) {
        inbound_parcels.push_back(std::move(parcel));
      }
      inward_sequence_dead = inward_.parcels.IsDead();
    }
  }

  for (Parcel& parcel : outbound_parcels) {
    outward_forwarding_link->AcceptParcel(parcel);
  }

  for (Parcel& parcel : inbound_parcels) {
    inward_forwarding_link->AcceptParcel(parcel);
  }

  if (inward_sequence_dead) {
    inward_forwarding_link->Deactivate();

    absl::MutexLock lock(&mutex_);
    inward_.link = nullptr;
  }

  if (outward_sequence_dead) {
    outward_forwarding_link->Deactivate();

    absl::MutexLock lock(&mutex_);
    outward_.link = nullptr;
  }
}

Router::RouterSide::RouterSide() = default;

Router::RouterSide::~RouterSide() = default;

}  // namespace core
}  // namespace ipcz
