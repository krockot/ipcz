// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/routed_portal_backend.h"

#include <algorithm>
#include <utility>

#include "core/buffering_portal_backend.h"
#include "core/node_link.h"
#include "core/parcel.h"
#include "core/parcel_queue.h"
#include "core/portal_backend_state.h"
#include "core/portal_control_block.h"
#include "core/trap.h"
#include "core/trap_event_dispatcher.h"
#include "debug/log.h"
#include "util/handle_util.h"

namespace ipcz {
namespace core {

RoutedPortalBackend::RoutedPortalBackend(
    const PortalName& name,
    mem::Ref<NodeLink> link,
    const PortalName& remote_portal,
    Side side,
    os::Memory::Mapping control_block_mapping)
    : name_(name),
      link_(std::move(link)),
      remote_portal_(remote_portal),
      side_(side),
      control_block_mapping_(std::move(control_block_mapping)) {}

RoutedPortalBackend::~RoutedPortalBackend() = default;

bool RoutedPortalBackend::AdoptBufferingBackendState(
    BufferingPortalBackend& backend) {
  absl::MutexLock our_lock(&mutex_);
  absl::MutexLock their_lock(&backend.mutex_);
  bool remote_portal_closed = false;
  {
    PortalControlBlock::Locked control(control_block_);
    // TODO: extract incoming parcel expectations from control block? if the
    // remote end has outgoing messages in flight for us,
    switch (control.opposite(side_).status) {
      case PortalControlBlock::Status::kReady:
        // Ensure that a ready peer's node will retain necessary state long
        // enough to forward the parcels we're about to flush to it, even if
        // the destination portal moves before those parcels arrive.
        control.side(side_).queue_state.num_sent_parcels +=
            backend.state_.status.num_remote_parcels;
        control.side(side_).queue_state.num_sent_bytes +=
            backend.state_.status.num_remote_bytes;
        break;

      case PortalControlBlock::Status::kClosed:
        remote_portal_closed = true;
        return true;

      case PortalControlBlock::Status::kMoving:
        // Cancel adoption to leave this portal in a buffering state.
        return false;
    }
  }

  state_ = std::move(backend.state_);
  if (remote_portal_closed) {
    return true;
  }

  for (Parcel& parcel : state_.outgoing_parcels.TakeParcels()) {
    link_->SendParcel(remote_portal_, parcel);
  }
  return true;
}

std::unique_ptr<BufferingPortalBackend> RoutedPortalBackend::StartBuffering() {
  auto new_backend = std::make_unique<BufferingPortalBackend>(side_);
  absl::MutexLock our_lock(&mutex_);
  absl::MutexLock their_lock(&new_backend->mutex_);

  PortalControlBlock::QueueState our_queue_state;
  PortalControlBlock::QueueState their_queue_state;
  {
    // Block any further control block updates from the peer and grab a copy of
    // the last QueueStates.
    PortalControlBlock::Locked control(control_block_);
    PortalControlBlock::SideState& side = control.side(side_);
    side.status = PortalControlBlock::Status::kMoving;
    our_queue_state = side.queue_state;
    their_queue_state = control.opposite(side_).queue_state;
  }

  // TODO: configure the buffering backend to expect any messages which were
  // already routed our way but haven't arrived yet. it must stick around to
  // forward them to our destination once that's known.

  new_backend->routed_name_ = name_;

  return new_backend;
}

PortalBackend::Type RoutedPortalBackend::GetType() const {
  return Type::kRouted;
}

bool RoutedPortalBackend::CanTravelThroughPortal(Portal& sender) {
  return true;
}

void RoutedPortalBackend::PrepareForTravel(PortalInTransit& portal_in_transit) {
  // Should not be reached. We only call PrepareForTravel() on buffering
  // backends now.
  // TODO: remove from general PortalBackend interface?
  ABSL_ASSERT(false);
}

bool RoutedPortalBackend::AcceptParcel(Parcel& parcel,
                                       TrapEventDispatcher& dispatcher) {
  absl::MutexLock lock(&mutex_);
  state_.incoming_parcels.push(std::move(parcel));
  state_.status.num_local_bytes += parcel.data_view().size();
  ++state_.status.num_local_parcels;
  state_.traps.MaybeNotify(dispatcher, state_.status);
  return true;
}

bool RoutedPortalBackend::NotifyPeerClosed(TrapEventDispatcher& dispatcher) {
  absl::MutexLock lock(&mutex_);
  if (state_.status.flags & IPCZ_PORTAL_STATUS_PEER_CLOSED) {
    return true;
  }

  state_.status.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
  state_.traps.MaybeNotify(dispatcher, state_.status);
  return true;
}

IpczResult RoutedPortalBackend::Close(
    std::vector<mem::Ref<Portal>>& other_portals_to_close) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(!state_.closed);
  state_.closed = true;

  {
    PortalControlBlock::Locked control(control_block_);
    switch (control.opposite(side_).status) {
      case PortalControlBlock::Status::kReady:
        control.side(side_).status = PortalControlBlock::Status::kClosed;
        break;
      case PortalControlBlock::Status::kClosed:
        // Nothing to do if the peer is already closed too.
        return IPCZ_RESULT_OK;
      case PortalControlBlock::Status::kMoving:
        // TODO: switch to a buffering state in this case
        break;
    }
  }

  // TODO: this can be replaced with a coalescable event signal to the peer node
  // since we've updated the control block to reflect our closed state.
  link_->SendPeerClosed(remote_portal_);
  return IPCZ_RESULT_OK;
}

IpczResult RoutedPortalBackend::QueryStatus(IpczPortalStatus& status) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(!state_.closed);
  status = state_.status;
  return IPCZ_RESULT_OK;
}

IpczResult RoutedPortalBackend::Put(absl::Span<const uint8_t> data,
                                    absl::Span<PortalInTransit> portals,
                                    absl::Span<const IpczOSHandle> os_handles,
                                    const IpczPutLimits* limits) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(!state_.closed);
  if (state_.pending_parcel) {
    return IPCZ_RESULT_ALREADY_EXISTS;
  }

  uint32_t max_queued_parcels = std::numeric_limits<uint32_t>::max();
  uint32_t max_queued_bytes = std::numeric_limits<uint32_t>::max();
  if (limits) {
    if (limits->max_queued_parcels > 0) {
      max_queued_parcels = limits->max_queued_parcels;
    }
    if (limits->max_queued_bytes > 0) {
      max_queued_bytes = limits->max_queued_bytes;
    }
  }

  IpczPortalStatus& status = state_.status;
  if (max_queued_parcels > 0 &&
      status.num_remote_parcels >= max_queued_parcels) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  } else if (max_queued_bytes <= status.num_remote_bytes) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  } else if (max_queued_bytes - status.num_remote_bytes < data.size()) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  }

  bool remote_portal_moved = false;
  {
    PortalControlBlock::Locked control(control_block_);
    switch (control.opposite(side_).status) {
      case PortalControlBlock::Status::kReady:
        control.side(side_).queue_state.num_sent_bytes += data.size();
        ++control.side(side_).queue_state.num_sent_parcels;
        break;
      case PortalControlBlock::Status::kClosed:
        // Nothing to do if the peer is already closed.
        status.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
        return IPCZ_RESULT_NOT_FOUND;
      case PortalControlBlock::Status::kMoving:
        remote_portal_moved = true;
        break;
    }
  }

  std::vector<PortalInTransit> parcel_portals;
  parcel_portals.reserve(portals.size());
  for (PortalInTransit& portal : portals) {
    parcel_portals.push_back(std::move(portal));
  }

  std::vector<os::Handle> parcel_os_handles;
  parcel_os_handles.reserve(os_handles.size());
  for (const IpczOSHandle& handle : os_handles) {
    parcel_os_handles.push_back(os::Handle::FromIpczOSHandle(handle));
  }

  Parcel parcel;
  parcel.SetData(std::vector<uint8_t>(data.begin(), data.end()));
  parcel.SetPortals(std::move(parcel_portals));
  parcel.SetOSHandles(std::move(parcel_os_handles));
  ++status.num_remote_parcels;
  status.num_remote_bytes += data.size();

  if (remote_portal_moved) {
    // Queue the parcel out and signal that the Portal should switch to a
    // buffering backend immediately.
    state_.outgoing_parcels.push(std::move(parcel));
    return IPCZ_RESULT_UNAVAILABLE;
  }

  link_->SendParcel(remote_portal_, parcel);
  return IPCZ_RESULT_OK;
}

IpczResult RoutedPortalBackend::BeginPut(IpczBeginPutFlags flags,
                                         const IpczPutLimits* limits,
                                         uint32_t& num_data_bytes,
                                         void** data) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(!state_.closed);
  if (state_.pending_parcel) {
    return IPCZ_RESULT_ALREADY_EXISTS;
  }

  uint32_t max_queued_parcels = std::numeric_limits<uint32_t>::max();
  uint32_t max_queued_bytes = std::numeric_limits<uint32_t>::max();
  if (limits) {
    if (limits->max_queued_parcels > 0) {
      max_queued_parcels = limits->max_queued_parcels;
    }
    if (limits->max_queued_bytes > 0) {
      max_queued_bytes = limits->max_queued_bytes;
    }
  }

  IpczPortalStatus& status = state_.status;
  if (max_queued_parcels > 0 &&
      status.num_remote_parcels >= max_queued_parcels) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  } else if (max_queued_bytes <= status.num_remote_bytes) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  } else if (max_queued_bytes - status.num_remote_bytes < num_data_bytes) {
    if ((flags & IPCZ_BEGIN_PUT_ALLOW_PARTIAL) &&
        status.num_remote_bytes < limits->max_queued_bytes) {
      num_data_bytes = limits->max_queued_bytes - status.num_remote_bytes;
    } else {
      return IPCZ_RESULT_RESOURCE_EXHAUSTED;
    }
  }

  state_.pending_parcel.emplace();
  if (data) {
    state_.pending_parcel->ResizeData(num_data_bytes);
    *data = state_.pending_parcel->data_view().data();
  }
  return IPCZ_RESULT_OK;
}

IpczResult RoutedPortalBackend::CommitPut(
    uint32_t num_data_bytes_produced,
    absl::Span<PortalInTransit> portals,
    absl::Span<const IpczOSHandle> os_handles) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(!state_.closed);
  if (!state_.pending_parcel) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  if (state_.pending_parcel->data_view().size() < num_data_bytes_produced) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  IpczPortalStatus& status = state_.status;
  bool remote_portal_moved = false;
  {
    PortalControlBlock::Locked control(control_block_);
    switch (control.opposite(side_).status) {
      case PortalControlBlock::Status::kReady:
        control.side(side_).queue_state.num_sent_bytes +=
            num_data_bytes_produced;
        ++control.side(side_).queue_state.num_sent_parcels;
        break;
      case PortalControlBlock::Status::kClosed:
        // Nothing to do if the peer is already closed.
        status.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
        return IPCZ_RESULT_NOT_FOUND;
      case PortalControlBlock::Status::kMoving:
        remote_portal_moved = true;
        break;
    }
  }

  std::vector<PortalInTransit> parcel_portals;
  parcel_portals.reserve(portals.size());
  for (PortalInTransit& portal : portals) {
    parcel_portals.push_back(std::move(portal));
  }
  std::vector<os::Handle> parcel_os_handles;
  parcel_os_handles.reserve(os_handles.size());
  for (const IpczOSHandle& handle : os_handles) {
    parcel_os_handles.push_back(os::Handle::FromIpczOSHandle(handle));
  }

  Parcel parcel = std::move(*state_.pending_parcel);
  state_.pending_parcel.reset();
  parcel.ResizeData(num_data_bytes_produced);
  parcel.SetPortals(std::move(parcel_portals));
  parcel.SetOSHandles(std::move(parcel_os_handles));
  status.num_remote_bytes += num_data_bytes_produced;
  ++status.num_remote_parcels;

  if (remote_portal_moved) {
    // Queue the parcel out and signal that the Portal should switch to a
    // buffering backend immediately.
    state_.outgoing_parcels.push(std::move(parcel));
    return IPCZ_RESULT_UNAVAILABLE;
  }

  link_->SendParcel(remote_portal_, parcel);
  return IPCZ_RESULT_OK;
}

IpczResult RoutedPortalBackend::AbortPut() {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(!state_.closed);
  if (!state_.pending_parcel) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  state_.pending_parcel.reset();
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult RoutedPortalBackend::Get(void* data,
                                    uint32_t* num_data_bytes,
                                    IpczHandle* portals,
                                    uint32_t* num_portals,
                                    IpczOSHandle* os_handles,
                                    uint32_t* num_os_handles) {
  absl::MutexLock lock(&mutex_);
  if (state_.in_two_phase_get) {
    return IPCZ_RESULT_ALREADY_EXISTS;
  }

  if (state_.incoming_parcels.empty()) {
    if (state_.status.flags & IPCZ_PORTAL_STATUS_PEER_CLOSED) {
      return IPCZ_RESULT_NOT_FOUND;
    }
    return IPCZ_RESULT_UNAVAILABLE;
  }

  Parcel& next_parcel = state_.incoming_parcels.front();
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

  Parcel parcel = state_.incoming_parcels.pop();

  {
    PortalControlBlock::Locked control(control_block_);
    control.side(side_).queue_state.num_read_bytes += parcel.data_view().size();
    ++control.side(side_).queue_state.num_read_parcels;
  }

  memcpy(data, parcel.data_view().data(), parcel.data_view().size());
  state_.status.num_local_bytes -= parcel.data_view().size();
  --state_.status.num_local_parcels;
  parcel.Consume(portals, os_handles);
  return IPCZ_RESULT_OK;
}

IpczResult RoutedPortalBackend::BeginGet(const void** data,
                                         uint32_t* num_data_bytes,
                                         uint32_t* num_portals,
                                         uint32_t* num_os_handles) {
  absl::MutexLock lock(&mutex_);
  if (state_.in_two_phase_get) {
    return IPCZ_RESULT_ALREADY_EXISTS;
  }

  if (state_.incoming_parcels.empty()) {
    if (state_.status.flags & IPCZ_PORTAL_STATUS_PEER_CLOSED) {
      return IPCZ_RESULT_NOT_FOUND;
    }
    return IPCZ_RESULT_UNAVAILABLE;
  }

  Parcel& next_parcel = state_.incoming_parcels.front();
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

  state_.in_two_phase_get = true;
  return IPCZ_RESULT_OK;
}

IpczResult RoutedPortalBackend::CommitGet(uint32_t num_data_bytes_consumed,
                                          IpczHandle* portals,
                                          uint32_t* num_portals,
                                          IpczOSHandle* os_handles,
                                          uint32_t* num_os_handles) {
  absl::MutexLock lock(&mutex_);
  if (!state_.in_two_phase_get) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  Parcel& next_parcel = state_.incoming_parcels.front();
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

  uint32_t num_parcels_consumed = 0;
  if (num_data_bytes_consumed == data_size) {
    Parcel parcel = state_.incoming_parcels.pop();
    parcel.Consume(portals, os_handles);
    --state_.status.num_local_parcels;
    num_parcels_consumed = 1;
  } else {
    Parcel& parcel = state_.incoming_parcels.front();
    parcel.ConsumePartial(num_data_bytes_consumed, portals, os_handles);
  }

  {
    PortalControlBlock::Locked control(control_block_);
    control.side(side_).queue_state.num_read_bytes += num_data_bytes_consumed;
    control.side(side_).queue_state.num_read_parcels += num_parcels_consumed;
  }

  state_.status.num_local_bytes -= num_data_bytes_consumed;
  state_.in_two_phase_get = false;
  return IPCZ_RESULT_OK;
}

IpczResult RoutedPortalBackend::AbortGet() {
  absl::MutexLock lock(&mutex_);
  if (!state_.in_two_phase_get) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  state_.in_two_phase_get = false;
  return IPCZ_RESULT_OK;
}

IpczResult RoutedPortalBackend::AddTrap(std::unique_ptr<Trap> trap) {
  absl::MutexLock lock(&mutex_);
  return state_.traps.Add(std::move(trap));
}

IpczResult RoutedPortalBackend::ArmTrap(
    Trap& trap,
    IpczTrapConditionFlags* satisfied_condition_flags,
    IpczPortalStatus* status) {
  absl::MutexLock lock(&mutex_);
  IpczTrapConditionFlags flags = 0;
  IpczResult result = trap.Arm(state_.status, flags);
  if (result == IPCZ_RESULT_OK) {
    return IPCZ_RESULT_OK;
  }

  if (satisfied_condition_flags) {
    *satisfied_condition_flags = flags;
  }

  if (status) {
    size_t out_size = status->size;
    size_t copy_size = std::min(out_size, sizeof(state_.status));
    memcpy(status, &state_.status, copy_size);
    status->size = static_cast<uint32_t>(out_size);
  }

  return result;
}

IpczResult RoutedPortalBackend::RemoveTrap(Trap& trap) {
  absl::MutexLock lock(&mutex_);
  return state_.traps.Remove(trap);
}

}  // namespace core
}  // namespace ipcz
