// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/router.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <forward_list>
#include <sstream>
#include <utility>
#include <vector>

#include "core/local_router_link.h"
#include "core/node.h"
#include "core/node_link.h"
#include "core/node_name.h"
#include "core/parcel.h"
#include "core/portal.h"
#include "core/portal_descriptor.h"
#include "core/remote_router_link.h"
#include "core/router_link.h"
#include "core/router_link_state.h"
#include "core/routing_id.h"
#include "core/sequence_number.h"
#include "core/trap.h"
#include "core/trap_event_dispatcher.h"
#include "core/two_sided.h"
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

Router::Router(Side side) : side_(side) {
  ++g_num_routers;

  DVLOG(5) << "There are " << g_num_routers << " living Routers.";
}

Router::~Router() {
  --g_num_routers;

  DVLOG(5) << "There are " << g_num_routers << " living Routers.";
}

// static
size_t Router::GetNumRoutersForTesting() {
  return g_num_routers;
}

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
      DVLOG(4) << "Buffering " << parcel.Describe();
      const bool ok = outward_.parcels.Push(std::move(parcel));
      ABSL_ASSERT(ok);
      return IPCZ_RESULT_OK;
    }

    DVLOG(4) << "Sending " << parcel.Describe() << " over "
             << DescribeLink(link);
  }

  link->AcceptParcel(parcel);
  return IPCZ_RESULT_OK;
}

void Router::CloseRoute() {
  SequenceNumber final_sequence_length;
  mem::Ref<RouterLink> forwarding_link;
  mem::Ref<RouterLink> dead_outward_link;
  {
    absl::MutexLock lock(&mutex_);

    // We can't have an inward link, because CloseRoute() must only be called on
    // a terminal Router; that is, a Router directly controlled by a Portal.
    ABSL_ASSERT(!inward_.link);

    side_closed_ = true;
    if (!outward_.link || outbound_transmission_paused_) {
      return;
    }

    forwarding_link = outward_.link;

    // If we're paused we may have some outbound parcels buffered. Don't drop
    // the outward link yet in that case.
    if (outward_.parcels.IsEmpty()) {
      std::swap(dead_outward_link, outward_.link);
    }

    final_sequence_length = outbound_sequence_length_;
  }

  forwarding_link->AcceptRouteClosure(side_, final_sequence_length);

  if (dead_outward_link) {
    dead_outward_link->Deactivate();
  }
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

    {
      RouterLinkState::Locked state(outward_.link->GetLinkState(), side_);
      state.this_side().is_blocking_decay =
          outward_.decaying_proxy_link != nullptr;
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

    ABSL_ASSERT(outward_.parcels.IsEmpty());
    local_peer->outward_.link = std::move(link);
  } else {
    absl::MutexLock lock(&mutex_);
    // In the case where `route_is_peer` is false, this Router is becoming a
    // bidirectional proxy. We need to set its inward link accordingly.
    inward_.link = std::move(link);

    // We must also configure its outward ParcelQueue. Note that if the Router
    // has no outward link or outbound transmission is paused, the queue may
    // be in use and non-empty already. Otherwise its base SequenceNumber should
    // start at the Router's next outbound SequenceNumber.
    if (!outward_.parcels.IsEmpty()) {
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
    if (inward_.decaying_proxy_link || outward_.decaying_proxy_link) {
      return false;
    }

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
      DVLOG(4) << "Inbound " << parcel.Describe() << " received by "
               << DescribeLink(outward_.link);

      is_inbound = true;
    } else if (outward_.decaying_proxy_link &&
               outward_.decaying_proxy_link->IsRemoteLinkTo(link, routing_id)) {
      DVLOG(4) << "Inbound " << parcel.Describe() << " received by "
               << DescribeLink(outward_.decaying_proxy_link);

      is_inbound = true;
    } else if (inward_.link && inward_.link->IsRemoteLinkTo(link, routing_id)) {
      DVLOG(4) << "Outbound " << parcel.Describe() << " received by "
               << DescribeLink(inward_.link);

      is_inbound = false;
    } else if (inward_.decaying_proxy_link &&
               inward_.decaying_proxy_link->IsRemoteLinkTo(link, routing_id)) {
      DVLOG(4) << "Outbound " << parcel.Describe() << " received by "
               << DescribeLink(inward_.decaying_proxy_link);

      is_inbound = false;
    } else {
      DVLOG(4) << "Rejecting unexpected " << parcel.Describe() << " at "
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
    if (!outward_.parcels.Push(std::move(parcel))) {
      return false;
    }
  }

  Flush();
  return true;
}

void Router::AcceptRouteClosure(Side side, SequenceNumber sequence_length) {
  TrapEventDispatcher dispatcher;
  mem::Ref<RouterLink> forwarding_link;
  mem::Ref<RouterLink> dead_outward_link;
  if (side == side_) {
    // If we're being notified of our own side's closure, we want to propagate
    // this outward toward the other side.
    absl::MutexLock lock(&mutex_);
    side_closed_ = true;
    if (!outward_.closure_propagated && !outbound_transmission_paused_ &&
        outward_.link) {
      forwarding_link = outward_.link;
      outward_.closure_propagated = true;
    }
  } else {
    // We're being notified of the other side's closure, so we want to propagate
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

      if (!inward_.parcels.IsExpectingMoreParcels()) {
        // We can drop our outward link if we know there are no more in-flight
        // parcels coming our way. Otherwise it'll be dropped as soon as that's
        // the case.
        std::swap(dead_outward_link, outward_.link);
      }
    }
  }

  if (forwarding_link) {
    forwarding_link->AcceptRouteClosure(side, sequence_length);
  }

  if (dead_outward_link) {
    // May delete `this`.
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
  if (inward_.link) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

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
  if (inward_.link) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

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
  if (inward_.link) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
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
    // ref to the local peer Router if there is one.
    mem::Ref<Router> local_peer;
    {
      absl::MutexLock lock(&mutex_);
      traps_.Clear();
      if (outward_.link) {
        local_peer = outward_.link->GetLocalTarget();
      }
    }

    // Now we reacquire the lock, along with the local peer's lock if we have a
    // local peer. In the rare event that our link state changed since we held
    // the lock above, we'll loop back and try again.
    TwoMutexLock lock(&mutex_, local_peer ? &local_peer->mutex_ : nullptr);
    if (!local_peer && outward_.link && outward_.link->GetLocalTarget()) {
      // We didn't have a local peer before, but now we do.
      continue;
    } else if (local_peer) {
      local_peer->mutex_.AssertHeld();
      if (!outward_.link || outward_.link->GetLocalTarget() != local_peer ||
          !local_peer->outward_.link ||
          local_peer->outward_.link->GetLocalTarget() != this) {
        // We're no longer part of the same (or likely any) local pair.
        continue;
      }

      // We remove the peer's outward link so that it doesn't transmit more
      // parcels directly to us and so that it doesn't behave as if it still
      // has a local peer. It will be given a link to its new remote peer
      // via BeginProxying(), immediately after this descriptor is sent.
      ABSL_ASSERT(local_peer->outward_.parcels.IsEmpty());
      local_peer->outward_.parcels.ResetBaseSequenceNumber(
          local_peer->outbound_sequence_length_);
      local_peer->outward_.link.reset();

      if (status_.flags & IPCZ_PORTAL_STATUS_PEER_CLOSED) {
        ABSL_ASSERT(inward_.parcels.peer_sequence_length());
        descriptor.peer_closed = true;
        descriptor.closed_peer_sequence_length =
            *inward_.parcels.peer_sequence_length();
        inward_.closure_propagated = true;
      }

      descriptor.route_is_peer = true;
      descriptor.next_outgoing_sequence_number = outbound_sequence_length_;
      descriptor.next_incoming_sequence_number =
          inward_.parcels.current_sequence_number();
      descriptor.decaying_incoming_sequence_length =
          local_peer->outbound_sequence_length_;

      DVLOG(4) << "Splitting local pair to move " << side_.ToString()
               << " side with outbound sequence length "
               << outbound_sequence_length_ << " and current inbound sequence"
               << " number " << descriptor.next_incoming_sequence_number;

      // The local peer will listen on the new link, rather than this router.
      // This router will only persist to forward its queued inbound parcels
      // on to the new remote router.
      return local_peer;
    }

    // In this case, we're not splitting a local pair, but extending route on
    // this side. This router will become a proxy to the Router serialized here,
    // and will forward inbound parcels to it over a new inward link. Similarly
    // this router will forward outbound parcels from the new Router, to our own
    // outward peer.
    descriptor.route_is_peer = false;
    descriptor.next_outgoing_sequence_number = outbound_sequence_length_;
    descriptor.next_incoming_sequence_number =
        inward_.parcels.current_sequence_number();

    DVLOG(4) << "Extending route on " << side_.ToString() << " side with "
             << " outbound sequence length " << outbound_sequence_length_
             << " and current inbound sequence number "
             << descriptor.next_incoming_sequence_number;

    bool immediate_bypass = false;
    NodeName proxy_peer_node_name;
    RoutingId proxy_peer_routing_id;
    absl::uint128 bypass_key;
    if (status_.flags & IPCZ_PORTAL_STATUS_PEER_CLOSED) {
      descriptor.peer_closed = true;
      descriptor.closed_peer_sequence_length =
          *inward_.parcels.peer_sequence_length();
      inward_.closure_propagated = true;
    } else if (outward_.link && !outward_.link->GetLocalTarget() &&
               outward_.link->IsLinkToOtherSide() && !inward_.link &&
               !outward_.decaying_proxy_link && !inward_.decaying_proxy_link) {
      // If we're becoming a proxy under some common special conditions --
      // namely that no other part of the route is currently decaying -- we can
      // roll the first step of our own decay into this descriptor transmission.
      RemoteRouterLink& remote_link =
          *static_cast<RemoteRouterLink*>(outward_.link.get());
      proxy_peer_node_name = remote_link.node_link()->remote_node_name();
      proxy_peer_routing_id = remote_link.routing_id();
      bypass_key = RandomUint128();
      RouterLinkState::Locked locked(outward_.link->GetLinkState(), side_);
      if (!locked.other_side().is_blocking_decay) {
        immediate_bypass = true;
        locked.this_side().is_blocking_decay = true;
        locked.this_side().bypass_key = bypass_key;
      }
    }

    if (immediate_bypass) {
      DVLOG(4) << "Will initiate proxy bypass immediately on deserialization "
               << "with peer at " << proxy_peer_node_name.ToString() << " and "
               << "peer route to proxy on routing ID " << proxy_peer_routing_id
               << " using " << bypass_key;

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
          descriptor.decaying_incoming_sequence_length > 0
              ? descriptor.decaying_incoming_sequence_length
              : descriptor.next_incoming_sequence_number;

      DVLOG(4) << "Route " << router->side_.ToString() << " side moved from "
               << "split pair on "
               << from_node_link.remote_node_name().ToString() << " to "
               << from_node_link.node()->name().ToString() << " via routing ID "
               << descriptor.new_routing_id << " and decaying routing ID "
               << descriptor.new_decaying_routing_id;
    } else {
      DVLOG(4) << "Route " << router->side_.ToString() << " side extended from "
               << from_node_link.remote_node_name().ToString() << " to "
               << from_node_link.node()->name().ToString() << " via routing ID "
               << descriptor.new_routing_id;
    }
  }

  if (descriptor.proxy_peer_node_name.is_valid()) {
    // Our predecessor has given us the means to initiate its own bypass.
    router->InitiateProxyBypass(from_node_link, descriptor.new_routing_id,
                                descriptor.proxy_peer_node_name,
                                descriptor.proxy_peer_routing_id,
                                descriptor.bypass_key);
  }

  router->Flush();
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

  if (proxy_peer_node_name != requesting_node_link.node()->name()) {
    // Common case: the proxy's outward peer is NOT on the same node as we are.
    // In this case we send a BypassProxy request to that node, which may
    // require an introduction first.
    {
      // Begin decaying our outward link. We don't know the sequence length
      // coming from it yet, but that will be revealed by a subsequent
      // ProxyWillStop message back to us.
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
            // destruction. This is not the same as closure since we can't
            // guarantee any sequence length.
            return;
          }

          new_link->BypassProxy(requesting_node_name, proxy_peer_routing_id,
                                bypass_key, self);
        });
    return true;
  }

  // The proxy's outward peer lives on the same node as this router, so we can
  // skip some messaging and locally link the two routers together right now.

  mem::Ref<RouterLink> previous_outward_link_from_new_local_peer;
  mem::Ref<Router> new_local_peer =
      requesting_node_link.GetRouter(proxy_peer_routing_id);
  SequenceNumber proxied_inbound_sequence_length;
  SequenceNumber proxied_outbound_sequence_length;
  if (!new_local_peer || new_local_peer->side_ != side_.opposite()) {
    return false;
  }

  {
    TwoMutexLock lock(&mutex_, &new_local_peer->mutex_);
    proxied_inbound_sequence_length = new_local_peer->outbound_sequence_length_;
    proxied_outbound_sequence_length = outbound_sequence_length_;
    previous_outward_link_from_new_local_peer =
        std::move(new_local_peer->outward_.link);

    DVLOG(4) << "Initiating proxy bypass with new local peer on "
             << proxy_peer_node_name.ToString() << " and proxy links to "
             << requesting_node_link.remote_node_name().ToString()
             << " on routing IDs " << proxy_peer_routing_id << " and "
             << requesting_routing_id << "; inbound length "
             << proxied_inbound_sequence_length << " and outbound length "
             << proxied_outbound_sequence_length;

    // We get a decaying outward link to the proxy, only to accept inbound
    // parcels already sent to it by our new local peer.
    ABSL_ASSERT(!outward_.decaying_proxy_link);
    outward_.decaying_proxy_link = std::move(outward_.link);
    outward_.sequence_length_from_decaying_link =
        proxied_inbound_sequence_length;
    outward_.sequence_length_to_decaying_link =
        proxied_outbound_sequence_length;

    // Our new local peer gets a decaying outward link to the proxy, only to
    // forward outbound parcels already expected by the proxy.
    ABSL_ASSERT(!new_local_peer->outward_.decaying_proxy_link);
    new_local_peer->outward_.decaying_proxy_link =
        previous_outward_link_from_new_local_peer;
    new_local_peer->outward_.sequence_length_to_decaying_link =
        proxied_inbound_sequence_length;
    new_local_peer->outward_.sequence_length_from_decaying_link =
        proxied_outbound_sequence_length;

    // Finally, create a new LocalRouterLink and use it to replace both our
    // own outward link and our new local peer's outward link.
    TwoSided<mem::Ref<Router>> routers;
    routers[side_] = mem::WrapRefCounted(this);
    routers[side_.opposite()] = new_local_peer;
    TwoSided<mem::Ref<RouterLink>> links = LocalRouterLink::CreatePair(routers);

    // Block any further decay until both sides of the bypass route are
    // finished with the proxy.
    //
    // Note that the use of the left link is arbitrary here: both the left and
    // right links share the same RouterLinkState.
    RouterLinkState& state = links.left()->GetLinkState();
    state.unsafe_sides().left().is_blocking_decay = true;
    state.unsafe_sides().right().is_blocking_decay = true;

    routers.left()->mutex_.AssertHeld();
    routers.left()->outward_.link = std::move(links.left());

    routers.right()->mutex_.AssertHeld();
    routers.right()->outward_.link = std::move(links.right());
  }

  if (!previous_outward_link_from_new_local_peer) {
    // TODO: The local peer must have been closed. Tear down the route.
  } else {
    previous_outward_link_from_new_local_peer->StopProxying(
        proxied_inbound_sequence_length, proxied_outbound_sequence_length);
  }

  Flush();
  new_local_peer->Flush();
  return true;
}

bool Router::BypassProxyWithNewLink(
    mem::Ref<RouterLink> new_peer,
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

    {
      RouterLinkState::Locked state(outward_.link->GetLinkState(), side_);
      if (state.other_side().bypass_key != bypass_key ||
          !state.other_side().is_blocking_decay) {
        return false;
      }
    }

    if (inward_.link) {
      proxy_inbound_sequence_length =
          outward_.parcels.current_sequence_number();
    } else {
      proxy_inbound_sequence_length = outbound_sequence_length_;
    }

    RemoteRouterLink& remote_proxy =
        static_cast<RemoteRouterLink&>(*outward_.link);
    RemoteRouterLink& remote_peer = static_cast<RemoteRouterLink&>(*new_peer);
    const mem::Ref<NodeLink> node_link_to_proxy = remote_proxy.node_link();
    const mem::Ref<NodeLink> node_link_to_peer = remote_peer.node_link();
    DVLOG(4) << "Bypassing proxy at "
             << node_link_to_proxy->remote_node_name().ToString()
             << " on routing ID " << remote_proxy.routing_id() << " from "
             << node_link_to_proxy->node()->name().ToString()
             << " with new link to "
             << node_link_to_peer->remote_node_name().ToString()
             << " on routing ID " << remote_peer.routing_id()
             << "; inbound sequence length " << proxy_inbound_sequence_length
             << " and outbound sequence length "
             << proxy_outbound_sequence_length;

    decaying_outward_link_to_proxy = std::move(outward_.link);
    outward_.decaying_proxy_link = decaying_outward_link_to_proxy;
    outward_.sequence_length_to_decaying_link = proxy_inbound_sequence_length;
    outward_.sequence_length_from_decaying_link =
        proxy_outbound_sequence_length;
    outward_.link = new_peer;
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
    if (!outward_.link) {
      return false;
    }

    if (outward_.link->GetLocalTarget()) {
      // Bogus request, we obviously don't have a link to a proxy on `new_peer`s
      // node, because our outward link is local.
      return false;
    }

    RemoteRouterLink& old_remote_peer =
        static_cast<RemoteRouterLink&>(*outward_.link);
    RemoteRouterLink& new_remote_peer =
        static_cast<RemoteRouterLink&>(*new_peer);
    const mem::Ref<NodeLink> remote_node_link = old_remote_peer.node_link();
    if (new_remote_peer.node_link() != remote_node_link) {
      // Bogus request: our outward link does not go to the same node as
      // `new_peer`.
      return false;
    }

    if (outward_.decaying_proxy_link) {
      return false;
    }

    DVLOG(4) << "Bypassing proxy at "
             << remote_node_link->remote_node_name().ToString()
             << " on routing ID " << old_remote_peer.routing_id() << " from "
             << remote_node_link->node()->name().ToString()
             << " with new routing ID " << new_remote_peer.routing_id();

    decaying_proxy = std::move(outward_.link);
    outward_.link = std::move(new_peer);

    outward_.decaying_proxy_link = decaying_proxy;
    outward_.sequence_length_to_decaying_link = outbound_sequence_length_;
    outward_.sequence_length_from_decaying_link = sequence_length_from_proxy;

    sequence_length_to_proxy = outbound_sequence_length_;
  }

  ABSL_ASSERT(decaying_proxy);
  decaying_proxy->StopProxyingToLocalPeer(sequence_length_to_proxy);
  return true;
}

bool Router::StopProxyingToLocalPeer(SequenceNumber sequence_length) {
  mem::Ref<Router> local_peer;
  {
    absl::MutexLock lock(&mutex_);
    if (!outward_.decaying_proxy_link) {
      return false;
    }

    local_peer = outward_.decaying_proxy_link->GetLocalTarget();
  }

  if (!local_peer) {
    return false;
  }

  TwoMutexLock lock(&mutex_, &local_peer->mutex_);
  if (!local_peer->outward_.decaying_proxy_link ||
      !outward_.decaying_proxy_link || !inward_.decaying_proxy_link) {
    return false;
  }

  // Update all locally decaying links regarding the sequence length from the
  // remote peer to this router -- the decaying proxy -- so that those links may
  // eventually decay.
  local_peer->outward_.sequence_length_from_decaying_link = sequence_length;
  outward_.sequence_length_to_decaying_link = sequence_length;
  inward_.sequence_length_from_decaying_link = sequence_length;
  return true;
}

bool Router::OnProxyWillStop(SequenceNumber sequence_length) {
  {
    absl::MutexLock lock(&mutex_);
    if (!outward_.decaying_proxy_link ||
        outward_.sequence_length_from_decaying_link) {
      return true;
    }

    DVLOG(4) << "Bypassed proxy has finalized its inbound sequence length at "
             << sequence_length << " for "
             << DescribeLink(outward_.decaying_proxy_link);

    outward_.sequence_length_from_decaying_link = sequence_length;
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
  bool outward_this_side_busy = false;
  bool outward_other_side_busy = false;
  if (outward_.link) {
    RouterLinkState::Locked state(outward_.link->GetLinkState(), side_);
    outward_this_side_busy = state.this_side().is_blocking_decay;
    outward_other_side_busy = state.other_side().is_blocking_decay;
  }

  DLOG(INFO) << "## router [" << this << "]";
  DLOG(INFO) << " - side: " << (side_ == Side::kLeft ? "left" : "right");
  DLOG(INFO) << " - paused: " << (outbound_transmission_paused_ ? "yes" : "no");
  DLOG(INFO) << " - status flags: " << status_.flags;
  DLOG(INFO) << " - side closed: " << (side_closed_ ? "yes" : "no");
  DLOG(INFO) << " - outward " << DescribeLink(outward_.link);
  DLOG(INFO) << " - outward decaying "
             << DescribeLink(outward_.decaying_proxy_link);
  DLOG(INFO) << " - outward length to decaying link: "
             << DescribeOptionalLength(
                    outward_.sequence_length_to_decaying_link);
  DLOG(INFO) << " - outward length from decaying link: "
             << DescribeOptionalLength(
                    outward_.sequence_length_from_decaying_link);
  DLOG(INFO) << " - outward busy bits " << outward_this_side_busy << ":"
             << outward_other_side_busy;

  DLOG(INFO) << " - inward " << DescribeLink(inward_.link);
  DLOG(INFO) << " - inward decaying "
             << DescribeLink(inward_.decaying_proxy_link);
  DLOG(INFO) << " - inward length to decaying link: "
             << DescribeOptionalLength(
                    inward_.sequence_length_to_decaying_link);
  DLOG(INFO) << " - inward length from decaying link: "
             << DescribeOptionalLength(
                    inward_.sequence_length_from_decaying_link);
}

void Router::LogRouteTrace(Side toward_side) {
  LogDescription();

  mem::Ref<RouterLink> next_link;
  {
    absl::MutexLock lock(&mutex_);
    if (toward_side == side_) {
      next_link = inward_.link;
    } else {
      next_link = outward_.link;
    }
  }

  if (next_link) {
    next_link->LogRouteTrace(toward_side);
  }
}

void Router::Flush() {
  mem::Ref<RouterLink> inward_link;
  mem::Ref<RouterLink> outward_link;
  mem::Ref<RouterLink> dead_outward_link;
  mem::Ref<RouterLink> decaying_inward_proxy;
  mem::Ref<RouterLink> decaying_outward_proxy;
  absl::InlinedVector<Parcel, 2> outbound_parcels;
  absl::InlinedVector<Parcel, 2> outbound_parcels_to_proxy;
  absl::InlinedVector<Parcel, 2> inbound_parcels;
  absl::InlinedVector<Parcel, 2> inbound_parcels_to_proxy;
  bool inward_proxy_decayed = false;
  bool outward_proxy_decayed = false;
  bool notify_closure_outward = false;
  SequenceNumber final_sequence_length;
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

      DVLOG(4) << "Forwarding outbound " << parcel.Describe()
               << " over outward decaying "
               << DescribeLink(decaying_outward_proxy);

      outbound_parcels_to_proxy.push_back(std::move(parcel));
    }

    // Check now if we can wipe out our decaying outward link.
    const bool still_sending_to_outward_proxy =
        decaying_outward_proxy && inward_.link &&
        outward_.parcels.current_sequence_number() <
            *outward_.sequence_length_to_decaying_link;
    if (decaying_outward_proxy && !still_sending_to_outward_proxy) {
      const bool still_receiving_from_outward_proxy =
          (!outward_.sequence_length_from_decaying_link ||
           inward_.parcels.GetAvailableSequenceLength() <
               *outward_.sequence_length_from_decaying_link);
      if (!still_receiving_from_outward_proxy) {
        DVLOG(4) << "Outward " << DescribeLink(outward_.decaying_proxy_link)
                 << " fully decayed at outbound length "
                 << DescribeOptionalLength(
                        outward_.sequence_length_to_decaying_link)
                 << " and inbound length "
                 << *outward_.sequence_length_from_decaying_link;
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
        DVLOG(4) << "Forwarding outbound " << parcel.Describe()
                 << " over outward " << DescribeLink(outward_link);

        outbound_parcels.push_back(std::move(parcel));
      }
    }

    // Now flush any outbound parcels destined for a decaying proxy.
    while (inward_.parcels.HasNextParcel() && inward_.decaying_proxy_link &&
           inward_.parcels.current_sequence_number() <
               *inward_.sequence_length_to_decaying_link) {
      bool ok = inward_.parcels.Pop(parcel);
      ABSL_ASSERT(ok);

      DVLOG(4) << "Forwarding inbound " << parcel.Describe()
               << " over inward decaying "
               << DescribeLink(decaying_inward_proxy);

      inbound_parcels_to_proxy.push_back(std::move(parcel));
    }

    // Check now if we can wipe out our decaying inward link.
    const bool still_sending_to_inward_proxy =
        decaying_inward_proxy &&
        (!inward_.sequence_length_from_decaying_link ||
         inward_.parcels.current_sequence_number() <
             *inward_.sequence_length_to_decaying_link);
    if (decaying_inward_proxy && !still_sending_to_inward_proxy) {
      const bool still_receiving_from_inward_proxy =
          outward_.parcels.GetAvailableSequenceLength() <
          *inward_.sequence_length_from_decaying_link;
      if (!still_receiving_from_inward_proxy) {
        DVLOG(4) << "Inward " << DescribeLink(inward_.decaying_proxy_link)
                 << " fully decayed at inbound length "
                 << DescribeOptionalLength(
                        inward_.sequence_length_to_decaying_link)
                 << " and outbound length "
                 << *inward_.sequence_length_from_decaying_link;

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
        DVLOG(4) << "Forwarding inbound " << parcel.Describe()
                 << " over inward " << DescribeLink(inward_link);

        inbound_parcels.push_back(std::move(parcel));
      }
    }

    // If the inbound sequence is dead, the other side of the route is gone and
    // we have received all the parcels it sent. We can drop the outward link.
    if (inward_.parcels.IsDead()) {
      std::swap(dead_outward_link, outward_.link);
    }

    if (side_closed_ && !outward_.closure_propagated && outward_.link &&
        !outbound_transmission_paused_) {
      notify_closure_outward = true;
      final_sequence_length = outbound_sequence_length_;

      // If we're closed and have an outward link, our outbound queue should
      // definitely be empty now. This means it's safe to deactivate our outward
      // link: we don't care about inbound parcels if we're closed, and all
      // outbound parcels have been flushed out.
      ABSL_ASSERT(outward_.parcels.IsEmpty());
      std::swap(dead_outward_link, outward_.link);
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
    decaying_outward_proxy.reset();
  }

  if (inward_proxy_decayed) {
    // May delete `this`, as the route binding for `decaying_inward_proxy` may
    // constitute the last reference to this Router.
    decaying_inward_proxy->Deactivate();
    decaying_inward_proxy.reset();
  }

  if (outward_link && (inward_proxy_decayed || outward_proxy_decayed) &&
      (!decaying_inward_proxy && !decaying_outward_proxy)) {
    DVLOG(4) << "Router with fully decayed links may be eligible for "
             << "self-removal with outward " << DescribeLink(outward_link);

    // Just decayed our last link. We may attempt to remove ourself below if
    // other conditions apply, but for now ensure we no longer block decay.
    bool is_other_side_blocking;
    {
      RouterLinkState::Locked state(outward_link->GetLinkState(), side_);
      state.this_side().is_blocking_decay = false;
      is_other_side_blocking = state.other_side().is_blocking_decay;
    }

    if (!MaybeInitiateSelfRemoval() && !is_other_side_blocking) {
      // Ping the other side in case it might want to decay. If it doesn't, this
      // message is a harmless no-op.
      outward_link->DecayUnblocked();
    }
  }

  if (notify_closure_outward) {
    outward_link->AcceptRouteClosure(side_, final_sequence_length);
  }

  if (dead_outward_link) {
    // May delete `this`.
    dead_outward_link->Deactivate();
  }
}

bool Router::MaybeInitiateSelfRemoval() {
  NodeName peer_node_name;
  RoutingId routing_id_to_peer;
  absl::uint128 bypass_key;
  mem::Ref<RouterLink> successor;
  mem::Ref<Router> local_peer;
  {
    absl::MutexLock lock(&mutex_);
    ABSL_ASSERT(!inward_.decaying_proxy_link);
    if (!outward_.link || !inward_.link || outward_.decaying_proxy_link ||
        inward_.decaying_proxy_link || !outward_.link->IsLinkToOtherSide()) {
      // Terminal routers cannot self-remove, as they're controlled by a Portal.
      return false;
    }

    successor = inward_.link;

    // Note: there's a chance we won't use this key if we lose the race to decay
    // below or if we end up decaying in favor of a local peer who needs no key
    // to authenticate. However, generating this while holding the state's
    // spinlock is undesirable, and acquiring the spinlock multiple times is
    // undesirable. This path is rarely hit, so wasting the key should be fine.
    bypass_key = RandomUint128();

    {
      // Finally we can only begin to decay if our peer hasn't already begun
      // decaying itself.
      RouterLinkState::Locked state(outward_.link->GetLinkState(), side_);
      if (state.this_side().is_blocking_decay ||
          state.other_side().is_blocking_decay) {
        DVLOG(4) << "Proxy self-removal blocked by busy "
                 << DescribeLink(outward_.link);
        return false;
      }

      state.this_side().is_blocking_decay = true;
      state.this_side().bypass_key = bypass_key;
    }

    local_peer = outward_.link->GetLocalTarget();
    if (!local_peer) {
      auto& remote_peer = static_cast<RemoteRouterLink&>(*outward_.link);
      peer_node_name = remote_peer.node_link()->remote_node_name();
      routing_id_to_peer = remote_peer.routing_id();
    }
  }

  RemoteRouterLink& remote_successor =
      static_cast<RemoteRouterLink&>(*successor);
  const mem::Ref<NodeLink> node_link_to_successor =
      remote_successor.node_link();
  if (!local_peer) {
    DVLOG(4) << "Proxy at " << node_link_to_successor->node()->name().ToString()
             << " initiating its own bypass with link to successor "
             << node_link_to_successor->remote_node_name().ToString()
             << " on routing ID " << remote_successor.routing_id()
             << " and link to peer " << peer_node_name.ToString()
             << " on routing ID " << routing_id_to_peer;

    successor->RequestProxyBypassInitiation(peer_node_name, routing_id_to_peer,
                                            bypass_key);
    return true;
  }

  SequenceNumber sequence_length;
  const RoutingId new_routing_id =
      node_link_to_successor->AllocateRoutingIds(1);
  mem::Ref<RouterLink> new_link = node_link_to_successor->AddRoute(
      new_routing_id, new_routing_id, local_peer,
      RemoteRouterLink::Type::kToOtherSide);
  {
    TwoMutexLock lock(&mutex_, &local_peer->mutex_);

    // It's possible that the local peer has been closed, in which case its
    // closure will have already propagated to us and there's no bypass work to
    // be done.
    if (!local_peer->outward_.link) {
      ABSL_ASSERT(status_.flags & IPCZ_PORTAL_STATUS_PEER_CLOSED);
      DVLOG(4) << "Proxy self-removal blocked by peer closure.";
      return false;
    }

    DVLOG(4) << "Proxy initiating its own bypass from "
             << node_link_to_successor->remote_node_name().ToString() << " to "
             << "a local peer.";

    // Otherwise it should definitely still be linked to us, because we locked
    // in our own decaying state above.
    ABSL_ASSERT(local_peer->outward_.link->GetLocalTarget() == this);
    ABSL_ASSERT(outward_.link->GetLocalTarget() == local_peer);

    ABSL_ASSERT(!local_peer->outward_.decaying_proxy_link);
    local_peer->outward_.decaying_proxy_link =
        std::move(local_peer->outward_.link);
    local_peer->outward_.sequence_length_to_decaying_link =
        local_peer->outbound_sequence_length_;
    sequence_length = local_peer->outbound_sequence_length_;

    ABSL_ASSERT(!outward_.decaying_proxy_link);
    outward_.decaying_proxy_link = std::move(outward_.link);
    outward_.sequence_length_from_decaying_link =
        local_peer->outbound_sequence_length_;

    ABSL_ASSERT(!inward_.decaying_proxy_link);
    inward_.decaying_proxy_link = std::move(inward_.link);
    inward_.sequence_length_to_decaying_link =
        local_peer->outbound_sequence_length_;

    local_peer->outward_.link = new_link;
    local_peer->outbound_transmission_paused_ = true;

    RouterLinkState& state = new_link->GetLinkState();
    state.unsafe_sides().left().is_blocking_decay = true;
    state.unsafe_sides().right().is_blocking_decay = true;
  }

  successor->BypassProxyToSameNode(new_routing_id, sequence_length);
  local_peer->PauseOutboundTransmission(false);
  return true;
}

Router::RouterSide::RouterSide() = default;

Router::RouterSide::~RouterSide() = default;

}  // namespace core
}  // namespace ipcz
