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
#include "core/router_observer.h"
#include "core/routing_id.h"
#include "core/routing_mode.h"
#include "core/sequence_number.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "util/two_mutex_lock.h"

namespace ipcz {
namespace core {

Router::Router(Side side) : side_(side) {}

Router::~Router() = default;

void Router::BindToPortal(mem::Ref<RouterObserver> observer,
                          IpczPortalStatus& initial_status) {
  absl::MutexLock lock(&mutex_);
  observer_ = std::move(observer);
  if (incoming_parcels_.peer_sequence_length().has_value()) {
    initial_status.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
  }
  if (incoming_parcels_.IsDead()) {
    initial_status.flags |= IPCZ_PORTAL_STATUS_DEAD;
  }
  initial_status.num_local_parcels = incoming_parcels_.GetNumAvailableParcels();
  initial_status.num_local_bytes = incoming_parcels_.GetNumAvailableBytes();
}

void Router::Unbind() {
  absl::MutexLock lock(&mutex_);
  observer_.reset();
}

mem::Ref<RouterObserver> Router::GetObserver() {
  absl::MutexLock lock(&mutex_);
  return observer_;
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
    if (routing_mode_ == RoutingMode::kBuffering) {
      return buffered_parcels_.size() < limits.max_queued_parcels &&
             buffered_parcels_.data_size() <=
                 limits.max_queued_bytes + data_size;
    }

    ABSL_ASSERT(routing_mode_ == RoutingMode::kActive);
    link = peer_ ? peer_ : predecessor_;
  }

  ABSL_ASSERT(link);
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
    if (routing_mode_ == RoutingMode::kBuffering) {
      buffered_parcels_.push(std::move(parcel));
      return IPCZ_RESULT_OK;
    }

    ABSL_ASSERT(routing_mode_ == RoutingMode::kActive);
    link = peer_ ? peer_ : predecessor_;
  }

  ABSL_ASSERT(link);
  link->AcceptParcel(parcel);
  return IPCZ_RESULT_OK;
}

void Router::CloseRoute() {
  mem::Ref<RouterLink> someone_who_cares;
  SequenceNumber sequence_length;
  {
    absl::MutexLock lock(&mutex_);
    ABSL_ASSERT(routing_mode_ == RoutingMode::kBuffering ||
                routing_mode_ == RoutingMode::kActive);
    if (routing_mode_ == RoutingMode::kActive) {
      someone_who_cares = peer_ ? peer_ : predecessor_;
      sequence_length = outgoing_sequence_length_;
    }
  }

  if (someone_who_cares) {
    someone_who_cares->AcceptRouteClosure(side_, sequence_length);
  }
}

void Router::ActivateWithPeer(mem::Ref<RouterLink> link) {
  {
    absl::MutexLock lock(&mutex_);
    peer_ = link;
    ABSL_ASSERT(routing_mode_ == RoutingMode::kBuffering ||
                routing_mode_ == RoutingMode::kFullProxy);
    if (routing_mode_ == RoutingMode::kBuffering) {
      routing_mode_ = RoutingMode::kActive;
    }
  }

  FlushParcels();
}

void Router::ActivateWithPredecessor(mem::Ref<RouterLink> link) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(routing_mode_ == RoutingMode::kBuffering);
  ABSL_ASSERT(!predecessor_);
  ABSL_ASSERT(!peer_);
  ABSL_ASSERT(buffered_parcels_.empty());
  predecessor_ = link;
  routing_mode_ = RoutingMode::kActive;
}

void Router::BeginProxyingWithSuccessor(const PortalDescriptor& descriptor,
                                        mem::Ref<RouterLink> link) {
  bool is_proxying = false;
  mem::Ref<RouterLink> peer_link;
  {
    absl::MutexLock lock(&mutex_);
    ABSL_ASSERT(!successor_);
    if (descriptor.route_is_peer) {
      ABSL_ASSERT(routing_mode_ == RoutingMode::kBuffering);
      std::swap(peer_, peer_link);
      if (!incoming_parcels_.IsDead()) {
        routing_mode_ = RoutingMode::kHalfProxy;
        successor_ = link;
        is_proxying = true;
      }
    } else {
      successor_ = link;
    }
  }

  if (descriptor.route_is_peer) {
    ABSL_ASSERT(peer_link);
    mem::Ref<Router> local_peer = peer_link->GetLocalTarget();
    local_peer->ActivateWithPeer(link);
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
  mem::Ref<RouterObserver> observer;
  uint32_t num_parcels;
  uint32_t num_bytes;
  {
    absl::MutexLock lock(&mutex_);
    if (routing_mode_ == RoutingMode::kHalfProxy ||
        routing_mode_ == RoutingMode::kFullProxy) {
      ABSL_ASSERT(successor_);
      successor = successor_;
    } else {
      observer = observer_;
      if (!incoming_parcels_.Push(std::move(parcel))) {
        return false;
      }

      num_parcels = incoming_parcels_.GetNumAvailableParcels();
      num_bytes = incoming_parcels_.GetNumAvailableBytes();
    }
  }

  if (successor) {
    successor->AcceptParcel(parcel);
    return true;
  }

  if (observer) {
    observer->OnIncomingParcel(num_parcels, num_bytes);
  }
  return true;
}

bool Router::AcceptOutgoingParcel(Parcel& parcel) {
  mem::Ref<RouterLink> link;
  {
    absl::MutexLock lock(&mutex_);
    if (routing_mode_ == RoutingMode::kBuffering) {
      buffered_parcels_.push(std::move(parcel));
      return true;
    }
    link = peer_ ? peer_ : predecessor_;
  }

  ABSL_ASSERT(link);
  link->AcceptParcel(parcel);
  return true;
}

void Router::AcceptRouteClosure(Side side, SequenceNumber sequence_length) {
  if (side == side_) {
    // assert?
    return;
  }

  bool is_route_dead = false;
  mem::Ref<RouterObserver> observer;
  {
    absl::MutexLock lock(&mutex_);
    if (!observer_) {
      return;
    }
    observer = observer_;
    incoming_parcels_.SetPeerSequenceLength(sequence_length);
    is_route_dead = incoming_parcels_.IsDead();
  }

  observer->OnPeerClosed(is_route_dead);
}

IpczResult Router::GetNextIncomingParcel(void* data,
                                         uint32_t* num_bytes,
                                         IpczHandle* portals,
                                         uint32_t* num_portals,
                                         IpczOSHandle* os_handles,
                                         uint32_t* num_os_handles,
                                         IpczPortalStatus& status_after_get) {
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

  if (incoming_parcels_.IsDead()) {
    status_after_get.flags |= IPCZ_PORTAL_STATUS_DEAD;
  }
  status_after_get.num_local_parcels =
      incoming_parcels_.GetNumAvailableParcels();
  status_after_get.num_local_bytes = incoming_parcels_.GetNumAvailableBytes();

  return IPCZ_RESULT_OK;
}

mem::Ref<Router> Router::Serialize(PortalDescriptor& descriptor) {
  descriptor.side = side_;

  // The fast path for a local pair being split is to directly establish a new
  // peer link to the destination, rather than proxying. First we acquire a
  // ref to the local peer Router, if there is one.
  mem::Ref<Router> local_peer;
  {
    absl::MutexLock lock(&mutex_);
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
      routing_mode_ = RoutingMode::kBuffering;
      local_peer->routing_mode_ = RoutingMode::kBuffering;
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
  if (routing_mode_ == RoutingMode::kActive) {
    routing_mode_ = RoutingMode::kFullProxy;
  }
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
    router->incoming_parcels_.SetPeerSequenceLength(
        descriptor.closed_peer_sequence_length);
  }
  return router;
}

void Router::FlushParcels() {
  mem::Ref<RouterLink> where_to_forward_incoming_parcels;
  mem::Ref<RouterLink> where_to_forward_outgoing_parcels;
  std::forward_list<Parcel> outgoing_parcels;
  std::vector<Parcel> incoming_parcels;
  {
    absl::MutexLock lock(&mutex_);
    if (!buffered_parcels_.empty() &&
        routing_mode_ != RoutingMode::kBuffering) {
      where_to_forward_outgoing_parcels = peer_ ? peer_ : predecessor_;
      outgoing_parcels = buffered_parcels_.TakeParcels();
    }

    if (successor_) {
      incoming_parcels.reserve(incoming_parcels_.GetNumAvailableParcels());
      Parcel parcel;
      while (incoming_parcels_.Pop(parcel)) {
        incoming_parcels.push_back(std::move(parcel));
      }
      where_to_forward_incoming_parcels = successor_;
    }
  }

  for (Parcel& parcel : outgoing_parcels) {
    where_to_forward_outgoing_parcels->AcceptParcel(parcel);
  }

  for (Parcel& parcel : incoming_parcels) {
    where_to_forward_incoming_parcels->AcceptParcel(parcel);
  }
}

}  // namespace core
}  // namespace ipcz
