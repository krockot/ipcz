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
#include "third_party/abseil-cpp/absl/numeric/int128.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "util/random.h"
#include "util/two_mutex_lock.h"

namespace ipcz {
namespace core {

Router::Router(Side side) : side_(side) {}

Router::~Router() = default;

void Router::PauseOutgoingTransmission(bool paused) {
  {
    absl::MutexLock lock(&mutex_);
    if (paused) {
      num_outgoing_transmission_blockers_++;
    } else {
      ABSL_ASSERT(num_outgoing_transmission_blockers_ > 0);
      num_outgoing_transmission_blockers_--;
    }
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
  return peer_ && peer_->GetLocalTarget() == router;
}

bool Router::WouldOutgoingParcelExceedLimits(size_t data_size,
                                             const IpczPutLimits& limits) {
  mem::Ref<RouterLink> link;
  {
    absl::MutexLock lock(&mutex_);
    link = peer_ ? peer_ : predecessor_;
    if (!link) {
      return outgoing_parcels_.size() < limits.max_queued_parcels &&
             outgoing_parcels_.data_size() <=
                 limits.max_queued_bytes + data_size;
    }
  }

  return link->WouldParcelExceedLimits(data_size, limits);
}

bool Router::WouldIncomingParcelExceedLimits(size_t data_size,
                                             const IpczPutLimits& limits) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(routing_mode_ == RoutingMode::kActive);
  return incoming_parcels_.GetNumAvailableBytes() + data_size >
             limits.max_queued_bytes &&
         incoming_parcels_.GetNumAvailableParcels() >=
             limits.max_queued_parcels;
}

IpczResult Router::SendOutgoingParcel(absl::Span<const uint8_t> data,
                                      Parcel::PortalVector& portals,
                                      std::vector<os::Handle>& os_handles) {
  Parcel parcel;
  parcel.SetData(std::vector<uint8_t>(data.begin(), data.end()));
  parcel.SetPortals(std::move(portals));
  parcel.SetOSHandles(std::move(os_handles));

  mem::Ref<RouterLink> link;
  {
    absl::MutexLock lock(&mutex_);
    parcel.set_sequence_number(outgoing_sequence_length_++);
    link = peer_ ? peer_ : predecessor_;
    if (!link || num_outgoing_transmission_blockers_) {
      outgoing_parcels_.push(std::move(parcel));
      return IPCZ_RESULT_OK;
    }
  }

  link->AcceptParcel(parcel);
  return IPCZ_RESULT_OK;
}

void Router::CloseRoute() {
  mem::Ref<RouterLink> peer;
  mem::Ref<RouterLink> predecessor;
  SequenceNumber sequence_length;
  std::vector<Parcel> incoming_parcels;
  {
    absl::MutexLock lock(&mutex_);
    ABSL_ASSERT(routing_mode_ == RoutingMode::kActive);
    sequence_length = outgoing_sequence_length_;
    std::swap(peer, peer_);
    std::swap(predecessor, predecessor_);
    std::move(incoming_parcels_).StealAllParcels(incoming_parcels);
  }

  mem::Ref<RouterLink> target = peer ? peer : predecessor;
  if (target) {
    target->AcceptRouteClosure(side_, sequence_length);
  }

  if (peer) {
    peer->Deactivate();
  }

  if (predecessor) {
    predecessor->Deactivate();
  }
}

void Router::SetPeer(mem::Ref<RouterLink> link) {
  {
    absl::MutexLock lock(&mutex_);
    ABSL_ASSERT(routing_mode_ == RoutingMode::kActive ||
                routing_mode_ == RoutingMode::kProxy);
    peer_ = link;
  }

  FlushParcels();
}

void Router::SetPredecessor(mem::Ref<RouterLink> link) {
  {
    absl::MutexLock lock(&mutex_);
    ABSL_ASSERT(routing_mode_ == RoutingMode::kActive);
    ABSL_ASSERT(!predecessor_);
    ABSL_ASSERT(!peer_);
    ABSL_ASSERT(outgoing_parcels_.empty());
    predecessor_ = link;
  }

  FlushParcels();
}

void Router::BeginProxyingWithSuccessor(const PortalDescriptor& descriptor,
                                        mem::Ref<RouterLink> link) {
  mem::Ref<RouterLink> peer_link;
  {
    absl::MutexLock lock(&mutex_);
    ABSL_ASSERT(!successor_);
    if (descriptor.route_is_peer) {
      ABSL_ASSERT(routing_mode_ == RoutingMode::kActive);
      std::swap(peer_, peer_link);
      if (!incoming_parcels_.IsDead()) {
        routing_mode_ = RoutingMode::kHalfProxy;
        successor_ = link;
      }
    } else {
      successor_ = link;
      routing_mode_ = RoutingMode::kProxy;
    }
  }

  if (descriptor.route_is_peer) {
    ABSL_ASSERT(peer_link);
    mem::Ref<Router> local_peer = peer_link->GetLocalTarget();
    local_peer->SetPeer(link);
  }

  FlushParcels();

  absl::optional<SequenceNumber> peer_sequence_length;
  absl::MutexLock lock(&mutex_);
  if (status_.flags & IPCZ_PORTAL_STATUS_PEER_CLOSED &&
      !peer_closure_propagated_) {
    peer_closure_propagated_ = true;
    peer_sequence_length = incoming_parcels_.peer_sequence_length();
  }

  if (peer_sequence_length) {
    link->AcceptRouteClosure(Opposite(side_), *peer_sequence_length);
  }
}

bool Router::AcceptParcelFrom(NodeLink& link,
                              RoutingId routing_id,
                              Parcel& parcel) {
  bool is_incoming = false;
  {
    absl::MutexLock lock(&mutex_);
    if (peer_ && peer_->IsRemoteLinkTo(link, routing_id)) {
      is_incoming = true;
    } else if (predecessor_ && predecessor_->IsRemoteLinkTo(link, routing_id)) {
      is_incoming = true;
    } else if (!successor_ || !successor_->IsRemoteLinkTo(link, routing_id)) {
      return false;
    }
  }

  if (is_incoming) {
    return AcceptIncomingParcel(parcel);
  }

  return AcceptOutgoingParcel(parcel);
}

bool Router::AcceptIncomingParcel(Parcel& parcel) {
  mem::Ref<RouterLink> successor;
  TrapEventDispatcher dispatcher;
  {
    absl::MutexLock lock(&mutex_);
    if (!incoming_parcels_.Push(std::move(parcel))) {
      return false;
    }

    status_.num_local_parcels = incoming_parcels_.GetNumAvailableParcels();
    status_.num_local_bytes = incoming_parcels_.GetNumAvailableBytes();
    traps_.MaybeNotify(dispatcher, status_);
  }

  FlushParcels();
  return true;
}

bool Router::AcceptOutgoingParcel(Parcel& parcel) {
  mem::Ref<RouterLink> link;
  {
    absl::MutexLock lock(&mutex_);
    link = peer_ ? peer_ : predecessor_;
    if (!link || num_outgoing_transmission_blockers_) {
      outgoing_parcels_.push(std::move(parcel));
      return true;
    }
  }

  link->AcceptParcel(parcel);
  return true;
}

void Router::AcceptRouteClosure(Side side, SequenceNumber sequence_length) {
  if (side == side_) {
    // assert?
    return;
  }

  TrapEventDispatcher dispatcher;
  {
    absl::MutexLock lock(&mutex_);
    incoming_parcels_.SetPeerSequenceLength(sequence_length);
    status_.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
    if (incoming_parcels_.IsDead()) {
      status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
    }
    traps_.MaybeNotify(dispatcher, status_);
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
  if (!incoming_parcels_.HasNextParcel()) {
    if (incoming_parcels_.IsDead()) {
      return IPCZ_RESULT_NOT_FOUND;
    }
    return IPCZ_RESULT_UNAVAILABLE;
  }

  Parcel& p = incoming_parcels_.NextParcel();
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
  incoming_parcels_.Pop(parcel);
  memcpy(data, parcel.data_view().data(), parcel.data_view().size());
  parcel.Consume(portals, os_handles);

  status_.num_local_parcels = incoming_parcels_.GetNumAvailableParcels();
  status_.num_local_bytes = incoming_parcels_.GetNumAvailableBytes();
  if (incoming_parcels_.IsDead()) {
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
  if (!incoming_parcels_.HasNextParcel()) {
    return IPCZ_RESULT_UNAVAILABLE;
  }

  Parcel& p = incoming_parcels_.NextParcel();
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
  if (!incoming_parcels_.HasNextParcel()) {
    // If ipcz is used correctly this is impossible.
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  Parcel& p = incoming_parcels_.NextParcel();
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
  bool ok = incoming_parcels_.Pop(consumed_parcel);
  ABSL_ASSERT(ok);

  status_.num_local_parcels = incoming_parcels_.GetNumAvailableParcels();
  status_.num_local_bytes = incoming_parcels_.GetNumAvailableBytes();
  if (incoming_parcels_.IsDead()) {
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
    // peer link to the destination, rather than proxying. First we acquire a
    // ref to the local peer Router, if there is one.
    mem::Ref<Router> local_peer;
    {
      absl::MutexLock lock(&mutex_);
      traps_.Clear();
      if (peer_) {
        local_peer = peer_->GetLocalTarget();
      }
    }

    TwoMutexLock lock(&mutex_, local_peer ? &local_peer->mutex_ : nullptr);
    if (local_peer) {
      // The peer has already changed. Loop back and try again,
      if (!peer_ || peer_->GetLocalTarget() != local_peer) {
        continue;
      }

      local_peer->mutex_.AssertHeld();

      // Note that by the time we acquire both locks above, the pair may have
      // been split by another thread serializing the peer for transmission
      // elsewhere, so we need to first verify that the pair is still intact. If
      // it's not we fall back to the normal proxying path below.
      if (local_peer->peer_ && local_peer->peer_->GetLocalTarget() == this) {
        // Temporarily place the peer in buffering mode so that it can't
        // transmit any parcels to its new remote peer until this descriptor is
        // transmitted, since the remote peer doesn't exist until then.
        local_peer->peer_ = nullptr;
        ABSL_ASSERT(local_peer->outgoing_parcels_.empty());
        if (incoming_parcels_.peer_sequence_length()) {
          descriptor.peer_closed = true;
          descriptor.closed_peer_sequence_length =
              *incoming_parcels_.peer_sequence_length();
          peer_closure_propagated_ = true;
        } else {
          // Fix the incoming sequence length at the last transmitted parcel, so
          // this router can be destroyed once incoming parcels up to this point
          // have been forwarded to the new peer.
          incoming_parcels_.SetPeerSequenceLength(
              local_peer->outgoing_sequence_length_);
        }
        descriptor.route_is_peer = true;
        descriptor.next_outgoing_sequence_number = outgoing_sequence_length_;
        descriptor.next_incoming_sequence_number =
            incoming_parcels_.current_sequence_number();
        return local_peer;
      }
    } else if (peer_ && peer_->GetLocalTarget()) {
      // We didn't have a local peer before, but now we do. Try again.
      continue;
    }

    descriptor.route_is_peer = false;
    descriptor.next_outgoing_sequence_number = outgoing_sequence_length_;
    descriptor.next_incoming_sequence_number =
        incoming_parcels_.current_sequence_number();
    bool half_proxy = false;
    NodeName proxy_peer_node_name;
    RoutingId proxy_peer_routing_id;
    absl::uint128 bypass_key;
    if (incoming_parcels_.peer_sequence_length()) {
      descriptor.peer_closed = true;
      descriptor.closed_peer_sequence_length =
          *incoming_parcels_.peer_sequence_length();
      peer_closure_propagated_ = true;
    } else if (peer_ && !peer_->GetLocalTarget()) {
      // We only need to prepare the new router for proxy bypass if our own peer
      // is remote to us.
      RemoteRouterLink& remote_link =
          *static_cast<RemoteRouterLink*>(peer_.get());
      proxy_peer_node_name = remote_link.node_link()->remote_node_name();
      proxy_peer_routing_id = remote_link.routing_id();
      bypass_key = RandomUint128();
      RouterLinkState::Locked locked(peer_->GetLinkState(), side_);
      if (locked.other_side().routing_mode != RoutingMode::kHalfProxy) {
        half_proxy = true;
        locked.this_side().routing_mode = RoutingMode::kHalfProxy;
        locked.this_side().bypass_key = bypass_key;
      }
    }

    if (half_proxy) {
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
  router->outgoing_sequence_length_ = descriptor.next_outgoing_sequence_number;
  router->incoming_parcels_ =
      IncomingParcelQueue(descriptor.next_incoming_sequence_number);
  if (descriptor.peer_closed) {
    router->status_.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
    router->incoming_parcels_.SetPeerSequenceLength(
        descriptor.closed_peer_sequence_length);
    if (router->incoming_parcels_.IsDead()) {
      router->status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
    }
  }

  return router;
}

bool Router::InitiateProxyBypass(NodeLink& requesting_node_link,
                                 RoutingId requesting_routing_id,
                                 const NodeName& proxy_peer_node_name,
                                 RoutingId proxy_peer_routing_id,
                                 absl::uint128 bypass_key,
                                 bool notify_predecessor) {
  mem::Ref<RouterLink> predecessor;
  {
    absl::MutexLock lock(&mutex_);
    if (!predecessor_) {
      // Must have been disconnected already.
      return true;
    }

    if (!predecessor_->IsRemoteLinkTo(requesting_node_link,
                                      requesting_routing_id)) {
      // Authenticate that the request to bypass our predecessor is actually
      // coming from our predecessor.
      return false;
    }

    if (peer_) {
      // A well-behaved system will never ask a router to bypass a proxy when
      // the router already has a peer link.
      return false;
    }

    predecessor = predecessor_;
  }

  if (proxy_peer_node_name == requesting_node_link.node()->name()) {
    // TODO: special case when the outcome of proxy bypass is this router
    // becoming locally linked to its new peer. In this case
    // proxy_peer_routing_id must identify a route on `requesting_node_link`
    // itself which corresponds to the local peer's own link to its peer (our
    // predecessor on requesting_routing_id). we can use this to locate that
    // router and fix it up with this one using a new LocalRouterLink.
    ABSL_ASSERT(false);
    return true;
  }

  mem::Ref<NodeLink> new_peer_node =
      requesting_node_link.node()->GetLink(proxy_peer_node_name);
  if (new_peer_node) {
    new_peer_node->BypassProxy(requesting_node_link.remote_node_name(),
                               proxy_peer_routing_id, side_, bypass_key,
                               mem::WrapRefCounted(this));
    return true;
  }

  // We don't want to forward outgoing parcels to our predecessor while waiting
  // for the peer link to be established.
  PauseOutgoingTransmission(true);

  requesting_node_link.node()->EstablishLink(
      proxy_peer_node_name,
      [requesting_node_name = requesting_node_link.remote_node_name(),
       proxy_peer_routing_id, side = side_, bypass_key,
       self = mem::WrapRefCounted(this)](NodeLink* new_link) {
        if (!new_link) {
          // TODO: failed to establish a link to the new peer node, so the route
          // is effectively toast. behave as in peer closure.
          return;
        }

        new_link->BypassProxy(requesting_node_name, proxy_peer_routing_id, side,
                              bypass_key, self);
        self->PauseOutgoingTransmission(false);
      });
  return true;
}

void Router::FlushParcels() {
  mem::Ref<RouterLink> where_to_forward_incoming_parcels;
  mem::Ref<RouterLink> where_to_forward_outgoing_parcels;
  std::forward_list<Parcel> outgoing_parcels;
  std::vector<Parcel> incoming_parcels;
  bool incoming_queue_dead = false;
  {
    absl::MutexLock lock(&mutex_);
    where_to_forward_outgoing_parcels = peer_ ? peer_ : predecessor_;
    if (!outgoing_parcels_.empty() && where_to_forward_outgoing_parcels &&
        !num_outgoing_transmission_blockers_) {
      outgoing_parcels = outgoing_parcels_.TakeParcels();
    }

    if (successor_) {
      incoming_parcels.reserve(incoming_parcels_.GetNumAvailableParcels());
      Parcel parcel;
      while (incoming_parcels_.Pop(parcel)) {
        incoming_parcels.push_back(std::move(parcel));
      }
      incoming_queue_dead = incoming_parcels_.IsDead();
      where_to_forward_incoming_parcels = successor_;
    }
  }

  for (Parcel& parcel : outgoing_parcels) {
    where_to_forward_outgoing_parcels->AcceptParcel(parcel);
  }

  for (Parcel& parcel : incoming_parcels) {
    where_to_forward_incoming_parcels->AcceptParcel(parcel);
  }

  if (incoming_queue_dead) {
    // TODO: unbind route receiving inbound parcels and clean up any other refs
    // we might be leaving around. either the peer is closed or we're a half
    // proxy who has outlived its usefulness.
  }
}

}  // namespace core
}  // namespace ipcz
