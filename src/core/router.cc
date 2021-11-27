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

#include "core/outgoing_parcel_queue.h"
#include "core/parcel.h"
#include "core/portal_descriptor.h"
#include "core/router_link.h"
#include "core/routing_id.h"
#include "core/routing_mode.h"
#include "core/sequence_number.h"
#include "core/trap.h"
#include "core/trap_event_dispatcher.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "util/two_mutex_lock.h"

namespace ipcz {
namespace core {

Router::Router(Side side) : side_(side) {}

Router::~Router() = default;

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
      return buffered_parcels_.size() < limits.max_queued_parcels &&
             buffered_parcels_.data_size() <=
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
    if (!link) {
      buffered_parcels_.push(std::move(parcel));
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
    ABSL_ASSERT(buffered_parcels_.empty());
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
  if (incoming_parcels_.peer_sequence_length().has_value() &&
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
    if (!link) {
      buffered_parcels_.push(std::move(parcel));
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

  if (local_peer) {
    // Note that by the time we acquire both locks here, the pair may have been
    // split by another thread serializing the peer for transmission elsewhere,
    // so we need to first verify that the pair is still intact. If it's not we
    // fall back to the normal proxying path.
    TwoMutexLock lock(local_peer->mutex_, mutex_);
    if (peer_ == local_peer && local_peer->peer_ &&
        local_peer->peer_->GetLocalTarget() == this) {
      // Temporarily place the peer in buffering mode so that it can't transmit
      // any parcels to its new remote peer until this descriptor is
      // transmitted, since the remote peer doesn't exist until then.
      local_peer->peer_ = nullptr;
      ABSL_ASSERT(local_peer->buffered_parcels_.empty());
      incoming_parcels_.SetPeerSequenceLength(
          local_peer->outgoing_sequence_length_);
      if (incoming_parcels_.peer_sequence_length()) {
        descriptor.peer_closed = true;
        descriptor.closed_peer_sequence_length =
            *incoming_parcels_.peer_sequence_length();
        peer_closure_propagated_ = true;
      }
      descriptor.route_is_peer = true;
      descriptor.next_outgoing_sequence_number = outgoing_sequence_length_;
      descriptor.next_incoming_sequence_number =
          incoming_parcels_.current_sequence_number();
      return local_peer;
    }
  }

  absl::MutexLock lock(&mutex_);
  descriptor.route_is_peer = false;
  descriptor.next_outgoing_sequence_number = outgoing_sequence_length_;
  descriptor.next_incoming_sequence_number =
      incoming_parcels_.current_sequence_number();
  if (incoming_parcels_.peer_sequence_length()) {
    descriptor.peer_closed = true;
    descriptor.closed_peer_sequence_length =
        *incoming_parcels_.peer_sequence_length();
    peer_closure_propagated_ = true;
  }
  return mem::WrapRefCounted(this);
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

void Router::FlushParcels() {
  mem::Ref<RouterLink> where_to_forward_incoming_parcels;
  mem::Ref<RouterLink> where_to_forward_outgoing_parcels;
  std::forward_list<Parcel> outgoing_parcels;
  std::vector<Parcel> incoming_parcels;
  bool incoming_queue_dead = false;
  {
    absl::MutexLock lock(&mutex_);
    where_to_forward_outgoing_parcels = peer_ ? peer_ : predecessor_;
    if (!buffered_parcels_.empty() && where_to_forward_outgoing_parcels) {
      outgoing_parcels = buffered_parcels_.TakeParcels();
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
