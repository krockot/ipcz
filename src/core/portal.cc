// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/portal.h"

#include <limits>
#include <memory>
#include <utility>

#include "core/node.h"
#include "core/node_link.h"
#include "core/parcel.h"
#include "core/portal_descriptor.h"
#include "core/portal_link_state.h"
#include "core/routing_mode.h"
#include "core/trap.h"
#include "debug/log.h"
#include "mem/ref_counted.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/numeric/int128.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/handle_util.h"
#include "util/random.h"

namespace ipcz {
namespace core {

namespace {

Parcel::PortalVector AcquirePortalsForTransit(
    absl::Span<const IpczHandle> handles) {
  Parcel::PortalVector portals(handles.size());
  for (size_t i = 0; i < handles.size(); ++i) {
    portals[i] = mem::WrapRefCounted(ToPtr<Portal>(handles[i]));
  }
  return portals;
}

void ReleasePortalsAfterTransit(absl::Span<const IpczHandle> handles) {
  for (IpczHandle handle : handles) {
    // Acquire and immediately deref the reference owned by the application's
    // handle to the portal.
    mem::Ref<Portal>(mem::RefCounted::kAdoptExistingRef, ToPtr<Portal>(handle));
  }
}

std::vector<os::Handle> AcquireOSHandlesForTransit(
    absl::Span<const IpczOSHandle> handles) {
  std::vector<os::Handle> os_handles;
  os_handles.reserve(handles.size());
  for (const IpczOSHandle& handle : handles) {
    os_handles.push_back(os::Handle::FromIpczOSHandle(handle));
  }
  return os_handles;
}

void ReleaseOSHandlesFromCancelledTransit(absl::Span<os::Handle> handles) {
  for (os::Handle& handle : handles) {
    (void)handle.release();
  }
}

bool MatchPortalLink(const mem::Ref<PortalLink>& portal_link,
                     NodeLink& node_link,
                     RoutingId routing_id) {
  if (!portal_link) {
    return false;
  }

  return &portal_link->node() == &node_link &&
         portal_link->routing_id() == routing_id;
}

}  // namespace

Portal::Portal(mem::Ref<Node> node, Side side)
    : node_(std::move(node)), side_(side) {}

Portal::~Portal() = default;

// static
Portal::Pair Portal::CreateLocalPair(mem::Ref<Node> node) {
  auto left = mem::MakeRefCounted<Portal>(node, Side::kLeft);
  auto right = mem::MakeRefCounted<Portal>(std::move(node), Side::kRight);
  {
    absl::MutexLock lock(&left->mutex_);
    left->local_peer_ = right;
  }
  {
    absl::MutexLock lock(&right->mutex_);
    right->local_peer_ = left;
  }
  return {std::move(left), std::move(right)};
}

mem::Ref<Portal> Portal::Serialize(PortalDescriptor& descriptor) {
  // TODO: need to support sending both portals of a local pair in the same
  // parcel. one idea that could work:
  //
  //   leave each local portal with nothing but a successor link to their new
  //   portals - use these to forward unread incoming parcels. use a one-off
  //   field in PortalDescriptor to say "peer is this other descriptor X" on
  //   the receiving end.

  PortalLock lock(*this);

  // If we're just being serialized, we're only creating our successor now. We
  // can't possibly have one yet.
  ABSL_ASSERT(!successor_link_);

  if (local_peer_) {
    // We're part of a local portal pair. Ensure our local peer is split from us
    // so they don't modify our state further. If transit is cancelled we can
    // recover a reference to them and fix them back up with us later.
    Portal& peer = *local_peer_;
    peer.mutex_.AssertHeld();
    peer.local_peer_.reset();

    descriptor.route_is_peer = true;
    descriptor.side = side_;
    descriptor.peer_closed = peer.closed_;
    descriptor.peer_sequence_length =
        peer.closed_ ? *incoming_parcels_.peer_sequence_length() : 0;
    descriptor.next_incoming_sequence_number =
        incoming_parcels_.current_sequence_number();
    descriptor.next_outgoing_sequence_number =
        *peer.incoming_parcels_.GetNextExpectedSequenceNumber();

    // Take any unread parcels from the moving portal and stick them in the
    // outgoing queue of its locally retained peer. They will be transmitted
    // from there to the new portal once it's ready.
    Parcel parcel;
    OutgoingParcelQueue& saved_parcels = peer.outgoing_parcels_;
    while (incoming_parcels_.Pop(parcel)) {
      saved_parcels.push(std::move(parcel));
    }
    return local_peer_;
  }

  descriptor.side = side_;
  descriptor.next_incoming_sequence_number =
      incoming_parcels_.current_sequence_number();
  descriptor.next_outgoing_sequence_number = next_outgoing_sequence_number_;
  descriptor.peer_closed = (status_.flags & IPCZ_PORTAL_STATUS_PEER_CLOSED);
  descriptor.peer_sequence_length =
      incoming_parcels_.peer_sequence_length().value_or(0);

  if (peer_link_) {
    ABSL_ASSERT(routing_mode_ == RoutingMode::kActive);

    bool will_half_proxy = false;
    {
      PortalLinkState::Locked state(peer_link_->state(), side_);
      PortalLinkState::SideState& this_side = state.this_side();
      PortalLinkState::SideState& other_side = state.other_side();
      switch (other_side.routing_mode) {
        case RoutingMode::kClosed:
        case RoutingMode::kActive:
        case RoutingMode::kFullProxy:
          will_half_proxy = true;
          this_side.routing_mode = RoutingMode::kHalfProxy;
          break;
        case RoutingMode::kBuffering:
        case RoutingMode::kHalfProxy:
          this_side.routing_mode = RoutingMode::kFullProxy;
          break;
      }
    }

    if (will_half_proxy) {
      ABSL_ASSERT(!predecessor_link_);

      routing_mode_ = RoutingMode::kHalfProxy;

      // We're switching to half-proxying mode. Give the new portal some
      // information they can use to authenticate against our peer and
      // ultimately convince it to bypass us along the route.
      const absl::uint128 key = RandomUint128();
      descriptor.peer_name = *peer_link_->node().GetRemoteName();
      descriptor.peer_routing_id = peer_link_->routing_id();
      descriptor.peer_key = key;

      // And make sure the peer has access to the same key by the time our
      // successor tries to authenticate.
      PortalLinkState::Locked state(peer_link_->state(), side_);
      state.this_side().successor_key = key;
    } else {
      routing_mode_ = RoutingMode::kFullProxy;
    }

    return mem::WrapRefCounted(this);
  }

  if (predecessor_link_) {
    routing_mode_ = RoutingMode::kFullProxy;
    PortalLinkState::Locked state(predecessor_link_->state(),
                                  Side::kPredecessor);
    state.this_side().routing_mode = RoutingMode::kFullProxy;
    return mem::WrapRefCounted(this);
  }

  // Buffering portals stay buffering even once they have a successor.
  ABSL_ASSERT(routing_mode_ == RoutingMode::kBuffering);
  return mem::WrapRefCounted(this);
}

// static
mem::Ref<Portal> Portal::DeserializeNew(const mem::Ref<Node>& on_node,
                                        const mem::Ref<NodeLink>& from_node,
                                        os::Memory::Mapping link_state_mapping,
                                        const PortalDescriptor& descriptor) {
  auto portal = mem::MakeRefCounted<Portal>(on_node, descriptor.side);
  if (!portal->Deserialize(from_node, std::move(link_state_mapping),
                           descriptor)) {
    return nullptr;
  }
  return portal;
}

SequenceNumber Portal::ActivateFromBuffering(mem::Ref<PortalLink> peer) {
  SequenceNumber first_non_buffered_sequence_number;
  absl::optional<SequenceNumber> sequence_length;
  std::forward_list<Parcel> parcels_to_forward;
  mem::Ref<PortalLink> successor;
  absl::uint128 bypass_key;
  {
    PortalLock lock(*this);
    ABSL_ASSERT(!local_peer_);
    ABSL_ASSERT(!peer_link_);
    ABSL_ASSERT(routing_mode_ == RoutingMode::kBuffering);

    // TODO: we may need to plumb a TrapEventDispatcher here and into
    // SendParcel() in case something goes wrong and we end up discarding (and
    // closing) portal attachments locally.

    if (successor_link_) {
      successor = successor_link_;
      bypass_key = RandomUint128();
      routing_mode_ = RoutingMode::kHalfProxy;

      PortalLinkState::Locked state(peer->state(), side_);
      state.this_side().routing_mode = RoutingMode::kHalfProxy;
      state.this_side().successor_key = bypass_key;
    } else {
      routing_mode_ = RoutingMode::kActive;
    }
    peer_link_ = peer;
    parcels_to_forward = outgoing_parcels_.TakeParcels();
    first_non_buffered_sequence_number = next_outgoing_sequence_number_;
    if (closed_) {
      sequence_length = next_outgoing_sequence_number_;
    }
  }

  for (Parcel& parcel : parcels_to_forward) {
    peer->SendParcel(parcel);
  }

  if (sequence_length) {
    peer->NotifyClosed(*sequence_length);
  } else if (successor) {
    successor->InitiateProxyBypass(*peer->node().GetRemoteName(),
                                   peer->routing_id(), bypass_key);
  }

  return first_non_buffered_sequence_number;
}

void Portal::BeginForwardProxying(mem::Ref<PortalLink> successor) {
  std::vector<Parcel> parcels;
  {
    PortalLock lock(*this);
    ABSL_ASSERT(!successor_link_);
    successor_link_ = successor;
  }

  ForwardParcels();
}

bool Portal::AcceptParcelFromLink(NodeLink& link,
                                  RoutingId routing_id,
                                  Parcel& parcel,
                                  TrapEventDispatcher& dispatcher) {
  mem::Ref<PortalLink> immediate_forwarding_link;
  {
    absl::MutexLock lock(&mutex_);
    if (MatchPortalLink(predecessor_link_, link, routing_id) ||
        MatchPortalLink(peer_link_, link, routing_id)) {
      // The parcel is coming from the other side of the route.
      if (!incoming_parcels_.Push(std::move(parcel))) {
        return false;
      }
    } else if (MatchPortalLink(successor_link_, link, routing_id)) {
      // The parcel comes from our successor, what we do with it depends on our
      // state. If we're a half-proxy, we must have decayed from a full proxy,
      // so we queue (for tracking) and it will be forwarded below. Otherwise we
      // can forward it straight away.
      if (routing_mode_ == RoutingMode::kFullProxy) {
        immediate_forwarding_link = peer_link_ ? peer_link_ : predecessor_link_;
      } else if (outgoing_parcels_from_successor_) {
        ABSL_ASSERT(routing_mode_ == RoutingMode::kHalfProxy);
        if (!outgoing_parcels_from_successor_->Push(std::move(parcel))) {
          return false;
        }
      } else {
        // Unexpected parcel from successor.
        return false;
      }
    } else {
      // Should be unreachable, but if there are any edge cases where a stale
      // route could try to deliver an unexpected message, treat them as a
      // validation failure.
      return false;
    }

    if (routing_mode_ == RoutingMode::kActive) {
      status_.num_local_bytes = incoming_parcels_.GetNumAvailableBytes();
      status_.num_local_parcels = incoming_parcels_.GetNumAvailableParcels();
      traps_.MaybeNotify(dispatcher, status_);
      return true;
    }
  }

  if (immediate_forwarding_link) {
    immediate_forwarding_link->SendParcel(parcel);
  } else {
    ForwardParcels();
  }
  return true;
}

bool Portal::NotifyPeerClosed(SequenceNumber sequence_length,
                              TrapEventDispatcher& dispatcher) {
  mem::Ref<PortalLink> successor;
  {
    absl::MutexLock lock(&mutex_);
    status_.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
    incoming_parcels_.SetPeerSequenceLength(sequence_length);
    if (incoming_parcels_.IsDead()) {
      status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
    }

    // TODO: Need to clear `outgoing_parcels_` here and manually close all
    // Portals attached to any outgoing parcels. This in turn will need a
    // TrapEventDispatcher to be plumbed through.
    outgoing_parcels_.clear();

    successor = successor_link_;
    if (routing_mode_ == RoutingMode::kActive) {
      traps_.MaybeNotify(dispatcher, status_);
    }
  }

  if (successor) {
    successor->NotifyClosed(sequence_length);
  }

  return true;
}

bool Portal::ReplacePeerLink(absl::uint128 key,
                             const mem::Ref<PortalLink>& new_peer) {
  absl::uint128 bypass_key;
  mem::Ref<PortalLink> successor_to_initiate_proxy_bypass;
  {
    absl::MutexLock lock(&mutex_);
    if (peer_link_) {
      {
        PortalLinkState::Locked state(peer_link_->state(), side_);
        if (state.other_side().successor_key != key) {
          // Treat key mismatch as a validation failure.
          return false;
        }
      }

      // If we're a full proxy and we have a successor that could bypass us, we
      // want to decay to a half-proxy.
      bool should_decay =
          routing_mode_ == RoutingMode::kFullProxy && successor_link_;
      if (should_decay) {
        bypass_key = RandomUint128();
      }

      {
        PortalLinkState::Locked state(new_peer->state(), side_);
        if (should_decay &&
            state.other_side().routing_mode != RoutingMode::kHalfProxy &&
            state.other_side().routing_mode != RoutingMode::kBuffering) {
          // If we are a full proxy and our peer is neither buffering nor
          // half-proxying, we can lock in our decay to a half-proxy now.
          state.this_side().routing_mode = RoutingMode::kHalfProxy;
          state.this_side().successor_key = bypass_key;
        } else {
          state.this_side().routing_mode = routing_mode_;
          should_decay = false;
        }
      }

      peer_link_->StopProxyingTowardSide(Opposite(side_),
                                         next_outgoing_sequence_number_);
      peer_link_ = new_peer;

      if (should_decay) {
        routing_mode_ = RoutingMode::kHalfProxy;
        successor_to_initiate_proxy_bypass = successor_link_;
      }
    }
  }

  if (successor_to_initiate_proxy_bypass) {
    // Prepare to track the remaining in-flight parcels from our successor so we
    // can know when we're done forwarding to the peer or predecessor. The
    // successor will send back a StopProxyingTowardSide() which we'll use to
    // cap the sequence length of this queue.
    {
      absl::MutexLock lock(&mutex_);
      outgoing_parcels_from_successor_.emplace();
    }
    successor_to_initiate_proxy_bypass->InitiateProxyBypass(
        *new_peer->node().GetRemoteName(), new_peer->routing_id(), bypass_key);
  }

  return true;
}

bool Portal::StopProxyingTowardSide(Side side, SequenceNumber sequence_length) {
  {
    absl::MutexLock lock(&mutex_);
    if (side == side_) {
      incoming_parcels_.SetPeerSequenceLength(sequence_length);
    } else if (outgoing_parcels_from_successor_) {
      outgoing_parcels_from_successor_->SetPeerSequenceLength(sequence_length);
    } else {
      // Unexpected request.
      return false;
    }
  }

  ForwardParcels();
  return true;
}

bool Portal::InitiateProxyBypass(const NodeName& peer_name,
                                 RoutingId peer_proxy_routing_id,
                                 absl::uint128 bypass_key,
                                 bool notify_predecessor) {
  SequenceNumber sequence_length_to_predecessor;
  mem::Ref<PortalLink> predecessor;
  bool is_buffering;
  {
    absl::MutexLock lock(&mutex_);
    if (!predecessor_link_) {
      // Must have lost the predecessor link. We can assume its process has
      // terminated, and the resulting route closure will propagate by other
      // means. Nothing to do here.
      return true;
    }

    if (peer_link_) {
      // A well-behaved system will never ask a portal to bypass a proxy when
      // the portal already has a peer link.
      return false;
    }

    predecessor = predecessor_link_;
    is_buffering = routing_mode_ == RoutingMode::kBuffering;
  }

  const NodeName proxy_name = *predecessor->node().GetRemoteName();
  ABSL_ASSERT(peer_name.is_valid());
  mem::Ref<NodeLink> peer_node = node_->GetLink(peer_name);
  if (peer_node) {
    mem::Ref<PortalLink> bypass_link =
        peer_node->BypassProxyToPortal(proxy_name, peer_proxy_routing_id,
                                       bypass_key, mem::WrapRefCounted(this));

    if (is_buffering) {
      sequence_length_to_predecessor =
          ActivateFromBuffering(std::move(bypass_link));
    } else {
      // The portal is already active, so just start using the bypass link as
      // its peer instead of forwarding parcels to the predecessor.
      absl::MutexLock lock(&mutex_);
      sequence_length_to_predecessor = next_outgoing_sequence_number_;
      peer_link_ = std::move(bypass_link);
    }

    if (notify_predecessor) {
      predecessor->StopProxyingTowardSide(Opposite(side_),
                                          sequence_length_to_predecessor);
    }
    return true;
  }

  {
    absl::MutexLock lock(&mutex_);
    routing_mode_ = RoutingMode::kBuffering;
  }

  node_->EstablishLink(peer_name, [portal = mem::WrapRefCounted(this),
                                   peer_routing_id = peer_proxy_routing_id,
                                   bypass_key, notify_predecessor, predecessor,
                                   proxy_name](const mem::Ref<NodeLink>& link) {
    if (!link) {
      // TODO: handle gracefully
      LOG(ERROR) << "failed to establish new NodeLink";
      return;
    }

    SequenceNumber sequence_length_to_predecessor =
        portal->ActivateFromBuffering(link->BypassProxyToPortal(
            proxy_name, peer_routing_id, bypass_key, portal));
    if (notify_predecessor) {
      predecessor->StopProxyingTowardSide(Opposite(portal->side()),
                                          sequence_length_to_predecessor);
    }
  });
  return true;
}

IpczResult Portal::Close() {
  // TODO: Plumb a TrapEventDispatcher to Close() so it can queue events.
  std::vector<mem::Ref<Portal>> other_portals_to_close;
  {
    PortalLock lock(*this);
    ABSL_ASSERT(!closed_);

    // Proxying portals cannot be closed because they're unreachable to the
    // application. They will self-destruct when appropriate.
    ABSL_ASSERT(routing_mode_ == RoutingMode::kActive ||
                routing_mode_ == RoutingMode::kBuffering);
    ABSL_ASSERT(!successor_link_);
    closed_ = true;

    if (local_peer_) {
      Portal& peer = *local_peer_;
      peer.mutex_.AssertHeld();

      // Fast path: our peer is local so we can update its status directly.
      //
      // TODO: it's possible that we are still waiting for incoming parcels we
      // know to be in flight. Ensure that our local peer knows this so it
      // doesn't appear dead just because peer closure is flagged.
      peer.status_.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
      peer.incoming_parcels_.SetPeerSequenceLength(
          next_outgoing_sequence_number_);
      if (peer.incoming_parcels_.IsDead()) {
        peer.status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
      }

      // TODO: poke peer's traps
      return IPCZ_RESULT_OK;
    }

    // Signal our closure ASAP via the shared link state to reduce the potential
    // for redundant work on the peer's end.
    if (peer_link_) {
      routing_mode_ = RoutingMode::kClosed;
      {
        PortalLinkState::Locked state(peer_link_->state(), side_);
        state.this_side().routing_mode = RoutingMode::kClosed;
      }

      // We should never have outgoing parcels buffered while we have a peer
      // link.
      ABSL_ASSERT(outgoing_parcels_.empty());

      // TODO: Just signal?
      peer_link_->NotifyClosed(next_outgoing_sequence_number_);
    }

    // todo forward closure to predecessor when present without a peer
  }

  // TODO: do this iteratively rather than recursively since we might nest
  // arbitrarily deep
  for (mem::Ref<Portal>& portal : other_portals_to_close) {
    portal->Close();
  }
  return IPCZ_RESULT_OK;
}

IpczResult Portal::QueryStatus(IpczPortalStatus& status) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(!closed_);
  status = status_;
  return IPCZ_RESULT_OK;
}

IpczResult Portal::Put(absl::Span<const uint8_t> data,
                       absl::Span<const IpczHandle> portals,
                       absl::Span<const IpczOSHandle> os_handles,
                       const IpczPutLimits* limits) {
  auto portals_to_send = AcquirePortalsForTransit(portals);
  if (!ValidatePortalsToSendFromHere(absl::MakeSpan(portals_to_send))) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  std::vector<os::Handle> acquired_os_handles =
      AcquireOSHandlesForTransit(os_handles);
  IpczResult result =
      PutImpl(data, portals_to_send, acquired_os_handles, limits,
              /*is_two_phase_commit=*/false);
  if (result == IPCZ_RESULT_OK) {
    // Great job!
    ReleasePortalsAfterTransit(portals);
    return IPCZ_RESULT_OK;
  }

  ReleaseOSHandlesFromCancelledTransit(absl::MakeSpan(acquired_os_handles));
  return result;
}

IpczResult Portal::BeginPut(IpczBeginPutFlags flags,
                            const IpczPutLimits* limits,
                            uint32_t& num_data_bytes,
                            void** data) {
  PortalLock lock(*this);
  if (pending_parcel_) {
    return IPCZ_RESULT_ALREADY_EXISTS;
  }

  IpczResult result = ValidatePutLimits(num_data_bytes, limits);
  if (result != IPCZ_RESULT_OK) {
    return result;
  }

  if (local_peer_ && local_peer_->closed_) {
    return IPCZ_RESULT_NOT_FOUND;
  }

  if (peer_link_) {
    PortalLinkState::Locked link_state(peer_link_->state(), side_);
    if (link_state.other_side().routing_mode == RoutingMode::kClosed) {
      return IPCZ_RESULT_NOT_FOUND;
    }

    // TODO: we should be able to return shared memory directly within the
    // destination portal's parcel queue to reduce copies. need to figure out
    // if/how to do this only when no OS handles will be transferred by the
    // corresponding CommitPut(). e.g. flags on BeginPut, or on portal creation
    // to restrict portals to data-only, or assign them dedicated shared memory
    // data queue storage?
  }

  pending_parcel_.emplace();
  pending_parcel_->ResizeData(num_data_bytes);
  if (data) {
    *data = pending_parcel_->data_view().data();
  }
  return IPCZ_RESULT_OK;
}

IpczResult Portal::CommitPut(uint32_t num_data_bytes_produced,
                             absl::Span<const IpczHandle> portals,
                             absl::Span<const IpczOSHandle> os_handles) {
  auto portals_to_send = AcquirePortalsForTransit(portals);
  if (!ValidatePortalsToSendFromHere(absl::MakeSpan(portals_to_send))) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  Parcel parcel;
  {
    // Note that this does not null out `pending_parcel_`, so that we can
    // release the mutex without other put operations being interposed before
    // this CommitPut() call completes.
    absl::MutexLock lock(&mutex_);
    if (!pending_parcel_) {
      return IPCZ_RESULT_FAILED_PRECONDITION;
    }

    if (num_data_bytes_produced > pending_parcel_->data_view().size()) {
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    parcel = std::move(*pending_parcel_);
    parcel.ResizeData(num_data_bytes_produced);
  }

  std::vector<os::Handle> acquired_os_handles =
      AcquireOSHandlesForTransit(os_handles);
  IpczResult result =
      PutImpl(parcel.data_view(), portals_to_send, acquired_os_handles, nullptr,
              /*is_two_phase_commit=*/true);
  if (result == IPCZ_RESULT_OK) {
    // Great job!
    ReleasePortalsAfterTransit(portals);

    absl::MutexLock lock(&mutex_);
    pending_parcel_.reset();
    return IPCZ_RESULT_OK;
  }

  ReleaseOSHandlesFromCancelledTransit(absl::MakeSpan(acquired_os_handles));

  absl::MutexLock lock(&mutex_);
  pending_parcel_ = std::move(parcel);
  return result;
}

IpczResult Portal::AbortPut() {
  PortalLock lock(*this);
  ABSL_ASSERT(!closed_);

  if (!pending_parcel_) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }
  pending_parcel_.reset();
  return IPCZ_RESULT_OK;
}

IpczResult Portal::Get(void* data,
                       uint32_t* num_data_bytes,
                       IpczHandle* portals,
                       uint32_t* num_portals,
                       IpczOSHandle* os_handles,
                       uint32_t* num_os_handles) {
  PortalLock lock(*this);
  if (in_two_phase_get_) {
    return IPCZ_RESULT_ALREADY_EXISTS;
  }

  if (status_.flags & IPCZ_PORTAL_STATUS_DEAD) {
    return IPCZ_RESULT_NOT_FOUND;
  }

  if (!incoming_parcels_.HasNextParcel()) {
    return IPCZ_RESULT_UNAVAILABLE;
  }

  Parcel& next_parcel = incoming_parcels_.NextParcel();
  IpczResult result = IPCZ_RESULT_OK;
  uint32_t available_data_storage = num_data_bytes ? *num_data_bytes : 0;
  uint32_t available_portal_storage = num_portals ? *num_portals : 0;
  uint32_t available_os_handle_storage = num_os_handles ? *num_os_handles : 0;
  if (next_parcel.data_view().size() > available_data_storage ||
      next_parcel.portals_view().size() > available_portal_storage ||
      next_parcel.os_handles_view().size() > available_os_handle_storage) {
    result = IPCZ_RESULT_RESOURCE_EXHAUSTED;
  }
  if (num_data_bytes) {
    *num_data_bytes = static_cast<uint32_t>(next_parcel.data_view().size());
  }
  if (num_portals) {
    *num_portals = static_cast<uint32_t>(next_parcel.portals_view().size());
  }
  if (num_os_handles) {
    *num_os_handles =
        static_cast<uint32_t>(next_parcel.os_handles_view().size());
  }
  if (result != IPCZ_RESULT_OK) {
    return result;
  }

  Parcel parcel;
  incoming_parcels_.Pop(parcel);
  status_.num_local_parcels -= 1;
  status_.num_local_bytes -= parcel.data_view().size();
  if ((status_.flags & IPCZ_PORTAL_STATUS_PEER_CLOSED) &&
      incoming_parcels_.IsDead()) {
    status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
  }
  memcpy(data, parcel.data_view().data(), parcel.data_view().size());
  parcel.Consume(portals, os_handles);

  // TODO: poke peer traps if peer is local, otherwise update shared state

  return IPCZ_RESULT_OK;
}

IpczResult Portal::BeginGet(const void** data,
                            uint32_t* num_data_bytes,
                            uint32_t* num_portals,
                            uint32_t* num_os_handles) {
  PortalLock lock(*this);
  if (in_two_phase_get_) {
    return IPCZ_RESULT_ALREADY_EXISTS;
  }

  if (status_.flags & IPCZ_PORTAL_STATUS_DEAD) {
    return IPCZ_RESULT_NOT_FOUND;
  }

  if (!incoming_parcels_.HasNextParcel()) {
    return IPCZ_RESULT_UNAVAILABLE;
  }

  Parcel& next_parcel = incoming_parcels_.NextParcel();
  const size_t data_size = next_parcel.data_view().size();
  if (num_data_bytes) {
    *num_data_bytes = static_cast<uint32_t>(data_size);
  }
  if (num_portals) {
    *num_portals = static_cast<uint32_t>(next_parcel.portals_view().size());
  }
  if (num_os_handles) {
    *num_os_handles =
        static_cast<uint32_t>(next_parcel.os_handles_view().size());
  }

  if (data_size > 0) {
    if (!data || !num_data_bytes) {
      return IPCZ_RESULT_RESOURCE_EXHAUSTED;
    }
    *data = next_parcel.data_view().data();
  }

  in_two_phase_get_ = true;
  return IPCZ_RESULT_OK;
}

IpczResult Portal::CommitGet(uint32_t num_data_bytes_consumed,
                             IpczHandle* portals,
                             uint32_t* num_portals,
                             IpczOSHandle* os_handles,
                             uint32_t* num_os_handles) {
  PortalLock lock(*this);
  if (!in_two_phase_get_) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  Parcel& next_parcel = incoming_parcels_.NextParcel();
  const size_t data_size = next_parcel.data_view().size();
  if (num_data_bytes_consumed > data_size) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  uint32_t available_portal_storage = num_portals ? *num_portals : 0;
  uint32_t available_os_handle_storage = num_os_handles ? *num_os_handles : 0;
  if (num_portals) {
    *num_portals = static_cast<uint32_t>(next_parcel.portals_view().size());
  }
  if (num_os_handles) {
    *num_os_handles =
        static_cast<uint32_t>(next_parcel.os_handles_view().size());
  }
  if (available_portal_storage < next_parcel.portals_view().size() ||
      available_os_handle_storage < next_parcel.os_handles_view().size()) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  }

  if (num_data_bytes_consumed == data_size) {
    Parcel parcel;
    incoming_parcels_.Pop(parcel);
    status_.num_local_parcels -= 1;
    status_.num_local_bytes -= parcel.data_view().size();
    if ((status_.flags & IPCZ_PORTAL_STATUS_PEER_CLOSED) &&
        incoming_parcels_.IsDead()) {
      status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
    }
    parcel.Consume(portals, os_handles);
  } else {
    Parcel& parcel = incoming_parcels_.NextParcel();
    status_.num_local_bytes -= num_data_bytes_consumed;
    parcel.ConsumePartial(num_data_bytes_consumed, portals, os_handles);
  }
  in_two_phase_get_ = false;

  // TODO: poke peer traps if peer is local, otherwise update shared state

  return IPCZ_RESULT_OK;
}

IpczResult Portal::AbortGet() {
  absl::MutexLock lock(&mutex_);
  if (!in_two_phase_get_) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  in_two_phase_get_ = false;
  return IPCZ_RESULT_OK;
}

IpczResult Portal::CreateTrap(const IpczTrapConditions& conditions,
                              IpczTrapEventHandler handler,
                              uintptr_t context,
                              IpczHandle& trap) {
  auto new_trap = std::make_unique<Trap>(conditions, handler, context);
  trap = ToHandle(new_trap.get());

  absl::MutexLock lock(&mutex_);
  return traps_.Add(std::move(new_trap));
}

IpczResult Portal::ArmTrap(IpczHandle trap,
                           IpczTrapConditionFlags* satisfied_condition_flags,
                           IpczPortalStatus* status) {
  IpczTrapConditionFlags flags = 0;
  PortalLock lock(*this);
  IpczResult result = ToRef<Trap>(trap).Arm(status_, flags);
  if (result == IPCZ_RESULT_OK) {
    return IPCZ_RESULT_OK;
  }

  if (satisfied_condition_flags) {
    *satisfied_condition_flags = flags;
  }

  if (status) {
    size_t out_size = status->size;
    size_t copy_size = std::min(out_size, sizeof(status_));
    memcpy(status, &status_, copy_size);
    status->size = static_cast<uint32_t>(out_size);
  }

  return result;
}

IpczResult Portal::DestroyTrap(IpczHandle trap) {
  absl::MutexLock lock(&mutex_);
  return traps_.Remove(ToRef<Trap>(trap));
}

bool Portal::ValidatePortalsToSendFromHere(
    absl::Span<mem::Ref<Portal>> portals) {
  for (const mem::Ref<Portal>& portal : portals) {
    if (portal.get() == this) {
      return false;
    }

    PortalLock lock(*this);
    if (local_peer_ == portal) {
      return false;
    }
  }

  return true;
}

bool Portal::Deserialize(const mem::Ref<NodeLink>& from_node,
                         os::Memory::Mapping link_state_mapping,
                         const PortalDescriptor& descriptor) {
  {
    absl::MutexLock lock(&mutex_);
    incoming_parcels_ =
        IncomingParcelQueue(descriptor.next_incoming_sequence_number);
    next_outgoing_sequence_number_ = descriptor.next_outgoing_sequence_number;
    if (descriptor.peer_closed) {
      status_.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
      incoming_parcels_.SetPeerSequenceLength(descriptor.peer_sequence_length);
      if (incoming_parcels_.IsDead()) {
        status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
      }
    }
  }

  auto new_link = mem::MakeRefCounted<PortalLink>(
      from_node, descriptor.routing_id, std::move(link_state_mapping));
  if (descriptor.route_is_peer) {
    ActivateFromBuffering(std::move(new_link));
    return true;
  }

  {
    absl::MutexLock lock(&mutex_);
    predecessor_link_ = std::move(new_link);
  }

  // If the sent portal was able to immediately enter half-proxying mode, it
  // will provide us with the information we need to talk to its peer. This
  // avoids the need for it to send us an extra message just to initiate proxy
  // bypass.
  if (descriptor.peer_name.is_valid()) {
    const NodeName proxy_name = *from_node->GetRemoteName();
    return InitiateProxyBypass(descriptor.peer_name, descriptor.peer_routing_id,
                               descriptor.peer_key,
                               /*notify_predecessor=*/false);
  }

  // For now we'll forward all parcels to our predecessor.
  routing_mode_ = RoutingMode::kActive;
  return true;
}

IpczResult Portal::ValidatePutLimits(size_t data_size,
                                     const IpczPutLimits* limits) {
  mutex_.AssertHeld();

  uint32_t max_queued_parcels = std::numeric_limits<uint32_t>::max();
  uint32_t max_queued_bytes = std::numeric_limits<uint32_t>::max();
  if (limits) {
    if (limits->max_queued_parcels > 0) {
      max_queued_parcels = limits->max_queued_parcels;
    } else {
      max_queued_bytes = limits->max_queued_bytes;
    }
  }

  if (local_peer_) {
    local_peer_->mutex_.AssertHeld();
    if (local_peer_->status_.num_local_parcels >= max_queued_parcels) {
      return IPCZ_RESULT_RESOURCE_EXHAUSTED;
    }
    uint32_t queued_bytes = local_peer_->status_.num_local_bytes;
    if (queued_bytes >= max_queued_bytes) {
      return IPCZ_RESULT_RESOURCE_EXHAUSTED;
    }

    // Note that this can't underflow, per the above branch.
    uint32_t queue_byte_capacity = max_queued_bytes - queued_bytes;
    if (data_size > queue_byte_capacity) {
      return IPCZ_RESULT_RESOURCE_EXHAUSTED;
    }

    return IPCZ_RESULT_OK;
  }

  if (peer_link_) {
    // TODO: Use the shared link state to test peer's last read sequence # vs
    // our our next outgoing sequence # (also a TODO).
    // For now ignore limits.
    return IPCZ_RESULT_OK;
  }

  // TODO: when buffering we will need some idea about capacity on the eventual
  // receiving end.
  return IPCZ_RESULT_OK;
}

IpczResult Portal::PutImpl(absl::Span<const uint8_t> data,
                           Parcel::PortalVector& portals,
                           std::vector<os::Handle>& os_handles,
                           const IpczPutLimits* limits,
                           bool is_two_phase_commit) {
  mem::Ref<PortalLink> receiving_link;
  SequenceNumber sequence_number;
  {
    PortalLock lock(*this);
    if (pending_parcel_ && !is_two_phase_commit) {
      return IPCZ_RESULT_ALREADY_EXISTS;
    }

    if (limits) {
      IpczResult result = ValidatePutLimits(data.size(), limits);
      if (result != IPCZ_RESULT_OK) {
        return result;
      }
    }

    sequence_number = next_outgoing_sequence_number_++;
    if (local_peer_) {
      if (local_peer_->closed_) {
        return IPCZ_RESULT_NOT_FOUND;
      }

      Parcel parcel(sequence_number);
      parcel.SetData(std::vector<uint8_t>(data.begin(), data.end()));
      parcel.SetPortals(std::move(portals));
      parcel.SetOSHandles(std::move(os_handles));
      local_peer_->incoming_parcels_.Push(std::move(parcel));
      local_peer_->status_.num_local_parcels += 1;
      local_peer_->status_.num_local_bytes += data.size();

      // TODO: poke peer's traps

      return IPCZ_RESULT_OK;
    }

    if (routing_mode_ == RoutingMode::kActive) {
      receiving_link = peer_link_ ? peer_link_ : predecessor_link_;
    } else {
      ABSL_ASSERT(routing_mode_ == RoutingMode::kBuffering);
      Parcel parcel(sequence_number);
      parcel.SetData(std::vector<uint8_t>(data.begin(), data.end()));
      parcel.SetPortals(std::move(portals));
      parcel.SetOSHandles(std::move(os_handles));
      outgoing_parcels_.push(std::move(parcel));
      return IPCZ_RESULT_OK;
    }
  }

  ABSL_ASSERT(receiving_link);
  Parcel parcel(sequence_number);
  parcel.SetData(std::vector<uint8_t>(data.begin(), data.end()));
  parcel.SetPortals(std::move(portals));
  parcel.SetOSHandles(std::move(os_handles));
  receiving_link->SendParcel(parcel);
  return IPCZ_RESULT_OK;
}

void Portal::ForwardParcels() {
  mem::Ref<PortalLink> peer_or_predecessor;
  mem::Ref<PortalLink> successor;
  std::vector<Parcel> parcels_to_successor;
  std::vector<Parcel> parcels_to_peer_or_predecessor;
  bool dead;
  {
    absl::MutexLock lock(&mutex_);
    successor = successor_link_;
    peer_or_predecessor = peer_link_ ? peer_link_ : predecessor_link_;

    Parcel parcel;
    while (incoming_parcels_.Pop(parcel)) {
      parcels_to_successor.push_back(std::move(parcel));
    }
    if (outgoing_parcels_from_successor_) {
      while (outgoing_parcels_from_successor_->Pop(parcel)) {
        parcels_to_peer_or_predecessor.push_back(std::move(parcel));
      }
    }

    // Will be true iff all of our forwarding responsibilities have been
    // fulfilled.
    dead = (!successor || incoming_parcels_.IsDead()) &&
           (!outgoing_parcels_from_successor_ ||
            outgoing_parcels_from_successor_->IsDead());
  }

  if (successor) {
    for (Parcel& parcel : parcels_to_successor) {
      successor->SendParcel(parcel);
    }
  }

  if (peer_or_predecessor) {
    for (Parcel& parcel : parcels_to_peer_or_predecessor) {
      peer_or_predecessor->SendParcel(parcel);
    }
  }

  if (dead) {
    // Ensure we have no links routing to us. As the stack unwinds, any last
    // references to this portal should be released and it should be destroyed.
    absl::MutexLock lock(&mutex_);
    if (successor_link_) {
      successor_link_->Disconnect();
    }
    if (peer_link_) {
      peer_link_->Disconnect();
    }
    if (predecessor_link_) {
      predecessor_link_->Disconnect();
    }
  }
}

Portal::PortalLock::PortalLock(Portal& portal) : portal_(portal) {
  portal_.mutex_.Lock();
  while (portal_.local_peer_) {
    locked_peer_ = portal_.local_peer_;
    if (locked_peer_.get() < &portal_) {
      portal_.mutex_.Unlock();
      locked_peer_->mutex_.Lock();
      portal_.mutex_.Lock();

      // Small chance the peer changed since we unlocked our lock and acquired
      // the peer's lock before reacquiring ours. In that case, unlock their
      // lock and try again with the new peer.
      if (portal_.local_peer_ != locked_peer_) {
        locked_peer_->mutex_.Unlock();
        locked_peer_ = portal_.local_peer_;
        continue;
      }
    } else {
      locked_peer_->mutex_.Lock();
    }

    return;
  }
}

Portal::PortalLock::~PortalLock() {
  if (locked_peer_) {
    locked_peer_->mutex_.Unlock();
  }
  portal_.mutex_.Unlock();
}

Portal::TwoPortalLock::TwoPortalLock(Portal& a, Portal& b) : a_(a), b_(b) {
  if (&a < &b) {
    a.mutex_.Lock();
    b.mutex_.Lock();
  } else {
    b.mutex_.Lock();
    a.mutex_.Lock();
  }
}

Portal::TwoPortalLock::~TwoPortalLock() {
  a_.mutex_.Unlock();
  b_.mutex_.Unlock();
}

}  // namespace core
}  // namespace ipcz
