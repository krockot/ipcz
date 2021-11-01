// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/buffering_portal_backend.h"

#include <utility>

#include "core/node.h"
#include "core/trap.h"
#include "debug/log.h"
#include "util/handle_util.h"

namespace ipcz {
namespace core {

BufferingPortalBackend::BufferingPortalBackend(Side side) : side_(side) {
  memset(&status_, 0, sizeof(status_));
  status_.size = sizeof(status_);
}

BufferingPortalBackend::~BufferingPortalBackend() = default;

PortalBackend::Type BufferingPortalBackend::GetType() const {
  return Type::kBuffering;
}

bool BufferingPortalBackend::CanTravelThroughPortal(Portal& sender) {
  return true;
}

void BufferingPortalBackend::PrepareForTravel(
    PortalInTransit& portal_in_transit) {}

bool BufferingPortalBackend::AcceptParcel(Parcel& parcel,
                                          TrapEventDispatcher& dispatcher) {
  // TODO: allow receipt of parcels - a portal may transition to buffering while
  // parcels are already in flight for it.
  LOG(ERROR) << "not yet";
  return false;
}

bool BufferingPortalBackend::NotifyPeerClosed(TrapEventDispatcher& dispatcher) {
  absl::MutexLock lock(&mutex_);
  status_.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
  return true;
}

IpczResult BufferingPortalBackend::Close(
    std::vector<mem::Ref<Portal>>& other_portals_to_close) {
  absl::MutexLock lock(&mutex_);
  closed_ = true;
  return IPCZ_RESULT_OK;
}

IpczResult BufferingPortalBackend::QueryStatus(IpczPortalStatus& status) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(!closed_);
  status = status_;
  return IPCZ_RESULT_OK;
}

IpczResult BufferingPortalBackend::Put(
    absl::Span<const uint8_t> data,
    absl::Span<PortalInTransit> portals,
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
  outgoing_parcels_.push(std::move(parcel));
  status_.num_remote_parcels++;
  status_.num_remote_bytes += data.size();
  return IPCZ_RESULT_OK;
}

IpczResult BufferingPortalBackend::BeginPut(IpczBeginPutFlags flags,
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

IpczResult BufferingPortalBackend::CommitPut(
    uint32_t num_data_bytes_produced,
    absl::Span<PortalInTransit> portals,
    absl::Span<const IpczOSHandle> os_handles) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(!closed_);
  if (!pending_parcel_) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  if (pending_parcel_->data_view().size() < num_data_bytes_produced) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
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

  Parcel parcel = std::move(*pending_parcel_);
  pending_parcel_.reset();

  parcel.ResizeData(num_data_bytes_produced);
  parcel.SetPortals(std::move(parcel_portals));
  parcel.SetOSHandles(std::move(parcel_os_handles));
  outgoing_parcels_.push(std::move(parcel));
  status_.num_remote_bytes += num_data_bytes_produced;
  status_.num_remote_parcels++;
  return IPCZ_RESULT_OK;
}

IpczResult BufferingPortalBackend::AbortPut() {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(!closed_);
  if (!pending_parcel_) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  pending_parcel_.reset();
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult BufferingPortalBackend::Get(void* data,
                                       uint32_t* num_data_bytes,
                                       IpczHandle* portals,
                                       uint32_t* num_portals,
                                       IpczOSHandle* os_handles,
                                       uint32_t* num_os_handles) {
  return IPCZ_RESULT_UNAVAILABLE;
}

IpczResult BufferingPortalBackend::BeginGet(const void** data,
                                            uint32_t* num_data_bytes,
                                            uint32_t* num_portals,
                                            uint32_t* num_os_handles) {
  return IPCZ_RESULT_UNAVAILABLE;
}

IpczResult BufferingPortalBackend::CommitGet(uint32_t num_data_bytes_consumed,
                                             IpczHandle* portals,
                                             uint32_t* num_portals,
                                             IpczOSHandle* os_handles,
                                             uint32_t* num_os_handles) {
  return IPCZ_RESULT_FAILED_PRECONDITION;
}

IpczResult BufferingPortalBackend::AbortGet() {
  return IPCZ_RESULT_FAILED_PRECONDITION;
}

IpczResult BufferingPortalBackend::AddTrap(std::unique_ptr<Trap> trap) {
  absl::MutexLock lock(&mutex_);
  return traps_.Add(std::move(trap));
}

IpczResult BufferingPortalBackend::ArmTrap(
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

IpczResult BufferingPortalBackend::RemoveTrap(Trap& trap) {
  absl::MutexLock lock(&mutex_);
  return traps_.Remove(trap);
}

}  // namespace core
}  // namespace ipcz
