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

}  // namespace

Portal::Portal(Side side) : Portal(side, /*transferrable=*/true) {}

Portal::Portal(Side side, decltype(kNonTransferrable))
    : Portal(side, /*transferrable=*/false) {}

Portal::Portal(Side side, bool transferrable)
    : side_(side), transferrable_(transferrable) {}

Portal::~Portal() = default;

// static
Portal::Pair Portal::CreateLocalPair(Node& node) {
  auto left = mem::MakeRefCounted<Portal>(Side::kLeft);
  auto right = mem::MakeRefCounted<Portal>(Side::kRight);
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
  //   leave each local portal with nothing but a successor link to the new
  //   portal - use this to forward unread incoming parcels. use a one-off
  //   field in PortalDescriptor to say "peer is this other descriptor X" for
  //   the receiving end.

  PortalLock lock(*this);
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

  if (peer_link_) {
    bool is_half_proxying = false;
    SequenceNumber sequence_length;
    {
      PortalLinkState::Locked state(peer_link_->state(), side_);
      PortalLinkState::SideState& this_side = state.this_side();
      PortalLinkState::SideState& other_side = state.other_side();
      sequence_length = other_side.sequence_length;
      switch (other_side.routing_mode) {
        case RoutingMode::kClosed:
          is_half_proxying = true;
          this_side.routing_mode = RoutingMode::kHalfProxy;
          descriptor.peer_closed = true;
          break;
        case RoutingMode::kActive:
        case RoutingMode::kFullProxy:
          is_half_proxying = true;
          this_side.routing_mode = RoutingMode::kHalfProxy;
          break;
        case RoutingMode::kBuffering:
        case RoutingMode::kHalfProxy:
          this_side.routing_mode = RoutingMode::kFullProxy;
          break;
      }
    }

    if (is_half_proxying) {
      const absl::uint128 key = RandomUint128();
      descriptor.peer_name = *peer_link_->node().GetRemoteName();
      descriptor.peer_route = peer_link_->route();
      descriptor.peer_key = key;

      PortalLinkState::Locked state(peer_link_->state(), side_);
      state.this_side().successor_key = key;
    }

    descriptor.side = side_;
    descriptor.peer_sequence_length = sequence_length;
    descriptor.next_incoming_sequence_number =
        incoming_parcels_.current_sequence_number();
    descriptor.next_outgoing_sequence_number = next_outgoing_sequence_number_;

    incoming_parcels_.SetPeerSequenceLength(sequence_length);
  }

  return mem::WrapRefCounted(this);
}

// static
mem::Ref<Portal> Portal::DeserializeNew(NodeLink& from_node,
                                        os::Memory::Mapping link_state_mapping,
                                        const PortalDescriptor& descriptor) {
  auto portal = mem::MakeRefCounted<Portal>(descriptor.side);
  if (!portal->Deserialize(from_node, std::move(link_state_mapping),
                           descriptor)) {
    return nullptr;
  }
  return portal;
}

void Portal::SetPeerLink(mem::Ref<PortalLink> link) {
  absl::optional<SequenceNumber> sequence_length;
  std::forward_list<Parcel> parcels_to_forward;
  {
    PortalLock lock(*this);
    ABSL_ASSERT(!local_peer_);
    ABSL_ASSERT(!peer_link_);

    // TODO: we may need to plumb a TrapEventDispatcher here and into
    // SendParcel() in case something goes wrong and we end up discarding (and
    // closing) portal attachments locally.
    peer_link_ = link;
    parcels_to_forward = outgoing_parcels_.TakeParcels();
    if (closed_) {
      sequence_length = next_outgoing_sequence_number_;
    }
  }

  for (Parcel& parcel : parcels_to_forward) {
    link->SendParcel(parcel);
  }

  if (sequence_length) {
    link->NotifyClosed(*sequence_length);
  }
}

void Portal::SetSuccessorLink(mem::Ref<PortalLink> link) {
  PortalLock lock(*this);
  ABSL_ASSERT(!successor_link_);
  successor_link_ = std::move(link);

  Parcel parcel;
  while (incoming_parcels_.Pop(parcel)) {
    successor_link_->SendParcel(parcel);
  }

  if (incoming_parcels_.IsDead()) {
    // TODO: nothing else to do, go away?
  }
}

bool Portal::AcceptParcelFromLink(Parcel& parcel,
                                  TrapEventDispatcher& dispatcher) {
  absl::MutexLock lock(&mutex_);
  if (!incoming_parcels_.Push(std::move(parcel))) {
    return false;
  }
  status_.num_local_bytes = incoming_parcels_.GetNumAvailableBytes();
  status_.num_local_parcels = incoming_parcels_.GetNumAvailableParcels();
  traps_.MaybeNotify(dispatcher, status_);
  return true;
}

bool Portal::NotifyPeerClosed(SequenceNumber sequence_length,
                              TrapEventDispatcher& dispatcher) {
  absl::MutexLock lock(&mutex_);
  status_.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
  incoming_parcels_.SetPeerSequenceLength(sequence_length);
  if (incoming_parcels_.IsDead()) {
    status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
  }

  // TODO: Need to clear `outgoing_parcels_` here and manually close all Portals
  // attached to any outgoing parcels. This in turn will need a
  // TrapEventDispatcher to be plumbed through.
  outgoing_parcels_.clear();

  traps_.MaybeNotify(dispatcher, status_);
  return true;
}

IpczResult Portal::Close() {
  // TODO: Plumb a TrapEventDispatcher to Close() so it can queue events.
  std::vector<mem::Ref<Portal>> other_portals_to_close;
  {
    PortalLock lock(*this);
    ABSL_ASSERT(!closed_);

    // Forwarding portals must not be closed. They will clean themselves up.
    // TODO: proxying hops should be modeled as a fundamentally different type
    // from portals.
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
      {
        PortalLinkState::Locked state(peer_link_->state(), side_);
        state.this_side().routing_mode = RoutingMode::kClosed;
        state.this_side().sequence_length = next_outgoing_sequence_number_;
      }

      // We should never have outgoing parcels buffered while we have a peer
      // link.
      ABSL_ASSERT(outgoing_parcels_.empty());

      // TODO: Just signal?
      peer_link_->NotifyClosed(next_outgoing_sequence_number_);
    }
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

bool Portal::Deserialize(NodeLink& from_node,
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

  SetPeerLink(mem::MakeRefCounted<PortalLink>(mem::WrapRefCounted(&from_node),
                                              descriptor.route,
                                              std::move(link_state_mapping)));
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
  mem::Ref<PortalLink> link;
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

    if (peer_link_) {
      link = peer_link_;
    }

    if (!link) {
      Parcel parcel(sequence_number);
      parcel.SetData(std::vector<uint8_t>(data.begin(), data.end()));
      parcel.SetPortals(std::move(portals));
      parcel.SetOSHandles(std::move(os_handles));
      outgoing_parcels_.push(std::move(parcel));
      return IPCZ_RESULT_OK;
    }

    if (link) {
      // TODO: Update shared state to either lock in the expectation of this
      // parcel arriving or confirm that no more parcels will be accepted. In
      // the latter case, we'll queue the parcel in `outgoing_parcels_` and wait
      // to hear back from the peer about where to send parcels next.
    }
  }

  ABSL_ASSERT(link);
  Parcel parcel(sequence_number);
  parcel.SetData(std::vector<uint8_t>(data.begin(), data.end()));
  parcel.SetPortals(std::move(portals));
  parcel.SetOSHandles(std::move(os_handles));
  link->SendParcel(parcel);
  return IPCZ_RESULT_OK;
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
