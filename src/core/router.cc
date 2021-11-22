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

void Router::SetObserver(mem::Ref<RouterObserver> observer) {
  absl::MutexLock lock(&mutex_);
  observer_ = std::move(observer);
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
    RoutingMode routing_mode = RoutingMode::kClosed;
    absl::MutexLock lock(&mutex_);
    std::swap(routing_mode, routing_mode_);
    ABSL_ASSERT(routing_mode == RoutingMode::kBuffering ||
                routing_mode == RoutingMode::kActive);
    if (routing_mode == RoutingMode::kActive) {
      someone_who_cares = peer_ ? peer_ : predecessor_;
      sequence_length = outgoing_sequence_length_;
    }
  }

  if (someone_who_cares) {
    someone_who_cares->AcceptRouteClosure(side_, sequence_length);
  }
}

void Router::ActivateWithPeer(mem::Ref<RouterLink> link) {
  std::forward_list<Parcel> parcels;
  {
    absl::MutexLock lock(&mutex_);
    ABSL_ASSERT(routing_mode_ == RoutingMode::kBuffering);
    peer_ = link;
    routing_mode_ = RoutingMode::kActive;
    parcels = buffered_parcels_.TakeParcels();
  }

  for (Parcel& parcel : parcels) {
    link->AcceptParcel(parcel);
  }
}

void Router::ActivateWithPredecessor(mem::Ref<RouterLink> link) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(!predecessor_);
  ABSL_ASSERT(!peer_);
  ABSL_ASSERT(routing_mode_ == RoutingMode::kBuffering);
  ABSL_ASSERT(buffered_parcels_.empty());
  predecessor_ = link;
  routing_mode_ = RoutingMode::kActive;
}

void Router::BeginProxyingWithSuccessor(mem::Ref<RouterLink> link) {
  {
    absl::MutexLock lock(&mutex_);
    ABSL_ASSERT(!successor_);
    ABSL_ASSERT(routing_mode_ == RoutingMode::kHalfProxy ||
                routing_mode_ == RoutingMode::kFullProxy);
    ABSL_ASSERT(predecessor_ || routing_mode_ == RoutingMode::kHalfProxy);
    successor_ = std::move(link);
  }

  FlushProxiedParcels();
}

void Router::BeginProxyingWithSuccessorAndUpdateLocalPeer(
    mem::Ref<RouterLink> link) {
  bool is_proxying = false;
  mem::Ref<RouterLink> peer_link;
  {
    absl::MutexLock lock(&mutex_);
    ABSL_ASSERT(!successor_);
    ABSL_ASSERT(routing_mode_ == RoutingMode::kBuffering);
    peer_link = peer_;
    peer_ = nullptr;
    if (!incoming_parcels_.IsDead()) {
      routing_mode_ = RoutingMode::kHalfProxy;
      successor_ = link;
      is_proxying = true;
    }
  }

  ABSL_ASSERT(peer_link);
  mem::Ref<Router> local_peer = peer_link->GetLocalTarget();
  local_peer->ActivateWithPeer(std::move(link));

  if (is_proxying) {
    FlushProxiedParcels();
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
  // TODO
  return false;
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
                                         uint32_t* num_os_handles) {
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
    if (local_peer->peer_ && local_peer->peer_->GetLocalTarget() == this) {
      // Temporarily place the peer in buffering mode so that it can't transmit
      // any parcels to its new remote peer until this descriptor is
      // transmitted, since the remote peer doesn't exist until then.
      routing_mode_ = RoutingMode::kBuffering;
      local_peer->routing_mode_ = RoutingMode::kBuffering;
      local_peer->peer_ = nullptr;
      ABSL_ASSERT(local_peer->buffered_parcels_.empty());
      incoming_parcels_.SetPeerSequenceLength(
          local_peer->outgoing_sequence_length_);
      descriptor.route_is_peer = true;
      return local_peer;
    }
  }

  absl::MutexLock lock(&mutex_);
  routing_mode_ = RoutingMode::kFullProxy;
  descriptor.route_is_peer = false;
  return mem::WrapRefCounted(this);
}

void Router::FlushProxiedParcels() {
  mem::Ref<RouterLink> where_to_forward_incoming_parcels;
  mem::Ref<RouterLink> where_to_forward_outgoing_parcels;
  std::forward_list<Parcel> outgoing_parcels;
  std::vector<Parcel> incoming_parcels;
  {
    absl::MutexLock lock(&mutex_);
    ABSL_ASSERT(routing_mode_ == RoutingMode::kFullProxy ||
                routing_mode_ == RoutingMode::kHalfProxy);
    if (!buffered_parcels_.empty()) {
      ABSL_ASSERT(routing_mode_ == RoutingMode::kFullProxy);
      where_to_forward_outgoing_parcels = peer_ ? peer_ : predecessor_;
      outgoing_parcels = buffered_parcels_.TakeParcels();
    }

    ABSL_ASSERT(successor_);
    incoming_parcels.reserve(incoming_parcels_.GetNumAvailableParcels());
    Parcel parcel;
    while (incoming_parcels_.Pop(parcel)) {
      incoming_parcels.push_back(std::move(parcel));
    }
    where_to_forward_incoming_parcels = successor_;
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
