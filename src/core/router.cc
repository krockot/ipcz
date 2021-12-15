// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/router.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
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

std::string DescribeOptionalLength(absl::optional<SequenceNumber> length) {
  std::stringstream ss;
  if (length) {
    ss << *length;
    return ss.str();
  }
  return "none";
}

std::atomic<size_t> g_num_routers{0};

}  // namespace

Router::Router() {
  ++g_num_routers;
}

Router::~Router() {
  --g_num_routers;
}

// static
size_t Router::GetNumRoutersForTesting() {
  return g_num_routers;
}

void Router::PauseOutboundTransmission(bool paused) {
  {
    absl::MutexLock lock(&mutex_);
    ABSL_ASSERT(outward_.paused() != paused);
    outward_.set_paused(paused);
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

bool Router::HasLocalPeer(const mem::Ref<Router>& other) {
  absl::MutexLock lock(&mutex_);
  return outward_.GetLocalPeer() == other;
}

bool Router::HasStableLocalPeer(const mem::Ref<Router>& other) {
  TwoMutexLock lock(&mutex_, &other->mutex_);
  return outward_.GetLocalPeer() == other &&
         other->outward_.GetLocalPeer() == this &&
         !inward_.has_current_link() && !other->inward_.has_current_link() &&
         !outward_.has_decaying_link() && !other->outward_.has_decaying_link();
}

bool Router::WouldOutboundParcelExceedLimits(size_t data_size,
                                             const IpczPutLimits& limits) {
  mem::Ref<RouterLink> link;
  {
    absl::MutexLock lock(&mutex_);
    link = outward_.current_link();
    if (!link) {
      return outward_.parcels().GetNumAvailableParcels() <
                 limits.max_queued_parcels &&
             outward_.parcels().GetNumAvailableBytes() <=
                 limits.max_queued_bytes &&
             limits.max_queued_bytes -
                     outward_.parcels().GetNumAvailableBytes() <=
                 data_size;
    }
  }

  return link->WouldParcelExceedLimits(data_size, limits);
}

bool Router::WouldInboundParcelExceedLimits(size_t data_size,
                                            const IpczPutLimits& limits) {
  absl::MutexLock lock(&mutex_);
  return inward_.parcels().GetNumAvailableBytes() + data_size >
             limits.max_queued_bytes &&
         inward_.parcels().GetNumAvailableParcels() >=
             limits.max_queued_parcels;
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
    const SequenceNumber next_sequence_number = outward_.sequence_length();
    parcel.set_sequence_number(next_sequence_number);

    // TODO: pushing and then immediately popping a parcel is a waste of time.
    // optimize this out when we know we're a terminal router.
    DVLOG(4) << "Queuing outbound " << parcel.Describe();

    const bool push_ok = outward_.parcels().Push(std::move(parcel));
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

    traps_.DisableAllAndClear();

    ABSL_ASSERT(!inward_.has_current_link());

    if (!outward_.has_current_link() || outward_.paused()) {
      return;
    }

    forwarding_link = outward_.current_link();

    // If we're paused we may have some outbound parcels buffered. Don't drop
    // the outward link yet in that case.
    if (outward_.parcels().IsEmpty()) {
      dead_outward_link = outward_.TakeCurrentLink();
    }

    final_sequence_length = outward_.sequence_length();
    outward_.parcels().SetFinalSequenceLength(final_sequence_length);
  }

  forwarding_link->AcceptRouteClosure(final_sequence_length);

  if (dead_outward_link) {
    dead_outward_link->Deactivate();
  }
}

IpczResult Router::Merge(mem::Ref<Router> other) {
  if (HasLocalPeer(other)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  {
    TwoMutexLock lock(&mutex_, &other->mutex_);
    if (inward_.has_any_link() || bridge_ || other->bridge_) {
      // It's not legit to call this on non-terminal routers.
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    bridge_ = std::make_unique<DecayableLink>();
    other->bridge_ = std::make_unique<DecayableLink>();

    RouterLink::Pair links = LocalRouterLink::CreatePair(
        LinkType::kBridge, LocalRouterLink::InitialState::kCannotDecay,
        Router::Pair(mem::WrapRefCounted(this), other));
    bridge_->SetCurrentLink(std::move(links.first));
    other->bridge_->SetCurrentLink(std::move(links.second));
  }

  Flush();
  return IPCZ_RESULT_OK;
}

SequenceNumber Router::SetOutwardLink(mem::Ref<RouterLink> link) {
  SequenceNumber first_sequence_number_on_new_link;
  {
    absl::MutexLock lock(&mutex_);
    ABSL_ASSERT(!outward_.has_current_link());
    first_sequence_number_on_new_link =
        outward_.SetCurrentLink(std::move(link));

    if (!outward_.has_decaying_link() && !inward_.has_decaying_link()) {
      bool ok = outward_.UnblockDecay();
      ABSL_ASSERT(ok);
    }
  }

  Flush();
  return first_sequence_number_on_new_link;
}

void Router::BeginProxying(const RouterDescriptor& inward_peer_descriptor,
                           mem::Ref<RouterLink> link,
                           mem::Ref<RouterLink> decaying_link) {
  mem::Ref<RouterLink> outward_link;
  {
    absl::MutexLock lock(&mutex_);
    ABSL_ASSERT(!inward_.has_current_link());
    if (inward_peer_descriptor.proxy_already_bypassed) {
      // When `proxy_already_bypassed` is true, this means we have two local
      // Routers -- let's call them P and Q -- who were each other's local peer,
      // and who were just split apart by one of them (`this`, call it Q)
      // serializing a new inward peer to extend the route to another node. In
      // this case `link` is a link to the new router, and the local target of
      // `outward_link` is our former local peer (P) who must now use `link` as
      // its own outward link.
      //
      // We set up Q with an inward link here to `link` so it can forward any
      // incoming parcels -- already received or in flight from P -- to the new
      // router. Below we will fix up P with the new outward link to `link` as
      // well.
      //
      // This is an optimization for the common case of a local pair being split
      // across nodes, where we have enough information at serialization and
      // deserialization time to avoid the overhead of the usual asynchronous
      // proxy bypass procedure.
      outward_link = outward_.TakeCurrentLink();
    }
  }

  bool attempt_self_removal = false;
  mem::Ref<Router> local_peer;
  if (inward_peer_descriptor.proxy_already_bypassed) {
    ABSL_ASSERT(outward_link);
    local_peer = outward_link->GetLocalTarget();
    ABSL_ASSERT(local_peer);

    TwoMutexLock lock(&mutex_, &local_peer->mutex_);
    ABSL_ASSERT(!local_peer->outward_.has_current_link());
    if (decaying_link) {
      inward_.StartDecayingWithLink(
          std::move(decaying_link),
          local_peer->outward_.current_sequence_number(),
          outward_.current_sequence_number());
    }
    ABSL_ASSERT(outward_.parcels().IsEmpty());
    local_peer->outward_.SetCurrentLink(std::move(link));
  } else {
    absl::MutexLock lock(&mutex_);
    // In the case where `proxy_already_bypassed` is false, this Router is
    // becoming a bidirectional proxy. We need to set its inward link
    // accordingly.
    inward_.SetCurrentLink(std::move(link));

    // Our outward link may be ready for decay by the time we hit this path.
    // Removal is deferred until we have an inward link in that case, so we'll
    // try again now.
    attempt_self_removal = true;
  }

  Flush();
  if (local_peer) {
    local_peer->Flush();
  }

  if (attempt_self_removal) {
    MaybeInitiateSelfRemoval();
  }
}

bool Router::StopProxying(SequenceNumber inbound_sequence_length,
                          SequenceNumber outbound_sequence_length) {
  mem::Ref<Router> bridge_peer;
  {
    absl::MutexLock lock(&mutex_);
    if (inward_.has_decaying_link() || outward_.has_decaying_link()) {
      return false;
    }

    if (bridge_) {
      if (!bridge_->has_current_link()) {
        return false;
      }
      bridge_peer = bridge_->GetLocalPeer();
    } else {
      inward_.StartDecaying(inbound_sequence_length, outbound_sequence_length);
      outward_.StartDecaying(outbound_sequence_length, inbound_sequence_length);
    }
  }

  if (bridge_peer) {
    TwoMutexLock lock(&mutex_, &bridge_peer->mutex_);
    if (!bridge_ || !bridge_->has_current_link() || !bridge_peer->bridge_ ||
        !bridge_peer->bridge_->has_current_link()) {
      return true;
    }

    outward_.StartDecaying(outbound_sequence_length, inbound_sequence_length);
    bridge_peer->outward_.StartDecaying(inbound_sequence_length,
                                        outbound_sequence_length);
    bridge_->StartDecaying(inbound_sequence_length, outbound_sequence_length);
    bridge_peer->bridge_->StartDecaying(outbound_sequence_length,
                                        inbound_sequence_length);
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
    if (!inward_.parcels().Push(std::move(parcel))) {
      return false;
    }

    if (!inward_.has_any_link()) {
      status_.num_local_parcels = inward_.parcels().GetNumAvailableParcels();
      status_.num_local_bytes = inward_.parcels().GetNumAvailableBytes();
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
    if (!outward_.parcels().Push(std::move(parcel))) {
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
  mem::Ref<RouterLink> dead_outward_link;
  mem::Ref<RouterLink> bridge_link;
  if (source.is_inward()) {
    // If we're being notified of our own side's closure, we want to propagate
    // this outward toward the other side.
    absl::MutexLock lock(&mutex_);
    outward_.parcels().SetFinalSequenceLength(sequence_length);
    if (outward_.ShouldPropagateRouteClosure()) {
      forwarding_link = outward_.current_link();
      outward_.set_closure_propagated(true);
    }

    // If we're receiving this, it's coming from the other side of the bridge
    // which is already reset by now.
    bridge_.reset();
  } else {
    // We're being notified of the other side's closure, so we want to propagate
    // this inward toward our own terminal router. If that's us, update portal
    // status and traps.
    absl::MutexLock lock(&mutex_);
    inward_.parcels().SetFinalSequenceLength(sequence_length);
    if (inward_.ShouldPropagateRouteClosure()) {
      forwarding_link = inward_.current_link();
      inward_.set_closure_propagated(true);
    } else if (bridge_) {
      forwarding_link = bridge_->TakeCurrentOrDecayingLink();
      dead_outward_link = outward_.TakeCurrentLink();
    } else if (!inward_.has_current_link()) {
      status_.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
      if (inward_.parcels().IsDead()) {
        status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
      }
      traps_.UpdatePortalStatus(status_, dispatcher);

      if (!inward_.parcels().IsExpectingMoreParcels()) {
        // We can drop our outward link if we know there are no more in-flight
        // parcels coming our way. Otherwise it'll be dropped as soon as that's
        // the case.
        dead_outward_link = outward_.TakeCurrentLink();
      }
    }
  }

  if (forwarding_link) {
    forwarding_link->AcceptRouteClosure(sequence_length);
  }

  if (dead_outward_link) {
    dead_outward_link->Deactivate();
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
  if (inward_.has_current_link()) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (!inward_.parcels().HasNextParcel()) {
    if (inward_.parcels().IsDead()) {
      return IPCZ_RESULT_NOT_FOUND;
    }
    return IPCZ_RESULT_UNAVAILABLE;
  }

  Parcel& p = inward_.parcels().NextParcel();
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
  inward_.parcels().Pop(parcel);
  memcpy(data, parcel.data_view().data(), parcel.data_view().size());
  parcel.Consume(portals, os_handles);

  status_.num_local_parcels = inward_.parcels().GetNumAvailableParcels();
  status_.num_local_bytes = inward_.parcels().GetNumAvailableBytes();
  if (inward_.parcels().IsDead()) {
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
  if (inward_.has_current_link()) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (!inward_.parcels().HasNextParcel()) {
    return IPCZ_RESULT_UNAVAILABLE;
  }

  Parcel& p = inward_.parcels().NextParcel();
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
  if (inward_.has_current_link()) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (!inward_.parcels().HasNextParcel()) {
    // If ipcz is used correctly this is impossible.
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  Parcel& p = inward_.parcels().NextParcel();
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
  bool ok = inward_.parcels().Pop(consumed_parcel);
  ABSL_ASSERT(ok);

  status_.num_local_parcels = inward_.parcels().GetNumAvailableParcels();
  status_.num_local_bytes = inward_.parcels().GetNumAvailableBytes();
  if (inward_.parcels().IsDead()) {
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

mem::Ref<Router> Router::SerializeNewInwardPeer(NodeLink& to_node_link,
                                                RouterDescriptor& descriptor) {
  for (;;) {
    // The fast path for a local pair being split is to directly establish a new
    // outward link to the destination, rather than proxying. First we acquire a
    // ref to the local peer Router if there is one.
    //
    // Note that if this local link is busy -- i.e. one of the routers is
    // already decaying or is adjacent to a decaying router -- we cannot take
    // the optimized path and must proxy instead.
    bool was_link_busy = false;
    mem::Ref<Router> local_peer;
    {
      absl::MutexLock lock(&mutex_);
      traps_.DisableAllAndClear();
      if (outward_.has_current_link()) {
        local_peer = outward_.GetLocalPeer();
        was_link_busy = !outward_.current_link()->CanDecay();
      }
    }

    // Now we reacquire the lock, along with the local peer's lock if we have a
    // local peer. In the rare event that our link state changed since we held
    // the lock above, we'll loop back and try again.
    TwoMutexLock lock(&mutex_, local_peer ? &local_peer->mutex_ : nullptr);
    if (!local_peer && outward_.GetLocalPeer()) {
      // We didn't have a local peer before, but now we do.
      continue;
    }

    if (local_peer && !was_link_busy) {
      local_peer->mutex_.AssertHeld();

      // Links may have changed between lock acquisitions above.
      if (outward_.GetLocalPeer() != local_peer ||
          local_peer->outward_.GetLocalPeer() != this) {
        continue;
      }

      if (!outward_.TryToDecay(to_node_link.remote_node_name())) {
        // Decay has been blocked since we checked above. Restart since it is no
        // longer safe to take the optimized path.
        continue;
      }

      local_peer->outward_.ResetCurrentLink();

      if (status_.flags & IPCZ_PORTAL_STATUS_PEER_CLOSED) {
        ABSL_ASSERT(inward_.parcels().final_sequence_length());
        descriptor.peer_closed = true;
        descriptor.closed_peer_sequence_length =
            *inward_.parcels().final_sequence_length();
        inward_.set_closure_propagated(true);
      }

      descriptor.proxy_already_bypassed = true;
      descriptor.next_outgoing_sequence_number =
          outward_.parcels().current_sequence_number();
      descriptor.next_incoming_sequence_number =
          inward_.parcels().current_sequence_number();
      descriptor.decaying_incoming_sequence_length =
          local_peer->outward_.parcels().current_sequence_number();

      DVLOG(4) << "Splitting local pair to move router with outbound sequence "
               << "length " << outward_.parcels().current_sequence_number()
               << " and current inbound sequence number "
               << descriptor.next_incoming_sequence_number;

      // The local peer will listen on the new link, rather than this router.
      // This router will only persist to forward its queued inbound parcels
      // on to the new remote router.
      return local_peer;
    }

    // In this case, we're not splitting a local pair, but extending the route
    // on this side. This router will become a proxy to the new Router which is
    // being serialized here, and it will forward inbound parcels to that Router
    // over a new inward link. Similarly it router will forward outbound parcels
    // from the new Router to our own outward peer.
    descriptor.proxy_already_bypassed = false;
    descriptor.next_outgoing_sequence_number =
        outward_.parcels().current_sequence_number();
    descriptor.next_incoming_sequence_number =
        inward_.parcels().current_sequence_number();

    DVLOG(4) << "Extending route on new router with outbound sequence length "
             << outward_.parcels().current_sequence_number()
             << " and current inbound sequence number "
             << descriptor.next_incoming_sequence_number;

    NodeName proxy_peer_node_name;
    RoutingId proxy_peer_routing_id;
    if (status_.flags & IPCZ_PORTAL_STATUS_PEER_CLOSED) {
      descriptor.peer_closed = true;
      descriptor.closed_peer_sequence_length =
          *inward_.parcels().final_sequence_length();
      inward_.set_closure_propagated(true);
    } else if (outward_.has_current_link() && !outward_.GetLocalPeer() &&
               outward_.current_link()->GetType().is_central() &&
               !outward_.has_decaying_link() && !inward_.has_any_link()) {
      // If we're becoming a proxy under some common special conditions --
      // namely that no other part of the route is currently decaying -- we can
      // roll the first step of our own decay into this descriptor transmission.
      RemoteRouterLink& remote_link =
          static_cast<RemoteRouterLink&>(*outward_.current_link());
      proxy_peer_node_name = remote_link.node_link()->remote_node_name();
      proxy_peer_routing_id = remote_link.routing_id();

      if (outward_.TryToDecay(to_node_link.remote_node_name())) {
        DVLOG(4) << "Will initiate proxy bypass immediately on deserialization "
                 << "with peer at " << proxy_peer_node_name.ToString()
                 << " and peer route to proxy on routing ID "
                 << proxy_peer_routing_id;

        descriptor.proxy_peer_node_name =
            remote_link.node_link()->remote_node_name();
        descriptor.proxy_peer_routing_id = remote_link.routing_id();
      }
    }

    return mem::WrapRefCounted(this);
  }
}

// static
mem::Ref<Router> Router::Deserialize(const RouterDescriptor& descriptor,
                                     NodeLink& from_node_link) {
  auto router = mem::MakeRefCounted<Router>();
  {
    absl::MutexLock lock(&router->mutex_);
    router->outward_.parcels().ResetInitialSequenceNumber(
        descriptor.next_outgoing_sequence_number);
    router->inward_.parcels().ResetInitialSequenceNumber(
        descriptor.next_incoming_sequence_number);
    if (descriptor.peer_closed) {
      router->status_.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
      router->inward_.parcels().SetFinalSequenceLength(
          descriptor.closed_peer_sequence_length);
      if (router->inward_.parcels().IsDead()) {
        router->status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
      }
    }

    router->outward_.SetCurrentLink(from_node_link.AddRoute(
        descriptor.new_routing_id, descriptor.new_routing_id,
        descriptor.proxy_already_bypassed ? LinkType::kCentral
                                          : LinkType::kPeripheralOutward,
        LinkSide::kB, router));
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
      router->outward_.StartDecayingWithLink(
          from_node_link.AddRoute(descriptor.new_decaying_routing_id,
                                  descriptor.new_decaying_routing_id,
                                  LinkType::kPeripheralOutward, LinkSide::kB,
                                  router),
          router->outward_.current_sequence_number(),
          descriptor.decaying_incoming_sequence_length > 0
              ? descriptor.decaying_incoming_sequence_length
              : descriptor.next_incoming_sequence_number);

      DVLOG(4) << "Route moved from split pair on "
               << from_node_link.remote_node_name().ToString() << " to "
               << from_node_link.local_node_name().ToString()
               << " via routing ID " << descriptor.new_routing_id
               << " and decaying routing ID "
               << descriptor.new_decaying_routing_id;
    } else {
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
    if (!outward_.has_current_link()) {
      // Must have been disconnected already.
      return true;
    }

    if (!outward_.current_link()->IsRemoteLinkTo(requesting_node_link,
                                                 requesting_routing_id)) {
      // Authenticate that the request to bypass our outward peer is actually
      // coming from our outward peer.
      DLOG(ERROR) << "Rejecting InitiateProxyBypass from "
                  << requesting_node_link.remote_node_name().ToString()
                  << " on routing ID " << requesting_routing_id
                  << " with existing outward "
                  << DescribeLink(outward_.current_link());
      return false;
    }
  }

  if (proxy_peer_node_name != requesting_node_link.local_node_name()) {
    // Common case: the proxy's outward peer is NOT on the same node as we are.
    // In this case we send a BypassProxy request to that node, which may
    // require an introduction first.
    {
      // Begin decaying our outward link. We don't know the sequence length
      // coming from it yet, but that will be determined eventually.
      absl::MutexLock lock(&mutex_);
      outward_.StartDecaying(outward_.current_sequence_number());
    }

    mem::Ref<NodeLink> new_peer_node =
        requesting_node_link.node()->GetLink(proxy_peer_node_name);
    if (new_peer_node) {
      new_peer_node->BypassProxy(requesting_node_link.remote_node_name(),
                                 proxy_peer_routing_id,
                                 mem::WrapRefCounted(this));
      return true;
    }

    requesting_node_link.node()->EstablishLink(
        proxy_peer_node_name,
        [requesting_node_name = requesting_node_link.remote_node_name(),
         proxy_peer_routing_id,
         self = mem::WrapRefCounted(this)](NodeLink* new_link) {
          if (!new_link) {
            // TODO: failure to connect to a node here should result in route
            // destruction. This is not the same as closure since we can't
            // guarantee any sequence length.
            return;
          }

          new_link->BypassProxy(requesting_node_name, proxy_peer_routing_id,
                                self);
        });
    return true;
  }

  // The proxy's outward peer lives on the same node as this router, so we can
  // skip some messaging and locally link the two routers together right now.

  mem::Ref<Router> new_local_peer =
      requesting_node_link.GetRouter(proxy_peer_routing_id);
  mem::Ref<RouterLink> previous_outward_link_from_new_local_peer;
  SequenceNumber proxied_inbound_sequence_length;
  SequenceNumber proxied_outbound_sequence_length;
  if (!new_local_peer) {
    // The peer may have been explicitly closed by the time this message
    // arrives.
    return true;
  }

  {
    TwoMutexLock lock(&mutex_, &new_local_peer->mutex_);
    proxied_inbound_sequence_length =
        new_local_peer->outward_.current_sequence_number();
    proxied_outbound_sequence_length = outward_.current_sequence_number();

    DVLOG(4) << "Initiating proxy bypass with new local peer on "
             << proxy_peer_node_name.ToString() << " and proxy links to "
             << requesting_node_link.remote_node_name().ToString()
             << " on routing IDs " << proxy_peer_routing_id << " and "
             << requesting_routing_id << "; inbound length "
             << proxied_inbound_sequence_length << " and outbound length "
             << proxied_outbound_sequence_length;

    // We get a decaying outward link to the proxy, only to accept inbound
    // parcels already sent to it by our new local peer.
    outward_.StartDecaying(proxied_outbound_sequence_length,
                           proxied_inbound_sequence_length);

    // Our new local peer gets a decaying outward link to the proxy, only to
    // forward outbound parcels already expected by the proxy.
    previous_outward_link_from_new_local_peer =
        new_local_peer->outward_.current_link();
    new_local_peer->outward_.StartDecaying(proxied_inbound_sequence_length,
                                           proxied_outbound_sequence_length);

    // Finally, create a new LocalRouterLink and use it to replace both our
    // own outward link and our new local peer's outward link. The new link is
    // not ready for further decay until the above established decaying links
    // have fully decayed.
    RouterLink::Pair links = LocalRouterLink::CreatePair(
        LinkType::kCentral, LocalRouterLink::InitialState::kCannotDecay,
        Router::Pair(mem::WrapRefCounted(this), new_local_peer));
    outward_.SetCurrentLink(std::move(links.first));
    new_local_peer->outward_.SetCurrentLink(std::move(links.second));
  }

  if (previous_outward_link_from_new_local_peer) {
    previous_outward_link_from_new_local_peer->StopProxying(
        proxied_inbound_sequence_length, proxied_outbound_sequence_length);
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
    if (!outward_.has_current_link()) {
      // TODO: terminate the route. not the same as closure.
      return true;
    }

    if (!outward_.current_link()->CanNodeRequestBypass(
            new_peer->node_link()->remote_node_name())) {
      return false;
    }

    proxy_inbound_sequence_length = outward_.current_sequence_number();

    RemoteRouterLink& remote_proxy =
        static_cast<RemoteRouterLink&>(*outward_.current_link());
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

    decaying_outward_link_to_proxy = outward_.current_link();
    outward_.StartDecaying(proxy_inbound_sequence_length,
                           proxy_outbound_sequence_length);
    outward_.SetCurrentLink(new_peer);
  }

  decaying_outward_link_to_proxy->StopProxying(proxy_inbound_sequence_length,
                                               proxy_outbound_sequence_length);
  new_peer->ProxyWillStop(proxy_inbound_sequence_length);

  Flush();
  return true;
}

bool Router::BypassProxyWithNewLinkToSameNode(
    mem::Ref<RouterLink> new_peer,
    SequenceNumber sequence_length_from_proxy) {
  mem::Ref<RouterLink> decaying_proxy;
  SequenceNumber sequence_length_to_proxy;
  {
    absl::MutexLock lock(&mutex_);
    if (!outward_.has_current_link()) {
      // Not necessarily a validation failure if the link was severed due to a
      // remote crash.
      //
      // TODO: propagate route teardown?
      return true;
    }

    if (outward_.GetLocalPeer()) {
      // Bogus request, we obviously don't have a link to a proxy on `new_peer`s
      // node, because our outward link is local.
      return false;
    }

    RemoteRouterLink& old_remote_peer =
        static_cast<RemoteRouterLink&>(*outward_.current_link());
    RemoteRouterLink& new_remote_peer =
        static_cast<RemoteRouterLink&>(*new_peer);
    const mem::Ref<NodeLink> remote_node_link = old_remote_peer.node_link();
    if (new_remote_peer.node_link() != remote_node_link) {
      // Bogus request: our outward link does not go to the same node as
      // `new_peer`.
      return false;
    }

    if (outward_.has_decaying_link()) {
      return false;
    }

    DVLOG(4) << "Bypassing proxy at "
             << remote_node_link->remote_node_name().ToString()
             << " on routing ID " << old_remote_peer.routing_id() << " from "
             << remote_node_link->local_node_name().ToString()
             << " with new routing ID " << new_remote_peer.routing_id();

    sequence_length_to_proxy = outward_.current_sequence_number();

    decaying_proxy = outward_.current_link();
    outward_.StartDecaying(sequence_length_to_proxy,
                           sequence_length_from_proxy);
    outward_.SetCurrentLink(std::move(new_peer));
  }

  ABSL_ASSERT(decaying_proxy);
  decaying_proxy->StopProxyingToLocalPeer(sequence_length_to_proxy);
  return true;
}

bool Router::StopProxyingToLocalPeer(SequenceNumber sequence_length) {
  mem::Ref<Router> local_peer;
  mem::Ref<Router> bridge_peer;
  {
    absl::MutexLock lock(&mutex_);
    if (bridge_ && bridge_->has_decaying_link()) {
      bridge_peer = bridge_->GetDecayingLocalPeer();
    } else if (outward_.has_decaying_link()) {
      local_peer = outward_.GetDecayingLocalPeer();
    } else {
      return false;
    }
  }

  if (local_peer && !bridge_peer) {
    // Typical case, where no bridge link is present.
    TwoMutexLock lock(&mutex_, &local_peer->mutex_);
    if (!local_peer->outward_.has_decaying_link() ||
        !outward_.has_decaying_link() || !inward_.has_decaying_link()) {
      return false;
    }

    DVLOG(4) << "Stopping proxy with inward decaying "
             << DescribeLink(inward_.decaying_link()) << " and outward "
             << "decaying " << DescribeLink(outward_.decaying_link());

    local_peer->outward_.set_length_from_decaying_link(sequence_length);
    outward_.set_length_to_decaying_link(sequence_length);
    inward_.set_length_from_decaying_link(sequence_length);
  } else if (bridge_peer) {
    // Here the proxy is actually a pair of bridge routers linking two routes
    // together as a result of a prior Merge() operation. Just means we have to
    // update three routers instead of two.
    {
      absl::MutexLock lock(&bridge_peer->mutex_);
      if (!bridge_peer->outward_.has_decaying_link()) {
        return false;
      }
      local_peer = bridge_peer->outward_.GetDecayingLocalPeer();
      if (!local_peer) {
        return false;
      }
    }

    ThreeMutexLock lock(&mutex_, &local_peer->mutex_, &bridge_peer->mutex_);
    if (!local_peer->outward_.has_decaying_link() ||
        !outward_.has_decaying_link() ||
        !bridge_peer->outward_.has_decaying_link()) {
      return false;
    }

    local_peer->outward_.set_length_from_decaying_link(sequence_length);
    outward_.set_length_from_decaying_link(sequence_length);
    bridge_->set_length_to_decaying_link(sequence_length);
    bridge_peer->outward_.set_length_to_decaying_link(sequence_length);
    bridge_peer->bridge_->set_length_from_decaying_link(sequence_length);
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

bool Router::OnProxyWillStop(SequenceNumber sequence_length) {
  {
    absl::MutexLock lock(&mutex_);
    if (!outward_.has_decaying_link()) {
      return true;
    }

    DVLOG(4) << "Bypassed proxy has finalized its inbound sequence length at "
             << sequence_length << " for "
             << DescribeLink(outward_.decaying_link());

    outward_.set_length_from_decaying_link(sequence_length);
  }

  Flush();
  return true;
}

bool Router::OnDecayUnblocked() {
  MaybeInitiateSelfRemoval();
  return true;
}

void Router::LogDescription() {
  absl::MutexLock lock(&mutex_);
  DLOG(INFO) << "## router [" << this << "]";
  DLOG(INFO) << " - status flags: " << status_.flags;
  DLOG(INFO) << " - side closed: "
             << (outward_.parcels().final_sequence_length().has_value() ? "yes"
                                                                        : "no");
  DLOG(INFO) << " - outward " << DescribeLink(outward_.current_link());
  DLOG(INFO) << " - outward decaying "
             << DescribeLink(outward_.decaying_link());
  DLOG(INFO) << " - outward length to decaying link: "
             << DescribeOptionalLength(outward_.length_to_decaying_link());
  DLOG(INFO) << " - outward length from decaying link: "
             << DescribeOptionalLength(outward_.length_from_decaying_link());

  DLOG(INFO) << " - inward " << DescribeLink(inward_.current_link());
  DLOG(INFO) << " - inward decaying " << DescribeLink(inward_.decaying_link());
  DLOG(INFO) << " - inward length to decaying link: "
             << DescribeOptionalLength(inward_.length_to_decaying_link());
  DLOG(INFO) << " - inward length from decaying link: "
             << DescribeOptionalLength(inward_.length_from_decaying_link());
}

void Router::LogRouteTrace() {
  LogDescription();

  mem::Ref<RouterLink> next_link;
  {
    absl::MutexLock lock(&mutex_);
    next_link = outward_.current_link();
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
      next_link = bridge_ ? bridge_->current_link() : inward_.current_link();
    } else {
      next_link = outward_.current_link();
    }
  }
  if (next_link) {
    next_link->LogRouteTrace();
  }
}

void Router::Flush() {
  mem::Ref<RouterLink> inward_link;
  mem::Ref<RouterLink> outward_link;
  mem::Ref<RouterLink> dead_inward_link;
  mem::Ref<RouterLink> dead_outward_link;
  mem::Ref<RouterLink> decaying_inward_proxy;
  mem::Ref<RouterLink> decaying_outward_proxy;
  mem::Ref<RouterLink> bridge_link;
  mem::Ref<RouterLink> decaying_bridge_link;
  absl::InlinedVector<Parcel, 2> outbound_parcels;
  absl::InlinedVector<Parcel, 2> outbound_parcels_to_proxy;
  absl::InlinedVector<Parcel, 2> inbound_parcels;
  absl::InlinedVector<Parcel, 2> inbound_parcels_to_proxy;
  absl::InlinedVector<Parcel, 2> bridge_parcels;
  bool inward_proxy_decayed = false;
  bool outward_proxy_decayed = false;
  absl::optional<SequenceNumber> final_inward_sequence_length;
  absl::optional<SequenceNumber> final_outward_sequence_length;
  {
    absl::MutexLock lock(&mutex_);
    inward_link = inward_.current_link();
    outward_link = outward_.current_link();
    decaying_inward_proxy = inward_.decaying_link();
    decaying_outward_proxy = outward_.decaying_link();
    if (bridge_) {
      bridge_link = bridge_->has_decaying_link() ? bridge_->decaying_link()
                                                 : bridge_->current_link();
    }

    outward_.FlushParcels(outbound_parcels_to_proxy, outbound_parcels);
    if (outward_.IsDecayFinished(inward_.sequence_length())) {
      DVLOG(4) << "Outward " << DescribeLink(outward_.decaying_link())
               << " fully decayed at outbound length "
               << *outward_.length_to_decaying_link() << " and inbound length "
               << *outward_.length_from_decaying_link();
      outward_proxy_decayed = true;
      outward_.ResetDecayingLink();
    }

    inward_.FlushParcels(inbound_parcels_to_proxy, inbound_parcels);
    if (inward_.IsDecayFinished(outward_.sequence_length())) {
      DVLOG(4) << "Inward " << DescribeLink(inward_.decaying_link())
               << " fully decayed at inbound length "
               << *inward_.length_to_decaying_link() << " and outbound length "
               << *inward_.length_from_decaying_link();

      inward_proxy_decayed = true;
      inward_.ResetDecayingLink();
    }

    Parcel parcel;
    while (bridge_link && inward_.parcels().Pop(parcel)) {
      DVLOG(4) << "Forwarding inbound " << parcel.Describe() << " over bridge "
               << DescribeLink(bridge_link);
      bridge_parcels.push_back(std::move(parcel));
    }

    if (bridge_ && bridge_->length_to_decaying_link() &&
        inward_.current_sequence_number() >=
            *bridge_->length_to_decaying_link() &&
        bridge_->length_from_decaying_link() &&
        outward_.current_sequence_number() >=
            *bridge_->length_from_decaying_link()) {
      bridge_.reset();
    }

    // If the inbound sequence is dead, the other side of the route is gone and
    // we have received all the parcels it sent. We can drop the outward link.
    if (inward_.parcels().IsDead()) {
      dead_outward_link = outward_.TakeCurrentLink();
    }

    if (outward_.ShouldPropagateRouteClosure()) {
      final_outward_sequence_length =
          *outward_.parcels().final_sequence_length();

      ABSL_ASSERT(outward_.parcels().IsEmpty());
      if (!dead_outward_link) {
        dead_outward_link = outward_.TakeCurrentLink();
      }
    }

    if (inward_.ShouldPropagateRouteClosure()) {
      final_inward_sequence_length = *inward_.parcels().final_sequence_length();
      if (inward_.parcels().IsDead()) {
        dead_inward_link = inward_.TakeCurrentLink();
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

  for (Parcel& parcel : bridge_parcels) {
    bridge_link->AcceptParcel(parcel);
  }

  if (outward_proxy_decayed) {
    decaying_outward_proxy->Deactivate();
    decaying_outward_proxy.reset();
  }

  if (inward_proxy_decayed) {
    decaying_inward_proxy->Deactivate();
    decaying_inward_proxy.reset();
  }

  if (outward_link && (inward_proxy_decayed || outward_proxy_decayed) &&
      (!decaying_inward_proxy && !decaying_outward_proxy)) {
    DVLOG(4) << "Router with fully decayed links may be eligible for "
             << "self-removal with outward " << DescribeLink(outward_link);

    outward_link->SetSideCanDecay();
    if (!MaybeInitiateSelfRemoval() && outward_link->CanDecay()) {
      DVLOG(4) << "Cannot remove self, but pinging peer on "
               << DescribeLink(outward_link);

      // Ping the other side in case it might want to decay. If it doesn't, or
      // if it already starts to before this message arrives, that's OK.
      outward_link->DecayUnblocked();
    }
  }

  if (bridge_link && outward_link && !inward_link && !decaying_inward_proxy &&
      !decaying_outward_proxy) {
    MaybeInitiateBridgeBypass();
  }

  if (final_inward_sequence_length) {
    inward_link->AcceptRouteClosure(*final_inward_sequence_length);
  }

  if (final_outward_sequence_length) {
    outward_link->AcceptRouteClosure(*final_outward_sequence_length);
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
    if (!outward_.has_current_link() || !inward_.has_current_link() ||
        outward_.has_decaying_link() || inward_.has_decaying_link() ||
        !outward_.current_link()->GetType().is_central()) {
      return false;
    }

    ABSL_ASSERT(!inward_.GetLocalPeer());
    successor = mem::WrapRefCounted(
        static_cast<RemoteRouterLink*>(inward_.current_link().get()));

    if (!outward_.TryToDecay(successor->node_link()->remote_node_name())) {
      DVLOG(4) << "Proxy self-removal blocked by busy "
               << DescribeLink(outward_.current_link());
      return false;
    }

    local_peer = outward_.GetLocalPeer();
    if (!local_peer) {
      auto& remote_peer =
          static_cast<RemoteRouterLink&>(*outward_.current_link());
      peer_node_name = remote_peer.node_link()->remote_node_name();
      routing_id_to_peer = remote_peer.routing_id();
    }
  }

  if (!local_peer) {
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
      successor->node_link()->AllocateRoutingIds(1);
  mem::Ref<RouterLink> new_link = successor->node_link()->AddRoute(
      new_routing_id, new_routing_id, LinkType::kCentral, LinkSide::kA,
      local_peer);

  {
    TwoMutexLock lock(&mutex_, &local_peer->mutex_);

    // It's possible that the local peer has been closed, in which case its
    // closure will have already propagated to us and there's no bypass work to
    // be done.
    if (!local_peer->outward_.current_link()) {
      ABSL_ASSERT(status_.flags & IPCZ_PORTAL_STATUS_PEER_CLOSED);
      DVLOG(4) << "Proxy self-removal blocked by peer closure.";
      return false;
    }

    DVLOG(4) << "Proxy initiating its own bypass from "
             << successor->node_link()->remote_node_name().ToString() << " to "
             << "a local peer.";

    // Otherwise it should definitely still be linked to us, because we locked
    // in our own decaying state above.
    ABSL_ASSERT(local_peer->outward_.GetLocalPeer() == this);
    ABSL_ASSERT(outward_.GetLocalPeer() == local_peer);

    sequence_length = local_peer->outward_.current_sequence_number();

    local_peer->outward_.StartDecaying(sequence_length);
    outward_.StartDecaying(absl::nullopt, sequence_length);
    inward_.StartDecaying(sequence_length);

    local_peer->outward_.SetCurrentLink(new_link);
    local_peer->outward_.set_paused(true);
  }

  successor->BypassProxyToSameNode(new_routing_id, sequence_length);
  local_peer->PauseOutboundTransmission(false);
  return true;
}

void Router::MaybeInitiateBridgeBypass() {
  mem::Ref<Router> first_bridge = mem::WrapRefCounted(this);
  mem::Ref<Router> second_bridge;
  {
    absl::MutexLock lock(&mutex_);
    if (!bridge_ || !bridge_->has_current_link()) {
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
    link_to_first_peer = outward_.current_link();
    link_to_second_peer = second_bridge->outward_.current_link();

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

    if (!link_to_first_peer->MaybeBeginDecay(second_peer_node_name)) {
      return;
    }
    if (!link_to_second_peer->MaybeBeginDecay()) {
      // Cancel the decay on this bridge's side, because we couldn't decay the
      // other side of the bridge yet.
      link_to_first_peer->CancelDecay();
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
      first_bridge->outward_.StartDecaying();
      second_bridge->outward_.StartDecaying();
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
        node_link_to_second_peer->AllocateRoutingIds(1);
    mem::Ref<RouterLink> new_link = node_link_to_second_peer->AddRoute(
        bypass_routing_id, bypass_routing_id, LinkType::kCentral, LinkSide::kA,
        first_local_peer);
    {
      ThreeMutexLock lock(&first_bridge->mutex_, &second_bridge->mutex_,
                          &first_local_peer->mutex_);

      length_from_local_peer =
          first_local_peer->outward_.current_sequence_number();

      first_local_peer->outward_.StartDecaying(length_from_local_peer);
      second_bridge->outward_.StartDecaying(length_from_local_peer);
      first_bridge->bridge_->StartDecaying(length_from_local_peer);
      first_bridge->outward_.StartDecaying(absl::nullopt,
                                           length_from_local_peer);
      second_bridge->bridge_->StartDecaying(absl::nullopt,
                                            length_from_local_peer);

      first_local_peer->outward_.SetCurrentLink(new_link);
      first_local_peer->outward_.set_paused(true);
    }

    link_to_second_peer->BypassProxyToSameNode(bypass_routing_id,
                                               length_from_local_peer);
    first_local_peer->PauseOutboundTransmission(false);
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
        first_local_peer->outward_.current_sequence_number();
    const SequenceNumber length_from_second_peer =
        second_local_peer->outward_.current_sequence_number();

    first_local_peer->outward_.StartDecaying(length_from_first_peer,
                                             length_from_second_peer);
    second_local_peer->outward_.StartDecaying(length_from_second_peer,
                                              length_from_first_peer);
    first_bridge->outward_.StartDecaying(length_from_second_peer,
                                         length_from_first_peer);
    second_bridge->outward_.StartDecaying(length_from_first_peer,
                                          length_from_second_peer);
    first_bridge->bridge_->StartDecaying(length_from_first_peer,
                                         length_from_second_peer);
    second_bridge->bridge_->StartDecaying(length_from_second_peer,
                                          length_from_first_peer);

    RouterLink::Pair links = LocalRouterLink::CreatePair(
        LinkType::kCentral, LocalRouterLink::InitialState::kCannotDecay,
        Router::Pair(first_local_peer, second_local_peer));
    first_local_peer->outward_.SetCurrentLink(std::move(links.first));
    second_local_peer->outward_.SetCurrentLink(std::move(links.second));
  }

  first_bridge->Flush();
  second_bridge->Flush();
  first_local_peer->Flush();
  second_local_peer->Flush();
}

}  // namespace core
}  // namespace ipcz
