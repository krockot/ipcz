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
#include "core/portal_control_block.h"
#include "core/trap.h"
#include "debug/log.h"
#include "mem/ref_counted.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/handle_util.h"

namespace ipcz {
namespace core {

namespace {

Parcel::PortalVector AcquirePortalsForTransit(
    absl::Span<const IpczHandle> handles) {
  Parcel::PortalVector portals(handles.size());
  for (size_t i = 0; i < handles.size(); ++i) {
    portals[i].portal = mem::WrapRefCounted(ToPtr<Portal>(handles[i]));
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

void Portal::SetPeerLink(mem::Ref<NodeLink> link,
                         RouteId route,
                         os::Memory::Mapping control_block_mapping) {
  PortalLock lock(*this);
  ABSL_ASSERT(!local_peer_);
  ABSL_ASSERT(!peer_link_);

  bool remote_portal_closed = false;
  {
    PortalControlBlock::Locked control(control_block_mapping, side_);
    PortalControlBlock::QueueState& our_queue_state =
        control.this_side().queue_state;
    if (closed_) {
      // Inform the other side ASAP that we're closed.
      control.this_side().status = PortalControlBlock::Status::kClosed;
    }
    // TODO: extract incoming parcel expectations from control block? if the
    // remote end has outgoing messages in flight for us,
    switch (control.other_side().status) {
      case PortalControlBlock::Status::kReady:
        // Ensure that a ready peer's node will retain necessary state long
        // enough to forward the parcels we're about to flush to it, even if
        // the destination portal moves before those parcels arrive.
        our_queue_state.num_sent_parcels += status_.num_remote_parcels;
        our_queue_state.num_sent_bytes += status_.num_remote_bytes;
        break;

      case PortalControlBlock::Status::kClosed:
        remote_portal_closed = true;
        break;

      case PortalControlBlock::Status::kMoved:
        // Don't set the peer link since the peer has already moved.
        // TODO: extract a NodeName and route key from shared state so we can
        // get ourselves hooked up to the new peer location.
        LOG(ERROR) << "Peer relocation not yet implemented!";
        return;
    }
  }

  if (remote_portal_closed) {
    // No reason to keep track of this link state if the peer is already
    // closed since anything we send that way is going to be ignored.
    //
    // TODO: poke traps
    status_.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
    return;
  }

  // The peer was around at least long enough for us to update its queue state,
  // so it knows to expect any outgoing messages we send below.
  //
  // TODO: we may need to plumb a TrapEventDispatcher here and into SendParcel()
  // in case something goes wrong and we end up discarding (and closing) portal
  // attachments locally.
  peer_link_ = std::move(link);
  peer_route_ = route;
  peer_control_block_ = std::move(control_block_mapping);
  for (Parcel& parcel : outgoing_parcels_.TakeParcels()) {
    peer_link_->SendParcel(peer_route_, parcel);
  }
}

void Portal::SetForwardingLink(mem::Ref<NodeLink> link,
                               RouteId route,
                               os::Memory::Mapping control_block_mapping) {
  // TODO: actually do the right things here
  PortalLock lock(*this);
  forwarding_link_ = std::move(link);
  forwarding_route_ = route;
  forwarding_control_block_ = std::move(control_block_mapping);
}

bool Portal::AcceptParcelFromLink(Parcel& parcel,
                                  TrapEventDispatcher& dispatcher) {
  PortalLock lock(*this);
  status_.num_local_bytes += parcel.data_view().size();
  status_.num_local_parcels += 1;
  incoming_parcels_.push(std::move(parcel));
  traps_.MaybeNotify(dispatcher, status_);
  return true;
}

bool Portal::NotifyPeerClosed(TrapEventDispatcher& dispatcher) {
  absl::MutexLock lock(&mutex_);
  status_.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;

  // TODO: Need to clear `outgoing_parcels_` here and manually close all Portals
  // attached to any outgoing parcels. This in turn will need a
  // TrapEventDispatcher to be plumbed through.
  outgoing_parcels_.clear();
  return true;
}

IpczResult Portal::Close() {
  // TODO: Plumb a TrapEventDispatcher to Close() so it can queue events.
  std::vector<mem::Ref<Portal>> other_portals_to_close;
  {
    PortalLock lock(*this);
    ABSL_ASSERT(!closed_);

    // Forwarding portals must not be closed. They will clean themselves up.
    ABSL_ASSERT(!forwarding_link_);
    closed_ = true;

    if (local_peer_) {
      // Fast path: our peer is local so we can update its status directly.
      //
      // TODO: it's possible that we are still waiting for incoming messages
      // we know to be in flight. Ensure that our local peer knows this so it
      // doesn't appear dead just because peer closure is flagged.
      local_peer_->status_.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;

      // TODO: poke peer's traps
      return IPCZ_RESULT_OK;
    }

    // Signal our closure ASAP via the control block to reduce the potential for
    // redundant work on the peer's end.
    if (peer_link_) {
      {
        PortalControlBlock::Locked control(peer_control_block_, side_);
        control.this_side().status = PortalControlBlock::Status::kClosed;
      }
      peer_link_->SendPeerClosed(peer_route_);
    }

    for (Parcel& parcel : outgoing_parcels_.TakeParcels()) {
      for (PortalInTransit& portal : parcel.TakePortals()) {
        other_portals_to_close.push_back(std::move(portal.portal));
      }
    }
  }

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
  auto portals_in_transit = AcquirePortalsForTransit(portals);
  auto portals_view = absl::MakeSpan(portals_in_transit);
  if (!ValidatePortalsForTransitFromHere(portals_view)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  std::vector<os::Handle> acquired_os_handles =
      AcquireOSHandlesForTransit(os_handles);
  IpczResult result =
      PutImpl(data, portals_in_transit, acquired_os_handles, limits,
              /*is_two_phase_commit=*/false);
  if (result == IPCZ_RESULT_OK) {
    // Great job!
    FinalizePortalsAfterTransit(portals_view);
    return IPCZ_RESULT_OK;
  }

  RestorePortalsFromCancelledTransit(portals_view);
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
    PortalControlBlock::Locked control(peer_control_block_, side_);
    if (control.other_side().status == PortalControlBlock::Status::kClosed) {
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
  auto portals_in_transit = AcquirePortalsForTransit(portals);
  auto portals_view = absl::MakeSpan(portals_in_transit);
  if (!ValidatePortalsForTransitFromHere(portals_view)) {
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
  IpczResult result = PutImpl(parcel.data_view(), portals_in_transit,
                              acquired_os_handles, nullptr,
                              /*is_two_phase_commit=*/true);
  if (result == IPCZ_RESULT_OK) {
    // Great job!
    FinalizePortalsAfterTransit(portals_view);

    absl::MutexLock lock(&mutex_);
    pending_parcel_.reset();
    return IPCZ_RESULT_OK;
  }

  RestorePortalsFromCancelledTransit(portals_view);
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

  if (incoming_parcels_.empty()) {
    if (status_.flags & IPCZ_PORTAL_STATUS_PEER_CLOSED) {
      return IPCZ_RESULT_NOT_FOUND;
    }
    return IPCZ_RESULT_UNAVAILABLE;
  }

  Parcel& next_parcel = incoming_parcels_.front();
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

  Parcel parcel = incoming_parcels_.pop();
  status_.num_local_parcels -= 1;
  status_.num_local_bytes -= parcel.data_view().size();
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

  if (incoming_parcels_.empty()) {
    if (status_.flags & IPCZ_PORTAL_STATUS_PEER_CLOSED) {
      return IPCZ_RESULT_NOT_FOUND;
    }
    return IPCZ_RESULT_UNAVAILABLE;
  }

  Parcel& next_parcel = incoming_parcels_.front();
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

  Parcel& next_parcel = incoming_parcels_.front();
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
    Parcel parcel = incoming_parcels_.pop();
    status_.num_local_parcels -= 1;
    status_.num_local_bytes -= parcel.data_view().size();
    parcel.Consume(portals, os_handles);
  } else {
    Parcel& parcel = incoming_parcels_.front();
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

bool Portal::ValidatePortalsForTransitFromHere(
    absl::Span<PortalInTransit> portals_in_transit) {
  for (PortalInTransit& portal_in_transit : portals_in_transit) {
    Portal& portal = *portal_in_transit.portal;
    if (!portal.transferrable_ || &portal == this) {
      return false;
    }

    PortalLock lock(*this);
    if (local_peer_.get() == &portal) {
      return false;
    }
  }

  return true;
}

// static
void Portal::PreparePortalsForTransit(
    absl::Span<PortalInTransit> portals_in_transit) {
  for (PortalInTransit& portal : portals_in_transit) {
    ABSL_ASSERT(portal.portal);
    portal.portal->PrepareForTransit(portal);
  }
}

// static
void Portal::RestorePortalsFromCancelledTransit(
    absl::Span<PortalInTransit> portals_in_transit) {
  for (PortalInTransit& portal : portals_in_transit) {
    ABSL_ASSERT(portal.portal);
    portal.portal->RestoreFromCancelledTransit(portal);
  }
}

// static
void Portal::FinalizePortalsAfterTransit(absl::Span<PortalInTransit> portals) {
  for (PortalInTransit& portal_in_transit : portals) {
    // Steal the handle's ref to the portal since the handle must no longer be
    // in use by the application.
    mem::Ref<Portal> portal = {mem::RefCounted::kAdoptExistingRef,
                               portal_in_transit.portal.get()};
    portal->FinalizeAfterTransit(portal_in_transit);
  }
}

void Portal::PrepareForTransit(PortalInTransit& portal_in_transit) {
  portal_in_transit.side = side_;

  PortalLock lock(*this);
  moved_ = true;

  if (local_peer_) {
    // We are part of a local portal pair that now must be split up and left to
    // buffer until transit is complete.

    local_peer_->local_peer_.reset();
    portal_in_transit.local_peer_before_transit = std::move(local_peer_);
    return;
  }

  if (peer_link_) {
    // TODO: more shared state mgmt
    PortalControlBlock::Locked control(peer_control_block_, side_);
    control.this_side().status = PortalControlBlock::Status::kMoved;
  }
}

void Portal::RestoreFromCancelledTransit(PortalInTransit& portal_in_transit) {
  // TODO - not terribly important for now. mojo always discards resources that
  // were attached to messages which couldn't be sent. so in practice any
  // cancelled transit will immediately be followed by closure of all portal
  // attachments and it doesn't matter what state we leave them in. this needs
  // to be fixed eventually to meet specified ipcz API behvior though. for
  // example if a direct portal pair was spit for transit and then transit
  // failed, we should restore the direct portal pair to its original state
  // here.
}

void Portal::FinalizeAfterTransit(PortalInTransit& portal_in_transit) {
  // TODO: set up forwarding link to destination node, lock in shared state for
  // peer to stop sending us messages and find the new destination.
  //
  // `portal_in_transit` specifies the `route` for the new portal, if it was
  // transmitted over a NodeLink.
  if (portal_in_transit.route) {
    // TODO: set up `peer_link_`.
    LOG(ERROR) << "need to set up peer_link_!";
  }
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
    if (local_peer_->incoming_parcels_.size() >= max_queued_parcels) {
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
    // TODO: Use the control block to test peer's last read sequence # vs our
    // our next outgoing sequence # (also a TODO).
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
  mem::Ref<NodeLink> peer_link;
  RouteId peer_route;
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

    if (local_peer_) {
      if (local_peer_->closed_) {
        return IPCZ_RESULT_NOT_FOUND;
      }

      Parcel parcel;
      parcel.SetData(std::vector<uint8_t>(data.begin(), data.end()));
      parcel.SetPortals(std::move(portals));
      parcel.SetOSHandles(std::move(os_handles));
      local_peer_->incoming_parcels_.push(std::move(parcel));
      local_peer_->status_.num_local_parcels += 1;
      local_peer_->status_.num_local_bytes += data.size();

      // TODO: poke peer's traps

      return IPCZ_RESULT_OK;
    }

    if (peer_link_) {
      bool peer_moved = false;
      PortalControlBlock::Status peer_status;
      {
        PortalControlBlock::Locked control(peer_control_block_, side_);
        PortalControlBlock::QueueState& queue_state =
            control.this_side().queue_state;
        peer_status = control.other_side().status;
        if (peer_status == PortalControlBlock::Status::kReady) {
          queue_state.num_sent_parcels += 1;
        } else if (peer_status == PortalControlBlock::Status::kClosed) {
          return IPCZ_RESULT_NOT_FOUND;
        } else if (peer_status == PortalControlBlock::Status::kMoved) {
          peer_moved = true;
        }
      }

      if (!peer_moved) {
        peer_link = peer_link_;
        peer_route = peer_route_;
      }
    }
  }

  Parcel parcel;
  parcel.SetData(std::vector<uint8_t>(data.begin(), data.end()));
  parcel.SetPortals(std::move(portals));
  parcel.SetOSHandles(std::move(os_handles));

  if (peer_link) {
    PreparePortalsForTransit(parcel.portals_view());
    peer_link->SendParcel(peer_route, parcel);
    return IPCZ_RESULT_OK;
  }

  // No peer link, so queue for later transmission.
  {
    PortalLock lock(*this);
    outgoing_parcels_.push(std::move(parcel));
  }

  return IPCZ_RESULT_OK;
}

Portal::PortalLock::PortalLock(Portal& portal) : portal_(portal) {
  portal_.mutex_.Lock();
  locked_peer_ = portal_.local_peer_;
  while (locked_peer_) {
    if (locked_peer_.get() < &portal_) {
      portal_.mutex_.Unlock();
      locked_peer_->mutex_.Lock();
      portal_.mutex_.Lock();

      // Small chance the peer changed since we unlocked our lock and acquired
      // its lock first before reacquiring ours. In that case, unlock their lock
      // and try again with the new peer.
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

}  // namespace core
}  // namespace ipcz
