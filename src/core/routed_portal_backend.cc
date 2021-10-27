// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/routed_portal_backend.h"

#include <utility>

#include "core/buffering_portal_backend.h"
#include "core/node.h"
#include "core/trap.h"
#include "util/handle_util.h"

namespace ipcz {
namespace core {

RoutedPortalBackend::RoutedPortalBackend(const PortalName& name,
                                         const PortalAddress& peer_address)
    : name_(name), peer_address_(peer_address) {}

RoutedPortalBackend::~RoutedPortalBackend() = default;

void RoutedPortalBackend::AdoptBufferingBackendState(
    BufferingPortalBackend& backend) {
  absl::MutexLock lock(&mutex_);
  absl::MutexLock their_lock(&backend.mutex_);
  closed_ = backend.closed_;
  if (backend.pending_parcel_) {
    pending_parcel_ = std::make_unique<Parcel>();
    pending_parcel_->data = std::move(backend.pending_parcel_->data);
  }

  num_outgoing_parcels_ = backend.outgoing_parcels_.size();
  num_outgoing_bytes_ = backend.num_outgoing_bytes_;

  std::unique_ptr<Parcel> parcel;
  for (auto it = backend.outgoing_parcels_.rbegin();
       it != backend.outgoing_parcels_.rend(); ++it) {
    auto tail_parcel = std::make_unique<Parcel>();
    tail_parcel->data = std::move(it->data);
    tail_parcel->portals = std::move(it->portals);
    tail_parcel->os_handles = std::move(it->os_handles);
    if (!parcel) {
      last_outgoing_parcel_ = tail_parcel.get();
    } else {
      tail_parcel->next_parcel = std::move(parcel);
    }
    parcel = std::move(tail_parcel);
  }

  next_outgoing_parcel_ = std::move(parcel);
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
  status.num_remote_parcels = static_cast<uint32_t>(num_outgoing_parcels_);
  status.num_remote_bytes = static_cast<uint32_t>(num_outgoing_bytes_);
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
        num_outgoing_parcels_ >= limits->max_queued_parcels) {
      return IPCZ_RESULT_RESOURCE_EXHAUSTED;
    } else if (limits->max_queued_bytes > 0 &&
               num_outgoing_bytes_ >= limits->max_queued_bytes) {
      return IPCZ_RESULT_RESOURCE_EXHAUSTED;
    }
  }

  auto parcel = std::make_unique<Parcel>();
  parcel->data = std::vector<uint8_t>(data.begin(), data.end());
  parcel->portals.reserve(portals.size());
  for (IpczHandle portal : portals) {
    parcel->portals.emplace_back(mem::RefCounted::kAdoptExistingRef,
                                 ToPtr<Portal>(portal));
  }
  parcel->os_handles.reserve(os_handles.size());
  for (const IpczOSHandle& handle : os_handles) {
    parcel->os_handles.push_back(os::Handle::FromIpczOSHandle(handle));
  }

  if (last_outgoing_parcel_) {
    Parcel* previous_tail_parcel = last_outgoing_parcel_;
    last_outgoing_parcel_ = parcel.get();
    previous_tail_parcel->next_parcel = std::move(parcel);
  } else {
    last_outgoing_parcel_ = parcel.get();
    next_outgoing_parcel_ = std::move(parcel);
  }

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
        num_outgoing_parcels_ >= limits->max_queued_parcels) {
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

  pending_parcel_ = std::make_unique<Parcel>();
  if (data) {
    pending_parcel_->data.resize(num_data_bytes);
    *data = pending_parcel_->data.data();
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

  if (pending_parcel_->data.size() < num_data_bytes_produced) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  std::unique_ptr<Parcel> parcel = std::move(pending_parcel_);
  pending_parcel_.reset();

  parcel->data.resize(num_data_bytes_produced);
  parcel->portals.reserve(portals.size());
  for (IpczHandle portal : portals) {
    parcel->portals.emplace_back(mem::RefCounted::kAdoptExistingRef,
                                 ToPtr<Portal>(portal));
  }
  parcel->os_handles.reserve(os_handles.size());
  for (const IpczOSHandle& handle : os_handles) {
    parcel->os_handles.push_back(os::Handle::FromIpczOSHandle(handle));
  }

  if (last_outgoing_parcel_) {
    Parcel* previous_tail_parcel = last_outgoing_parcel_;
    last_outgoing_parcel_ = parcel.get();
    previous_tail_parcel->next_parcel = std::move(parcel);
  } else {
    last_outgoing_parcel_ = parcel.get();
    next_outgoing_parcel_ = std::move(parcel);
  }

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

  std::unique_ptr<Parcel>& next_parcel = next_incoming_parcel_;
  const bool empty = !next_parcel;
  if (empty) {
    if (peer_closed_) {
      return IPCZ_RESULT_NOT_FOUND;
    }
    return IPCZ_RESULT_UNAVAILABLE;
  }

  IpczResult result = IPCZ_RESULT_OK;
  uint32_t available_data_storage = num_data_bytes ? *num_data_bytes : 0;
  uint32_t available_portal_storage = num_portals ? *num_portals : 0;
  uint32_t available_os_handle_storage = num_os_handles ? *num_os_handles : 0;
  if (next_parcel->data.size() > available_data_storage ||
      next_parcel->portals.size() > available_portal_storage ||
      next_parcel->os_handles.size() > available_os_handle_storage) {
    result = IPCZ_RESULT_RESOURCE_EXHAUSTED;
  }

  if (num_data_bytes) {
    *num_data_bytes = static_cast<uint32_t>(next_parcel->data.size());
  }
  if (num_portals) {
    *num_portals = static_cast<uint32_t>(next_parcel->portals.size());
  }
  if (num_os_handles) {
    *num_os_handles = static_cast<uint32_t>(next_parcel->os_handles.size());
  }

  if (result != IPCZ_RESULT_OK) {
    return result;
  }

  std::vector<uint8_t> parcel_data = ConsumeParcel(portals, os_handles);
  memcpy(data, parcel_data.data(), parcel_data.size());
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

  std::unique_ptr<Parcel>& next_parcel = next_incoming_parcel_;
  const bool empty = !next_parcel;
  if (empty) {
    if (peer_closed_) {
      return IPCZ_RESULT_NOT_FOUND;
    }
    return IPCZ_RESULT_UNAVAILABLE;
  }

  const size_t data_size = next_parcel->data.size() - next_parcel->data_offset;
  if (num_data_bytes) {
    *num_data_bytes = static_cast<uint32_t>(data_size);
  }
  if (num_portals) {
    *num_portals = static_cast<uint32_t>(next_parcel->portals.size());
  }
  if (num_os_handles) {
    *num_os_handles = static_cast<uint32_t>(next_parcel->os_handles.size());
  }

  if (data_size > 0) {
    if (!data || !num_data_bytes) {
      return IPCZ_RESULT_RESOURCE_EXHAUSTED;
    }
    *data = next_parcel->data.data() + next_parcel->data_offset;
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

  std::unique_ptr<Parcel>& next_parcel = next_incoming_parcel_;
  const size_t data_size = next_parcel->data.size() - next_parcel->data_offset;
  if (num_data_bytes_consumed > data_size) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  uint32_t available_portal_storage = num_portals ? *num_portals : 0;
  uint32_t available_os_handle_storage = num_os_handles ? *num_os_handles : 0;
  if (num_portals) {
    *num_portals = static_cast<uint32_t>(next_parcel->portals.size());
  }
  if (num_os_handles) {
    *num_os_handles = static_cast<uint32_t>(next_parcel->os_handles.size());
  }
  if (available_portal_storage < next_parcel->portals.size() ||
      available_os_handle_storage < next_parcel->os_handles.size()) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  }

  if (num_data_bytes_consumed == data_size) {
    ConsumeParcel(portals, os_handles);
  } else {
    ConsumePartialParcel(num_data_bytes_consumed, portals, os_handles);
  }
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

std::vector<uint8_t> RoutedPortalBackend::ConsumeParcel(
    IpczHandle* portals,
    IpczOSHandle* os_handles) {
  mutex_.AssertHeld();
  ABSL_ASSERT(next_incoming_parcel_);
  std::unique_ptr<Parcel> parcel = std::move(next_incoming_parcel_);
  if (!parcel->next_parcel) {
    last_incoming_parcel_ = nullptr;
    next_incoming_parcel_.reset();
  } else {
    next_incoming_parcel_ = std::move(parcel->next_parcel);
  }
  ConsumePortalsAndHandles(*parcel, portals, os_handles);

  num_incoming_parcels_--;
  num_incoming_bytes_ -= parcel->data.size();

  // TODO: if the peer is closed and this was the last message, scan for traps
  // observing portal death

  return std::move(parcel->data);
}

void RoutedPortalBackend::ConsumePartialParcel(size_t num_bytes_consumed,
                                               IpczHandle* portals,
                                               IpczOSHandle* os_handles) {
  mutex_.AssertHeld();

  Parcel* parcel = next_incoming_parcel_.get();
  ABSL_ASSERT(parcel);
  parcel->data_offset += num_bytes_consumed;

  ConsumePortalsAndHandles(*parcel, portals, os_handles);

  num_incoming_bytes_ -= num_bytes_consumed;
}

void RoutedPortalBackend::ConsumePortalsAndHandles(Parcel& parcel,
                                                   IpczHandle* portals,
                                                   IpczOSHandle* os_handles) {
  mutex_.AssertHeld();
  for (size_t i = 0; i < parcel.portals.size(); ++i) {
    portals[i] = ToHandle(parcel.portals[i].release());
  }
  for (size_t i = 0; i < parcel.os_handles.size(); ++i) {
    os::Handle::ToIpczOSHandle(std::move(parcel.os_handles[i]), &os_handles[i]);
  }
}

RoutedPortalBackend::Parcel::Parcel() = default;

RoutedPortalBackend::Parcel::Parcel(Parcel&& other) = default;

RoutedPortalBackend::Parcel& RoutedPortalBackend::Parcel::operator=(
    Parcel&& other) = default;

RoutedPortalBackend::Parcel::~Parcel() = default;

}  // namespace core
}  // namespace ipcz
