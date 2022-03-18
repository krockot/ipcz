// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/router.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include "ipcz/ipcz.h"
#include "ipcz/local_router_link.h"
#include "ipcz/node.h"
#include "ipcz/node_link.h"
#include "ipcz/node_name.h"
#include "ipcz/parcel.h"
#include "ipcz/portal.h"
#include "ipcz/remote_router_link.h"
#include "ipcz/router_descriptor.h"
#include "ipcz/router_link.h"
#include "ipcz/router_link_state.h"
#include "ipcz/router_tracker.h"
#include "ipcz/sequence_number.h"
#include "ipcz/sublink_id.h"
#include "ipcz/trap_event_dispatcher.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "util/log.h"
#include "util/mutex_locks.h"
#include "util/random.h"
#include "util/ref_counted.h"

namespace ipcz {

namespace {

std::string DescribeLink(const Ref<RouterLink>& link) {
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

bool Router::HasLocalPeer(const Ref<Router>& other) {
  absl::MutexLock lock(&mutex_);
  return outward_edge_.GetLocalPeer() == other;
}

bool Router::WouldOutboundParcelExceedLimits(size_t data_size,
                                             const IpczPutLimits& limits) {
  Ref<RouterLink> link;
  {
    absl::MutexLock lock(&mutex_);
    if (outbound_parcels_.GetNumAvailableElements() >=
        limits.max_queued_parcels) {
      return true;
    }
    if (outbound_parcels_.GetTotalAvailableElementSize() >
        limits.max_queued_bytes) {
      return true;
    }
    const size_t available_capacity =
        limits.max_queued_bytes -
        outbound_parcels_.GetTotalAvailableElementSize();
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
  if (inbound_parcels_.GetTotalAvailableElementSize() >
      limits.max_queued_bytes) {
    return true;
  }

  const size_t available_capacity =
      limits.max_queued_bytes - inbound_parcels_.GetTotalAvailableElementSize();
  return data_size > available_capacity ||
         inbound_parcels_.GetNumAvailableElements() >=
             limits.max_queued_parcels;
}

IpczResult Router::SendOutboundParcel(absl::Span<const uint8_t> data,
                                      Parcel::ObjectVector& objects) {
  Parcel parcel;
  parcel.SetData(std::vector<uint8_t>(data.begin(), data.end()));
  parcel.SetObjects(std::move(objects));

  Ref<RouterLink> link;
  {
    absl::MutexLock lock(&mutex_);
    ABSL_ASSERT(!inward_edge_);
    const SequenceNumber sequence_number =
        outbound_parcels_.GetCurrentSequenceLength();
    parcel.set_sequence_number(sequence_number);

    if (outbound_parcels_.IsEmpty()) {
      // If there are no outbound parcels queued, we may be able to take a fast
      // path. Note that `link` may still be null here, in which case we will
      // fall back on the slower (queue + flush) path.
      link = outward_edge_.GetLinkToTransmitParcel(sequence_number);
    }

    if (link) {
      // On the fast path we just fix up the queue to start at the next outbound
      // sequence number.
      outbound_parcels_.ResetInitialSequenceNumber(sequence_number + 1);
    } else {
      DVLOG(4) << "Queuing outbound " << parcel.Describe();
      const bool push_ok =
          outbound_parcels_.Push(sequence_number, std::move(parcel));
      ABSL_ASSERT(push_ok);
    }
  }

  if (link) {
    link->AcceptParcel(parcel);
  } else {
    Flush();
  }

  return IPCZ_RESULT_OK;
}

void Router::CloseRoute() {
  TrapEventDispatcher dispatcher;
  {
    absl::MutexLock lock(&mutex_);
    ABSL_ASSERT(!inward_edge_);
    traps_.RemoveAll(dispatcher);
    bool ok = outbound_parcels_.SetFinalSequenceLength(
        outbound_parcels_.GetCurrentSequenceLength());
    ABSL_ASSERT(ok);
  }

  Flush();
}

IpczResult Router::Merge(Ref<Router> other) {
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
        Router::Pair(WrapRefCounted(this), other));
    bridge_->SetPrimaryLink(std::move(links.first));
    other->bridge_->SetPrimaryLink(std::move(links.second));
  }

  Flush();
  return IPCZ_RESULT_OK;
}

void Router::SetOutwardLink(Ref<RouterLink> link) {
  {
    absl::MutexLock lock(&mutex_);
    outward_edge_.SetPrimaryLink(link);
    if (link->GetType().is_central() && outward_edge_.is_stable() &&
        (!inward_edge_ || inward_edge_->is_stable())) {
      link->MarkSideStable();
    }
  }

  Flush(/*force_bypass_attempt=*/true);
}

bool Router::StopProxying(SequenceNumber proxy_inbound_sequence_length,
                          SequenceNumber proxy_outbound_sequence_length) {
  Ref<Router> bridge_peer;
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
    const SequenceNumber sequence_number = parcel.sequence_number();
    if (!inbound_parcels_.Push(sequence_number, std::move(parcel))) {
      return true;
    }

    if (!inward_edge_) {
      status_.num_local_parcels = inbound_parcels_.GetNumAvailableElements();
      status_.num_local_bytes = inbound_parcels_.GetTotalAvailableElementSize();
      traps_.UpdatePortalStatus(status_, TrapSet::UpdateReason::kNewLocalParcel,
                                dispatcher);
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
    const SequenceNumber sequence_number = parcel.sequence_number();
    if (!outbound_parcels_.Push(sequence_number, std::move(parcel))) {
      return false;
    }
  }

  Flush();
  return true;
}

bool Router::AcceptRouteClosureFrom(LinkType link_type,
                                    SequenceNumber sequence_length) {
  TrapEventDispatcher dispatcher;
  {
    absl::MutexLock lock(&mutex_);
    if (link_type == LinkType::kCentral ||
        link_type == LinkType::kPeripheralOutward) {
      if (!inbound_parcels_.SetFinalSequenceLength(sequence_length)) {
        return false;
      }
      if (!inward_edge_ && !bridge_) {
        status_.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
        if (inbound_parcels_.IsDead()) {
          status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
        }
        traps_.UpdatePortalStatus(status_, TrapSet::UpdateReason::kPeerClosed,
                                  dispatcher);
      }
    } else if (link_type == LinkType::kBridge) {
      if (!outbound_parcels_.SetFinalSequenceLength(sequence_length)) {
        return false;
      }
      bridge_.reset();
    }
  }

  Flush();
  return true;
}

void Router::AcceptRouteDisconnectionFrom(LinkType link_type) {
  TrapEventDispatcher dispatcher;
  Ref<RouterLink> forward_primary_link;
  Ref<RouterLink> forward_decayling_link;
  {
    absl::MutexLock lock(&mutex_);
    if (link_type.is_peripheral_inward()) {
      forward_primary_link = outward_edge_.ReleasePrimaryLink();
      forward_decayling_link = outward_edge_.ReleaseDecayingLink();
    } else if (inward_edge_) {
      forward_primary_link = inward_edge_->ReleasePrimaryLink();
      forward_decayling_link = inward_edge_->ReleaseDecayingLink();
    } else if (bridge_) {
      forward_primary_link = bridge_->ReleasePrimaryLink();
      forward_decayling_link = bridge_->ReleaseDecayingLink();
    } else {
      status_.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
      if (!inbound_parcels_.final_sequence_length()) {
        inbound_parcels_.SetFinalSequenceLength(
            inbound_parcels_.GetCurrentSequenceLength());
      }
      if (inbound_parcels_.IsDead()) {
        status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
      }
      traps_.UpdatePortalStatus(status_, TrapSet::UpdateReason::kPeerClosed,
                                dispatcher);
    }
  }

  if (forward_primary_link) {
    forward_primary_link->AcceptRouteDisconnection();
  }
  if (forward_decayling_link) {
    forward_decayling_link->AcceptRouteDisconnection();
  }

  Flush();
}

IpczResult Router::GetNextIncomingParcel(IpczGetFlags flags,
                                         void* data,
                                         uint32_t* num_bytes,
                                         IpczHandle* handles,
                                         uint32_t* num_handles) {
  TrapEventDispatcher dispatcher;
  absl::MutexLock lock(&mutex_);
  if (inward_edge_) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (!inbound_parcels_.HasNextElement()) {
    if (inbound_parcels_.IsDead()) {
      return IPCZ_RESULT_NOT_FOUND;
    }
    return IPCZ_RESULT_UNAVAILABLE;
  }

  Parcel& p = inbound_parcels_.NextElement();
  const bool allow_partial = (flags & IPCZ_GET_PARTIAL) != 0;
  const size_t data_capacity = num_bytes ? *num_bytes : 0;
  const size_t handles_capacity = num_handles ? *num_handles : 0;
  const size_t data_size =
      allow_partial ? std::min(p.data_size(), data_capacity) : p.data_size();
  const size_t handles_size = allow_partial
                                  ? std::min(p.num_objects(), handles_capacity)
                                  : p.num_objects();
  if (num_bytes) {
    *num_bytes = static_cast<uint32_t>(data_size);
  }
  if (num_handles) {
    *num_handles = static_cast<uint32_t>(handles_size);
  }

  const bool consuming_whole_parcel =
      data_capacity >= data_size && handles_capacity >= handles_size;
  if (!consuming_whole_parcel && !allow_partial) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  }

  memcpy(data, p.data_view().data(), data_size);
  const bool ok = inbound_parcels_.Consume(
      data_size, absl::MakeSpan(handles, handles_size));
  ABSL_ASSERT(ok);

  status_.num_local_parcels = inbound_parcels_.GetNumAvailableElements();
  status_.num_local_bytes = inbound_parcels_.GetTotalAvailableElementSize();
  if (inbound_parcels_.IsDead()) {
    status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
    traps_.UpdatePortalStatus(
        status_, TrapSet::UpdateReason::kLocalParcelConsumed, dispatcher);
  }

  return IPCZ_RESULT_OK;
}

IpczResult Router::BeginGetNextIncomingParcel(const void** data,
                                              uint32_t* num_data_bytes,
                                              uint32_t* num_handles) {
  absl::MutexLock lock(&mutex_);
  if (inward_edge_) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (!inbound_parcels_.HasNextElement()) {
    return IPCZ_RESULT_UNAVAILABLE;
  }

  Parcel& p = inbound_parcels_.NextElement();
  const uint32_t data_size = static_cast<uint32_t>(p.data_size());
  const uint32_t handles_size = static_cast<uint32_t>(p.num_objects());
  if (data) {
    *data = p.data_view().data();
  }
  if (num_data_bytes) {
    *num_data_bytes = data_size;
  }
  if (num_handles) {
    *num_handles = handles_size;
  }
  if ((data_size && (!data || !num_data_bytes)) ||
      (handles_size && !num_handles)) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  }

  return IPCZ_RESULT_OK;
}

IpczResult Router::CommitGetNextIncomingParcel(uint32_t num_data_bytes_consumed,
                                               absl::Span<IpczHandle> handles) {
  TrapEventDispatcher dispatcher;
  absl::MutexLock lock(&mutex_);
  if (inward_edge_) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (!inbound_parcels_.HasNextElement()) {
    // If ipcz is used correctly this is impossible.
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  Parcel& p = inbound_parcels_.NextElement();
  if (num_data_bytes_consumed > p.data_size() ||
      handles.size() > p.num_objects()) {
    return IPCZ_RESULT_OUT_OF_RANGE;
  }

  const bool ok = inbound_parcels_.Consume(num_data_bytes_consumed, handles);
  ABSL_ASSERT(ok);

  status_.num_local_parcels = inbound_parcels_.GetNumAvailableElements();
  status_.num_local_bytes = inbound_parcels_.GetTotalAvailableElementSize();
  if (inbound_parcels_.IsDead()) {
    status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
    traps_.UpdatePortalStatus(
        status_, TrapSet::UpdateReason::kLocalParcelConsumed, dispatcher);
  }
  return IPCZ_RESULT_OK;
}

IpczResult Router::Trap(const IpczTrapConditions& conditions,
                        IpczTrapEventHandler handler,
                        uint64_t context,
                        IpczTrapConditionFlags* satisfied_condition_flags,
                        IpczPortalStatus* status) {
  absl::MutexLock lock(&mutex_);
  return traps_.Add(conditions, handler, context, status_,
                    satisfied_condition_flags, status);
}

void Router::SerializeNewRouter(NodeLink& to_node_link,
                                RouterDescriptor& descriptor) {
  TrapEventDispatcher dispatcher;
  Ref<Router> local_peer;
  bool initiate_proxy_bypass = false;
  {
    absl::MutexLock lock(&mutex_);
    traps_.RemoveAll(dispatcher);
    local_peer = outward_edge_.GetLocalPeer();
    initiate_proxy_bypass = outward_edge_.TryLockPrimaryLinkForBypass(
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
  const absl::optional<NodeLink::Sublink> new_sublink =
      to_node_link.GetSublink(descriptor.new_sublink);
  if (!new_sublink) {
    // The sublink has been torn down, presumably because of node disconnection.
    return;
  }

  const absl::optional<NodeLink::Sublink> new_decaying_sublink =
      to_node_link.GetSublink(descriptor.new_decaying_sublink);

  Ref<Router> local_peer;
  {
    absl::MutexLock lock(&mutex_);
    ABSL_ASSERT(inward_edge_);
    if (descriptor.proxy_already_bypassed) {
      Ref<RouterLink> local_peer_link = outward_edge_.ReleasePrimaryLink();
      local_peer = local_peer_link->GetLocalTarget();
      ABSL_ASSERT(local_peer);
      ABSL_ASSERT(new_decaying_sublink);
      inward_edge_->SetPrimaryLink(new_decaying_sublink->router_link);
    } else {
      inward_edge_->SetPrimaryLink(new_sublink->router_link);
    }

    Ref<RouterLink> outward_link = outward_edge_.primary_link();
    if (outward_edge_.is_stable() && outward_link &&
        inward_edge_->is_stable()) {
      outward_link->MarkSideStable();
    }
  }

  if (local_peer) {
    local_peer->SetOutwardLink(new_sublink->router_link);
  }

  Flush(/*force_bypass_attempt=*/true);
}

// static
Ref<Router> Router::Deserialize(const RouterDescriptor& descriptor,
                                NodeLink& from_node_link) {
  auto router = MakeRefCounted<Router>();
  {
    absl::MutexLock lock(&router->mutex_);
    router->outbound_parcels_.ResetInitialSequenceNumber(
        descriptor.next_outgoing_sequence_number);
    router->inbound_parcels_.ResetInitialSequenceNumber(
        descriptor.next_incoming_sequence_number);
    if (descriptor.peer_closed) {
      router->status_.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
      if (!router->inbound_parcels_.SetFinalSequenceLength(
              descriptor.closed_peer_sequence_length)) {
        return nullptr;
      }
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
      Ref<RemoteRouterLink> new_link = from_node_link.AddRemoteRouterLink(
          descriptor.new_decaying_sublink, nullptr,
          LinkType::kPeripheralOutward, LinkSide::kB, router);
      if (!new_link) {
        return nullptr;
      }
      router->outward_edge_.SetPrimaryLink(std::move(new_link));

      router->outward_edge_.StartDecaying(
          router->outbound_parcels_.current_sequence_number(),
          descriptor.decaying_incoming_sequence_length > 0
              ? descriptor.decaying_incoming_sequence_length
              : descriptor.next_incoming_sequence_number);

      new_link = from_node_link.AddRemoteRouterLink(
          descriptor.new_sublink,
          from_node_link.memory().AdoptFragmentRef<RouterLinkState>(
              descriptor.new_link_state_fragment),
          LinkType::kCentral, LinkSide::kB, router);
      if (!new_link) {
        return nullptr;
      }
      router->outward_edge_.SetPrimaryLink(std::move(new_link));

      DVLOG(4) << "Route moved from split pair on "
               << from_node_link.remote_node_name().ToString() << " to "
               << from_node_link.local_node_name().ToString() << " via sublink "
               << descriptor.new_sublink << " and decaying sublink "
               << descriptor.new_decaying_sublink;
    } else {
      Ref<RemoteRouterLink> new_link = from_node_link.AddRemoteRouterLink(
          descriptor.new_sublink, nullptr, LinkType::kPeripheralOutward,
          LinkSide::kB, router);
      if (!new_link) {
        return nullptr;
      }
      router->outward_edge_.SetPrimaryLink(std::move(new_link));

      DVLOG(4) << "Route extended from "
               << from_node_link.remote_node_name().ToString() << " to "
               << from_node_link.local_node_name().ToString() << " via sublink "
               << descriptor.new_sublink;
    }
  }

  if (descriptor.proxy_peer_node_name.is_valid()) {
    // Our predecessor has given us the means to initiate its own bypass.
    router->InitiateProxyBypass(from_node_link, descriptor.new_sublink,
                                descriptor.proxy_peer_node_name,
                                descriptor.proxy_peer_sublink);
  }

  router->Flush(/*force_bypass_attempt=*/true);
  return router;
}

bool Router::InitiateProxyBypass(NodeLink& requesting_node_link,
                                 SublinkId requesting_sublink,
                                 const NodeName& proxy_peer_node_name,
                                 SublinkId proxy_peer_sublink) {
  {
    absl::MutexLock lock(&mutex_);
    if (!outward_edge_.primary_link()) {
      // Must have been disconnected already.
      return true;
    }

    if (!outward_edge_.primary_link()->IsRemoteLinkTo(requesting_node_link,
                                                      requesting_sublink)) {
      // Authenticate that the request to bypass our outward peer is actually
      // coming from our outward peer.
      DLOG(ERROR) << "Rejecting InitiateProxyBypass from "
                  << requesting_node_link.remote_node_name().ToString()
                  << " on sublink " << requesting_sublink
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

    Ref<NodeLink> new_peer_node =
        requesting_node_link.node()->GetLink(proxy_peer_node_name);
    if (new_peer_node) {
      new_peer_node->BypassProxy(
          requesting_node_link.remote_node_name(), proxy_peer_sublink,
          proxy_outbound_sequence_length, WrapRefCounted(this));
      return true;
    }

    requesting_node_link.node()->EstablishLink(
        proxy_peer_node_name,
        [requesting_node_name = requesting_node_link.remote_node_name(),
         proxy_peer_sublink, proxy_outbound_sequence_length,
         self = WrapRefCounted(this)](NodeLink* new_link) {
          if (!new_link) {
            // TODO: failure to connect to a node here should result in route
            // destruction. This is not the same as closure since we can't
            // guarantee any sequence length.
            return;
          }

          new_link->BypassProxy(requesting_node_name, proxy_peer_sublink,
                                proxy_outbound_sequence_length, self);
        });
    return true;
  }

  // The proxy's outward peer lives on the same node as this router, so we can
  // skip some messaging and locally link the two routers together right now.

  Ref<Router> new_local_peer =
      requesting_node_link.GetRouter(proxy_peer_sublink);
  Ref<RouterLink> previous_outward_link_from_new_local_peer;
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
             << " on sublinks " << proxy_peer_sublink << " and "
             << requesting_sublink << "; inbound length "
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
            proxy_inbound_sequence_length, proxy_outbound_sequence_length)) {
      return false;
    }

    // Finally, create a new LocalRouterLink and use it to replace both our
    // own outward link and our new local peer's outward link. The new link is
    // not ready to support bypass until all the above links have fully decayed.
    RouterLink::Pair links = LocalRouterLink::CreatePair(
        LinkType::kCentral, LocalRouterLink::InitialState::kCannotBypass,
        Router::Pair(WrapRefCounted(this), new_local_peer));
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
    Ref<RemoteRouterLink> new_peer,
    SequenceNumber proxy_outbound_sequence_length) {
  SequenceNumber proxy_inbound_sequence_length;
  Ref<RouterLink> decaying_outward_link_to_proxy;
  {
    absl::MutexLock lock(&mutex_);
    if (!outward_edge_.primary_link()) {
      // TODO: terminate the route. not the same as closure.
      return true;
    }

    if (!outward_edge_.CanNodeRequestBypassOfPrimaryLink(
            new_peer->node_link()->remote_node_name())) {
      new_peer->Deactivate();
      return false;
    }

    proxy_inbound_sequence_length = outbound_parcels_.current_sequence_number();

    RemoteRouterLink& remote_proxy =
        static_cast<RemoteRouterLink&>(*outward_edge_.primary_link());
    RemoteRouterLink& remote_peer = static_cast<RemoteRouterLink&>(*new_peer);
    const Ref<NodeLink> node_link_to_proxy = remote_proxy.node_link();
    const Ref<NodeLink> node_link_to_peer = remote_peer.node_link();
    DVLOG(4) << "Bypassing proxy at "
             << node_link_to_proxy->remote_node_name().ToString()
             << " on sublink " << remote_proxy.sublink() << " from "
             << node_link_to_proxy->local_node_name().ToString()
             << " with new link to "
             << node_link_to_peer->remote_node_name().ToString()
             << " on sublink " << remote_peer.sublink()
             << "; inbound sequence length " << proxy_inbound_sequence_length
             << " and outbound sequence length "
             << proxy_outbound_sequence_length;

    decaying_outward_link_to_proxy = outward_edge_.primary_link();
    if (!outward_edge_.StartDecaying(proxy_inbound_sequence_length,
                                     proxy_outbound_sequence_length)) {
      new_peer->Deactivate();
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
    Ref<RouterLink> new_peer,
    SequenceNumber proxy_inbound_sequence_length) {
  Ref<RouterLink> decaying_proxy;
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
      new_peer->Deactivate();
      return false;
    }

    RemoteRouterLink& old_remote_peer =
        static_cast<RemoteRouterLink&>(*outward_edge_.primary_link());
    RemoteRouterLink& new_remote_peer =
        static_cast<RemoteRouterLink&>(*new_peer);
    const Ref<NodeLink> remote_node_link = old_remote_peer.node_link();
    if (new_remote_peer.node_link() != remote_node_link) {
      // Bogus request: our outward link does not go to the same node as
      // `new_peer`.
      new_peer->Deactivate();
      return false;
    }

    DVLOG(4) << "Bypassing proxy at "
             << remote_node_link->remote_node_name().ToString()
             << " on sublink " << old_remote_peer.sublink() << " from "
             << remote_node_link->local_node_name().ToString()
             << " with new sublink " << new_remote_peer.sublink();

    proxy_outbound_sequence_length =
        outbound_parcels_.current_sequence_number();

    decaying_proxy = outward_edge_.primary_link();
    if (!outward_edge_.StartDecaying(proxy_outbound_sequence_length,
                                     proxy_inbound_sequence_length)) {
      new_peer->Deactivate();
      return false;
    }
    outward_edge_.SetPrimaryLink(std::move(new_peer));
  }

  ABSL_ASSERT(decaying_proxy);
  decaying_proxy->StopProxyingToLocalPeer(proxy_outbound_sequence_length);

  Flush();

  return true;
}

bool Router::StopProxyingToLocalPeer(
    SequenceNumber proxy_outbound_sequence_length) {
  Ref<Router> local_peer;
  Ref<Router> bridge_peer;
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

  Ref<RouterLink> next_link;
  {
    absl::MutexLock lock(&mutex_);
    next_link = outward_edge_.primary_link();
  }
  if (next_link) {
    next_link->LogRouteTrace();
  }
}

void Router::AcceptLogRouteTraceFrom(LinkType link_type) {
  LogDescription();

  Ref<RouterLink> next_link;
  {
    absl::MutexLock lock(&mutex_);
    if (link_type.is_central() || link_type.is_peripheral_outward()) {
      if (bridge_) {
        next_link = bridge_->primary_link();
      } else if (inward_edge_) {
        next_link = inward_edge_->primary_link();
      }
    } else {
      next_link = outward_edge_.primary_link();
    }
  }
  if (next_link) {
    next_link->LogRouteTrace();
  }
}

void Router::Flush(bool force_bypass_attempt) {
  Ref<RouterLink> inward_link;
  Ref<RouterLink> outward_link;
  Ref<RouterLink> bridge_link;
  Ref<RouterLink> decaying_inward_link;
  Ref<RouterLink> decaying_outward_link;
  Ref<RouterLink> dead_inward_link;
  Ref<RouterLink> dead_outward_link;
  Ref<RouterLink> dead_bridge_link;
  absl::InlinedVector<Parcel, 2> outbound_parcels;
  absl::InlinedVector<Parcel, 2> outbound_parcels_to_proxy;
  absl::InlinedVector<Parcel, 2> inbound_parcels;
  absl::InlinedVector<Parcel, 2> inbound_parcels_to_proxy;
  absl::InlinedVector<Parcel, 2> bridge_parcels;
  bool inward_link_decayed = false;
  bool outward_link_decayed = false;
  bool on_central_link = false;
  bool dropped_last_decaying_link = false;
  absl::optional<SequenceNumber> final_outward_sequence_length;
  absl::optional<SequenceNumber> final_inward_sequence_length;
  {
    absl::MutexLock lock(&mutex_);
    inward_link = inward_edge_ ? inward_edge_->primary_link() : nullptr;
    outward_link = outward_edge_.primary_link();
    on_central_link = outward_link && outward_link->GetType().is_central();
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

    // If we're dropping the last of our decaying links here, our outward link
    // may now be stable. This can unblock other potential operations such as
    // proxy bypass and closure propagation.
    const bool inward_edge_stable =
        !decaying_inward_link || inward_link_decayed;
    const bool outward_edge_stable =
        !decaying_outward_link || outward_link_decayed;
    const bool both_edges_stable = inward_edge_stable && outward_edge_stable;
    const bool either_link_decayed =
        inward_link_decayed || outward_link_decayed;
    if (on_central_link && either_link_decayed && both_edges_stable) {
      DVLOG(4) << "Router with fully decayed links may be eligible for "
               << "self-removal with outward " << DescribeLink(outward_link);
      outward_link->MarkSideStable();
      dropped_last_decaying_link = true;
    }

    if (on_central_link && outbound_parcels_.IsDead() &&
        outward_link->TryLockForClosure()) {
      // If the outbound sequence is dead, our side of the route is gone and we
      // have no more parcels to transmit. Attempt to propagate our closure,
      // which we can only do if we're also the only router left on our side of
      // the route and the central link can be locked.
      dead_outward_link = outward_edge_.ReleasePrimaryLink();
      final_outward_sequence_length = outbound_parcels_.final_sequence_length();
    } else if (!inbound_parcels_.ExpectsMoreElements()) {
      // If we expect no more inbound parcels, the other side of the route is
      // gone and we've already received everything it sent. We can also drop
      // the outward link in this case.
      dead_outward_link = outward_edge_.ReleasePrimaryLink();
    }

    if (inbound_parcels_.IsDead()) {
      // If the inbound parcel queue is dead, we've not only received all
      // inbound parcels, but we've also forwarded all of them if applicable.
      // We can therefore also drop any inward or bridge link.
      final_inward_sequence_length = inbound_parcels_.final_sequence_length();
      if (inward_edge_) {
        dead_inward_link = inward_edge_->ReleasePrimaryLink();
      } else {
        dead_bridge_link = bridge_link;
        bridge_.reset();
      }
    }
  }

  if (on_central_link) {
    outward_link->ShareLinkStateMemoryIfNecessary();
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
  }

  if (inward_link_decayed) {
    decaying_inward_link->Deactivate();
  }

  if (bridge_link && outward_link && !inward_link && !decaying_inward_link &&
      !decaying_outward_link) {
    MaybeInitiateBridgeBypass();
  }

  if (dead_outward_link) {
    if (final_outward_sequence_length) {
      dead_outward_link->AcceptRouteClosure(*final_outward_sequence_length);
    }
    dead_outward_link->Deactivate();
  }

  if (dead_inward_link) {
    if (final_inward_sequence_length) {
      dead_inward_link->AcceptRouteClosure(*final_inward_sequence_length);
    }
    dead_inward_link->Deactivate();
  }

  if (dead_bridge_link) {
    if (final_inward_sequence_length) {
      dead_bridge_link->AcceptRouteClosure(*final_inward_sequence_length);
    }
  }

  // Beyond this point we're concerned with managing proxy bypass. Either this
  // router or the router on the other side of the link *may* be eligible.

  if (dead_outward_link || !on_central_link) {
    // No more link or not a central link; no bypass possible.
    return;
  }

  if (!dropped_last_decaying_link && !force_bypass_attempt) {
    // No relevant state changes, no bypass possible.
    return;
  }

  if (inward_link && MaybeInitiateSelfRemoval()) {
    // Our own bypass has been successfully initiated.
    return;
  }

  outward_link->FlushOtherSideIfWaiting();
}

void Router::NotifyLinkDisconnected(const NodeLink& link, SublinkId sublink) {
  bool outward_disconnect = false;
  bool inward_disconnect = false;
  {
    absl::MutexLock lock(&mutex_);
    if (outward_edge_.IsRoutedThrough(link, sublink)) {
      outward_disconnect = true;
    } else if (inward_edge_ && inward_edge_->IsRoutedThrough(link, sublink)) {
      inward_disconnect = true;
    }
  }

  if (outward_disconnect) {
    AcceptRouteDisconnectionFrom(LinkType::kPeripheralOutward);
  } else if (inward_disconnect) {
    AcceptRouteDisconnectionFrom(LinkType::kPeripheralInward);
  }
}

bool Router::MaybeInitiateSelfRemoval() {
  NodeName peer_node_name;
  SublinkId sublink_to_peer;
  Ref<RemoteRouterLink> successor;
  Ref<Router> local_peer;
  {
    absl::MutexLock lock(&mutex_);
    if (!inward_edge_ || !inward_edge_->is_stable()) {
      return false;
    }

    ABSL_ASSERT(!inward_edge_->GetLocalPeer());
    successor = WrapRefCounted(
        static_cast<RemoteRouterLink*>(inward_edge_->primary_link().get()));

    if (!outward_edge_.TryLockPrimaryLinkForBypass(
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
      sublink_to_peer = remote_peer.sublink();
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
             << " on sublink " << successor->sublink() << " and link to peer "
             << peer_node_name.ToString() << " on sublink " << sublink_to_peer;
    successor->RequestProxyBypassInitiation(peer_node_name, sublink_to_peer);
    return true;
  }

  SequenceNumber sequence_length;
  const SublinkId new_sublink =
      successor->node_link()->memory().AllocateSublinkIds(1);
  FragmentRef<RouterLinkState> new_link_state =
      successor->node_link()->memory().AllocateRouterLinkState();
  Ref<RouterLink> new_link = successor->node_link()->AddRemoteRouterLink(
      new_sublink, new_link_state, LinkType::kCentral, LinkSide::kA,
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

  successor->BypassProxyToSameNode(new_sublink, std::move(new_link_state),
                                   sequence_length);
  local_peer->SetOutwardLink(std::move(new_link));
  return true;
}

void Router::MaybeInitiateBridgeBypass() {
  Ref<Router> first_bridge = WrapRefCounted(this);
  Ref<Router> second_bridge;
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

  Ref<Router> first_local_peer;
  Ref<Router> second_local_peer;
  Ref<RouterLink> link_to_first_peer;
  Ref<RouterLink> link_to_second_peer;
  Ref<RemoteRouterLink> remote_link_to_second_peer;
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
      remote_link_to_second_peer = WrapRefCounted(
          static_cast<RemoteRouterLink*>(link_to_second_peer.get()));
      second_peer_node_name =
          remote_link_to_second_peer->node_link()->remote_node_name();
    }

    if (!link_to_first_peer->TryLockForBypass(second_peer_node_name)) {
      return;
    }
    if (!link_to_second_peer->TryLockForBypass()) {
      // Cancel the decay on this bridge's side, because we couldn't decay the
      // other side of the bridge yet.
      link_to_first_peer->Unlock();
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
        remote_link_to_second_peer->sublink());
    return;
  }

  if (!second_local_peer) {
    // This case is equivalent to basic proxy bypass when the proxy and its
    // outward peer are local to the same node. In this case we only need to
    // lock all the local routers to set up decaying links and use the same
    // BypassProxyToSameNode message used in the non-bridging case. As with
    // StopProxying() above, StopProxyingToLocalPeer() has logic to handle the
    // case where the receiving proxy is a route bridge.

    const Ref<NodeLink> node_link_to_second_peer =
        remote_link_to_second_peer->node_link();
    SequenceNumber length_from_local_peer;
    const SublinkId bypass_sublink =
        node_link_to_second_peer->memory().AllocateSublinkIds(1);
    FragmentRef<RouterLinkState> bypass_link_state =
        node_link_to_second_peer->memory().AllocateRouterLinkState();
    Ref<RouterLink> new_link = node_link_to_second_peer->AddRemoteRouterLink(
        bypass_sublink, bypass_link_state, LinkType::kCentral, LinkSide::kA,
        first_local_peer);
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
        bypass_sublink, std::move(bypass_link_state), length_from_local_peer);
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
                                             Ref<Router> local_peer) {
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

  // The primary new sublink to the destination node will act as the route's
  // new central link, between our local peer and the new remote router.
  //
  // An additional route is allocated to act as a decaying inward link between
  // us and the new router, to forward any parcels already queued here or
  // in-flight from our local peer.
  const SublinkId new_sublink = to_node_link.memory().AllocateSublinkIds(2);
  FragmentRef<RouterLinkState> new_link_state =
      to_node_link.memory().AllocateRouterLinkState();
  const SublinkId decaying_sublink = new_sublink + 1;

  // Register the new routes on the NodeLink. Note that we don't provide them to
  // any routers yet since we don't want the routers using them until this
  // descriptor is transmitted to its destination node. The links will be
  // adopted after transmission in BeginProxyingToNewRouter().
  Ref<RouterLink> new_link = to_node_link.AddRemoteRouterLink(
      new_sublink, new_link_state, LinkType::kCentral, LinkSide::kA,
      local_peer);

  // The local peer's side of the link has nothing to decay, so it can
  // immediately raise its support for bypass.
  new_link->MarkSideStable();

  to_node_link.AddRemoteRouterLink(decaying_sublink, nullptr,
                                   LinkType::kPeripheralInward, LinkSide::kA,
                                   WrapRefCounted(this));

  descriptor.new_sublink = new_sublink;
  descriptor.new_link_state_fragment = new_link_state.release().descriptor();
  descriptor.new_decaying_sublink = decaying_sublink;
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

  if (inbound_parcels_.final_sequence_length()) {
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
      descriptor.proxy_peer_sublink = remote_link.sublink();
      DVLOG(4) << "Will initiate proxy bypass immediately on deserialization "
               << "with peer at " << descriptor.proxy_peer_node_name.ToString()
               << " and peer route to proxy on sublink "
               << descriptor.proxy_peer_sublink;

      // Immediately begin decaying inward and outward edges. They'll get
      // concrete links to decay in BeginProxyingToNewRouter().
      inward_edge_->StartDecaying();
      outward_edge_.StartDecaying();
    }
  }

  const SublinkId new_sublink = to_node_link.memory().AllocateSublinkIds(1);
  descriptor.new_sublink = new_sublink;

  // Register the new router link with the NodeLink. We don't provide this to
  // the router yet because it's not safe to use until this descriptor has been
  // transmitted to its destination node. The link will be adopted after
  // transmission, in BeginProxyingToNewRouter().
  to_node_link.AddRemoteRouterLink(new_sublink, nullptr,
                                   LinkType::kPeripheralInward, LinkSide::kA,
                                   WrapRefCounted(this));
}

}  // namespace ipcz
