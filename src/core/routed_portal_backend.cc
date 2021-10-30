// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/routed_portal_backend.h"

#include <algorithm>
#include <utility>

#include "core/buffering_portal_backend.h"
#include "core/node.h"
#include "core/parcel.h"
#include "core/parcel_queue.h"
#include "core/portal_control_block.h"
#include "core/trap.h"
#include "core/trap_event_dispatcher.h"
#include "debug/log.h"
#include "util/handle_util.h"

namespace ipcz {
namespace core {

RoutedPortalBackend::RoutedPortalBackend(
    const PortalName& name,
    const PortalAddress& peer_address,
    Side side,
    os::Memory::Mapping control_block_mapping)
    : name_(name),
      peer_address_(peer_address),
      side_(side),
      control_block_mapping_(std::move(control_block_mapping)) {
  memset(&status_, 0, sizeof(status_));
  status_.size = sizeof(status_);
}

RoutedPortalBackend::~RoutedPortalBackend() = default;

bool RoutedPortalBackend::AdoptBufferingBackendState(
    Node::LockedRouter& router,
    BufferingPortalBackend& backend) {
  std::forward_list<Parcel> parcels_to_send;
  {
    absl::MutexLock our_lock(&mutex_);
    absl::MutexLock their_lock(&backend.mutex_);
    {
      PortalControlBlock::Locked control(control_block_);
      switch (control.opposite(side_).status) {
        case PortalControlBlock::Status::kReady:
          // Ensure that a ready peer's node will retain necessary state long
          // enough to forward the parcels we're about to flush to it, even if
          // the destination portal moves before those parcels arrive.
          control.side(side_).queue_state.num_sent_parcels +=
              backend.status_.num_remote_parcels;
          control.side(side_).queue_state.num_sent_bytes +=
              backend.status_.num_remote_bytes;
          break;

        case PortalControlBlock::Status::kClosed:
          // Don't bother forwarding anything, the peer is closed. The only
          // relevant state to adopt is whether we were also already closed. If
          // the peer has messages in flight to us, we can still receive them if
          // we remain open.
          closed_ = backend.closed_;
          status_.num_remote_bytes = 0;
          status_.num_remote_parcels = 0;
          return true;

        case PortalControlBlock::Status::kMoving:
          // Cancel adoption to leave this portal in a buffering state.
          return false;
      }
    }

    closed_ = backend.closed_;
    pending_parcel_ = std::move(backend.pending_parcel_);
    traps_ = std::move(backend.traps_);
    parcels_to_send = backend.outgoing_parcels_.TakeParcels();
  }

  for (auto& parcel : parcels_to_send) {
    router->RouteParcel(peer_address_, parcel);
  }

  return true;
}

PortalBackend::Type RoutedPortalBackend::GetType() const {
  return Type::kRouted;
}

bool RoutedPortalBackend::CanTravelThroughPortal(Portal& sender) {
  // TODO
  return false;
}

bool RoutedPortalBackend::AcceptParcel(Parcel& parcel,
                                       TrapEventDispatcher& dispatcher) {
  absl::MutexLock lock(&mutex_);
  incoming_parcels_.push(std::move(parcel));
  status_.num_local_bytes += parcel.data_view().size();
  ++status_.num_local_parcels;
  traps_.MaybeNotify(dispatcher, status_);
  return true;
}

bool RoutedPortalBackend::NotifyPeerClosed(TrapEventDispatcher& dispatcher) {
  absl::MutexLock lock(&mutex_);
  if (status_.flags & IPCZ_PORTAL_STATUS_PEER_CLOSED) {
    return true;
  }

  status_.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
  traps_.MaybeNotify(dispatcher, status_);
  return true;
}

IpczResult RoutedPortalBackend::Close(
    Node::LockedRouter& router,
    std::vector<mem::Ref<Portal>>& other_portals_to_close) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(!closed_);
  closed_ = true;

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
  router->NotifyPeerClosed(peer_address_);
  return IPCZ_RESULT_OK;
}

IpczResult RoutedPortalBackend::QueryStatus(IpczPortalStatus& status) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(!closed_);
  status = status_;
  return IPCZ_RESULT_OK;
}

IpczResult RoutedPortalBackend::Put(Node::LockedRouter& router,
                                    absl::Span<const uint8_t> data,
                                    absl::Span<const IpczHandle> portals,
                                    absl::Span<const IpczOSHandle> os_handles,
                                    const IpczPutLimits* limits) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(!closed_);
  if (pending_parcel_) {
    return IPCZ_RESULT_ALREADY_EXISTS;
  }

  if (limits) {
    if (limits->max_queued_parcels > 0 &&
        outgoing_parcels_.size() >= limits->max_queued_parcels) {
      return IPCZ_RESULT_RESOURCE_EXHAUSTED;
    } else if (limits->max_queued_bytes > 0 &&
               status_.num_remote_bytes >= limits->max_queued_bytes) {
      return IPCZ_RESULT_RESOURCE_EXHAUSTED;
    }
  }

  std::vector<mem::Ref<Portal>> parcel_portals;
  parcel_portals.reserve(portals.size());
  for (IpczHandle portal : portals) {
    parcel_portals.emplace_back(mem::RefCounted::kAdoptExistingRef,
                                ToPtr<Portal>(portal));
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
  router->RouteParcel(peer_address_, parcel);
  // outgoing_parcels_.push(std::move(parcel));
  return IPCZ_RESULT_OK;
}

IpczResult RoutedPortalBackend::BeginPut(IpczBeginPutFlags flags,
                                         const IpczPutLimits* limits,
                                         uint32_t& num_data_bytes,
                                         void** data) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(!closed_);
  if (pending_parcel_) {
    return IPCZ_RESULT_ALREADY_EXISTS;
  }

  if (limits) {
    if (limits->max_queued_parcels > 0 &&
        outgoing_parcels_.size() >= limits->max_queued_parcels) {
      return IPCZ_RESULT_RESOURCE_EXHAUSTED;
    } else if (limits->max_queued_bytes > 0 &&
               status_.num_remote_bytes + num_data_bytes >
                   limits->max_queued_bytes) {
      if ((flags & IPCZ_BEGIN_PUT_ALLOW_PARTIAL) &&
          status_.num_remote_bytes < limits->max_queued_bytes) {
        num_data_bytes = limits->max_queued_bytes - status_.num_remote_bytes;
      } else {
        return IPCZ_RESULT_RESOURCE_EXHAUSTED;
      }
    }
  }

  pending_parcel_.emplace();
  if (data) {
    pending_parcel_->ResizeData(num_data_bytes);
    *data = pending_parcel_->data_view().data();
  }
  return IPCZ_RESULT_OK;
}

IpczResult RoutedPortalBackend::CommitPut(
    Node::LockedRouter& router,
    uint32_t num_data_bytes_produced,
    absl::Span<const IpczHandle> portals,
    absl::Span<const IpczOSHandle> os_handles) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(!closed_);
  if (!pending_parcel_) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  if (pending_parcel_->data_view().size() < num_data_bytes_produced) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  std::vector<mem::Ref<Portal>> parcel_portals;
  parcel_portals.reserve(portals.size());
  for (IpczHandle portal : portals) {
    parcel_portals.emplace_back(mem::RefCounted::kAdoptExistingRef,
                                ToPtr<Portal>(portal));
  }
  std::vector<os::Handle> parcel_os_handles;
  parcel_os_handles.reserve(os_handles.size());
  for (const IpczOSHandle& handle : os_handles) {
    parcel_os_handles.push_back(os::Handle::FromIpczOSHandle(handle));
  }

  Parcel parcel = std::move(*pending_parcel_);
  pending_parcel_.reset();

  parcel.ResizeData(num_data_bytes_produced);
  parcel.SetPortals(std::move(parcel_portals));
  parcel.SetOSHandles(std::move(parcel_os_handles));
  outgoing_parcels_.push(std::move(parcel));
  return IPCZ_RESULT_OK;
}

IpczResult RoutedPortalBackend::AbortPut() {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(!closed_);
  if (!pending_parcel_) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  pending_parcel_.reset();
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult RoutedPortalBackend::Get(void* data,
                                    uint32_t* num_data_bytes,
                                    IpczHandle* portals,
                                    uint32_t* num_portals,
                                    IpczOSHandle* os_handles,
                                    uint32_t* num_os_handles) {
  absl::MutexLock lock(&mutex_);
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
  memcpy(data, parcel.data_view().data(), parcel.data_view().size());
  status_.num_local_bytes -= parcel.data_view().size();
  --status_.num_local_parcels;
  parcel.Consume(portals, os_handles);
  return IPCZ_RESULT_OK;
}

IpczResult RoutedPortalBackend::BeginGet(const void** data,
                                         uint32_t* num_data_bytes,
                                         uint32_t* num_portals,
                                         uint32_t* num_os_handles) {
  absl::MutexLock lock(&mutex_);
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

IpczResult RoutedPortalBackend::CommitGet(uint32_t num_data_bytes_consumed,
                                          IpczHandle* portals,
                                          uint32_t* num_portals,
                                          IpczOSHandle* os_handles,
                                          uint32_t* num_os_handles) {
  absl::MutexLock lock(&mutex_);
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
    parcel.Consume(portals, os_handles);
    --status_.num_local_parcels;
  } else {
    Parcel& parcel = incoming_parcels_.front();
    parcel.ConsumePartial(num_data_bytes_consumed, portals, os_handles);
  }

  status_.num_local_bytes -= num_data_bytes_consumed;
  in_two_phase_get_ = false;
  return IPCZ_RESULT_OK;
}

IpczResult RoutedPortalBackend::AbortGet() {
  absl::MutexLock lock(&mutex_);
  if (!in_two_phase_get_) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  in_two_phase_get_ = false;
  return IPCZ_RESULT_OK;
}

IpczResult RoutedPortalBackend::AddTrap(std::unique_ptr<Trap> trap) {
  absl::MutexLock lock(&mutex_);
  return traps_.Add(std::move(trap));
}

IpczResult RoutedPortalBackend::ArmTrap(
    Trap& trap,
    IpczTrapConditionFlags* satisfied_condition_flags,
    IpczPortalStatus* status) {
  absl::MutexLock lock(&mutex_);
  IpczTrapConditionFlags flags = 0;
  IpczResult result = trap.Arm(status_, flags);
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

IpczResult RoutedPortalBackend::RemoveTrap(Trap& trap) {
  absl::MutexLock lock(&mutex_);
  return traps_.Remove(trap);
}

}  // namespace core
}  // namespace ipcz
