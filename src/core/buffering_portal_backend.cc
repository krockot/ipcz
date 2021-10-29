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

BufferingPortalBackend::BufferingPortalBackend(Side side) : side_(side) {}

BufferingPortalBackend::~BufferingPortalBackend() = default;

PortalBackend::Type BufferingPortalBackend::GetType() const {
  return Type::kBuffering;
}

bool BufferingPortalBackend::CanTravelThroughPortal(Portal& sender) {
  return true;
}

bool BufferingPortalBackend::AcceptParcel(Parcel& parcel,
                                          TrapEventDispatcher& dispatcher) {
  // TODO: allow receipt of parcels - a portal may transition to buffering while
  // parcels are already in flight for it.
  LOG(ERROR) << "not yet";
  return false;
}

IpczResult BufferingPortalBackend::Close(
    Node::LockedRouter& router,
    std::vector<mem::Ref<Portal>>& other_portals_to_close) {
  absl::MutexLock lock(&mutex_);
  closed_ = true;
  return IPCZ_RESULT_OK;
}

IpczResult BufferingPortalBackend::QueryStatus(IpczPortalStatus& status) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(!closed_);
  status.flags = 0;
  status.num_local_parcels = 0;
  status.num_local_bytes = 0;
  status.num_remote_parcels = outgoing_parcels_.size();
  status.num_remote_bytes = num_outgoing_bytes_;
  return IPCZ_RESULT_OK;
}

IpczResult BufferingPortalBackend::Put(
    Node::LockedRouter& router,
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
  outgoing_parcels_.push(std::move(parcel));
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

IpczResult BufferingPortalBackend::CommitPut(
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
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult BufferingPortalBackend::ArmTrap(
    Trap& trap,
    IpczTrapConditions* satisfied_conditions,
    IpczPortalStatus* status) {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

IpczResult BufferingPortalBackend::RemoveTrap(Trap& trap) {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

}  // namespace core
}  // namespace ipcz
