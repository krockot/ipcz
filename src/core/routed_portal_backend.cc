// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/routed_portal_backend.h"

#include <utility>

#include "core/buffering_portal_backend.h"
#include "core/node.h"
#include "core/parcel.h"
#include "core/parcel_queue.h"
#include "core/trap.h"
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
        (void)side_;
      }

RoutedPortalBackend::~RoutedPortalBackend() = default;

void RoutedPortalBackend::AdoptBufferingBackendState(
    BufferingPortalBackend& backend) {
  absl::MutexLock lock(&mutex_);
  absl::MutexLock their_lock(&backend.mutex_);
  closed_ = backend.closed_;
  if (backend.pending_parcel_) {
    pending_parcel_ = std::move(backend.pending_parcel_);
  }

  outgoing_parcels_ = std::move(backend.outgoing_parcels_);
  num_outgoing_bytes_ = backend.num_outgoing_bytes_;
}

PortalBackend::Type RoutedPortalBackend::GetType() const {
  return Type::kRouted;
}

bool RoutedPortalBackend::CanTravelThroughPortal(Portal& sender) {
  return false;
}

IpczResult RoutedPortalBackend::Close(
    Node::LockedRouter& router,
    std::vector<mem::Ref<Portal>>& other_portals_to_close) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(!closed_);
  closed_ = true;
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult RoutedPortalBackend::QueryStatus(IpczPortalStatus& status) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(!closed_);
  status.flags = 0;
  status.num_local_parcels = 0;
  status.num_local_bytes = 0;
  status.num_remote_parcels = outgoing_parcels_.size();
  status.num_remote_bytes = num_outgoing_bytes_;
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
               num_outgoing_bytes_ >= limits->max_queued_bytes) {
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
               num_outgoing_bytes_ + num_data_bytes >
                   limits->max_queued_bytes) {
      if ((flags & IPCZ_BEGIN_PUT_ALLOW_PARTIAL) &&
          num_outgoing_bytes_ < limits->max_queued_bytes) {
        num_data_bytes = limits->max_queued_bytes - num_outgoing_bytes_;
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
    if (peer_closed_) {
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
  num_incoming_bytes_ -= parcel.data_view().size();
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
    if (peer_closed_) {
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
  } else {
    Parcel& parcel = incoming_parcels_.front();
    parcel.ConsumePartial(num_data_bytes_consumed, portals, os_handles);
  }

  num_incoming_bytes_ -= num_data_bytes_consumed;
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
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult RoutedPortalBackend::ArmTrap(
    Trap& trap,
    IpczTrapConditions* satisfied_conditions,
    IpczPortalStatus* status) {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult RoutedPortalBackend::RemoveTrap(Trap& trap) {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

}  // namespace core
}  // namespace ipcz
