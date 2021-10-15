// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/direct_portal_backend.h"

#include <limits>
#include <utility>
#include <vector>

#include "core/node.h"
#include "core/portal_backend.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"

namespace ipcz {
namespace core {

namespace {

Portal& ToPortal(IpczHandle handle) {
  return *reinterpret_cast<Portal*>(static_cast<uintptr_t>(handle));
}

IpczHandle ToHandle(Portal* portal) {
  return static_cast<IpczHandle>(reinterpret_cast<uintptr_t>(portal));
}

// A parcel passed between two directly connected portals.
struct Parcel {
  std::vector<uint8_t> data;
  std::vector<std::unique_ptr<PortalBackend>> portals;
  std::vector<os::Handle> os_handles;

  // Offset into data from which the next bytes will be read. Non-zero only for
  // a parcel which has already been partially consumed by a get operation.
  size_t data_offset = 0;

  // The next parcel in queue.
  std::unique_ptr<Parcel> next_parcel;
};

}  // namespace

// State for one side of a directly connected portal pair.
struct DirectPortalBackend::PortalState {
  // Back-reference to the DirectPortalBackend which controls this side of the
  // portal pair. If the side is closed already, this is null.
  DirectPortalBackend* backend = nullptr;

  // Incoming parcel queue; messages from the other side are placed here
  // directly.
  std::unique_ptr<Parcel> incoming_parcels;
  Parcel* last_incoming_parcel = nullptr;

  // Counters exposed by QueryStatus and used to enforce limits during put
  // operations.
  uint64_t num_queued_parcels = 0;
  uint64_t num_queued_data_bytes = 0;

  // Indicates whether a two-phase get is in progress.
  bool in_two_phase_get = false;

  // A pending parcel for an in-progress two-phase put.
  std::unique_ptr<Parcel> pending_parcel;
};

// State shared between two directly connected portals. Both portals' states are
// hosted here behind a common mutex to simplify synchronization when operating
// on either one.
struct DirectPortalBackend::SharedState : public mem::RefCounted {
  explicit SharedState(mem::Ref<Node> node) : node(std::move(node)) {}

  const mem::Ref<Node> node;

  absl::Mutex mutex;
  PortalState sides[2] ABSL_GUARDED_BY(mutex);

 private:
  ~SharedState() override = default;
};

DirectPortalBackend::DirectPortalBackend(mem::Ref<SharedState> state,
                                         size_t side)
    : state_(std::move(state)), side_(side) {}

DirectPortalBackend::~DirectPortalBackend() = default;

// static
DirectPortalBackend::Pair DirectPortalBackend::CreatePair(mem::Ref<Node> node) {
  auto state = mem::MakeRefCounted<SharedState>(std::move(node));
  std::unique_ptr<DirectPortalBackend> backend0(
      new DirectPortalBackend(state, 0));
  std::unique_ptr<DirectPortalBackend> backend1(
      new DirectPortalBackend(state, 1));

  absl::MutexLock lock(&state->mutex);
  state->sides[0].backend = backend0.get();
  state->sides[1].backend = backend1.get();
  return {std::move(backend0), std::move(backend1)};
}

IpczResult DirectPortalBackend::Close() {
  absl::MutexLock lock(&state_->mutex);
  PortalState& state = this_side();
  state.backend = nullptr;
  state.incoming_parcels.reset();
  state.last_incoming_parcel = nullptr;
  state.num_queued_data_bytes = 0;
  state.num_queued_parcels = 0;
  return IPCZ_RESULT_OK;
}

IpczResult DirectPortalBackend::QueryStatus(
    IpczPortalStatusFieldFlags field_flags,
    IpczPortalStatus& status) {
  absl::MutexLock lock(&state_->mutex);
  if (field_flags & IPCZ_PORTAL_STATUS_FIELD_BITS) {
    if (other_side().backend == nullptr) {
      status.bits = IPCZ_PORTAL_STATUS_BIT_CLOSED;
    } else {
      status.bits = 0;
    }
  }

  if (field_flags & IPCZ_PORTAL_STATUS_FIELD_LOCAL_PARCELS) {
    status.num_local_parcels = this_side().num_queued_parcels;
  }

  if (field_flags & IPCZ_PORTAL_STATUS_FIELD_LOCAL_BYTES) {
    status.num_local_bytes = this_side().num_queued_data_bytes;
  }

  if (field_flags & IPCZ_PORTAL_STATUS_FIELD_REMOTE_PARCELS) {
    status.num_remote_parcels = other_side().num_queued_parcels;
  }

  if (field_flags & IPCZ_PORTAL_STATUS_FIELD_REMOTE_BYTES) {
    status.num_remote_bytes = other_side().num_queued_data_bytes;
  }

  return IPCZ_RESULT_OK;
}

IpczResult DirectPortalBackend::Put(absl::Span<const uint8_t> data,
                                    absl::Span<const IpczHandle> portals,
                                    absl::Span<const IpczOSHandle> os_handles,
                                    const IpczPutLimits* limits) {
  IpczResult result = IPCZ_RESULT_OK;
  absl::MutexLock lock(&state_->mutex);
  PortalBackend* other_backend = other_side().backend;
  if (!other_backend) {
    return IPCZ_RESULT_NOT_FOUND;
  }

  if (this_side().pending_parcel) {
    return IPCZ_RESULT_ALREADY_EXISTS;
  }

  Portal* other_portal = other_backend->portal();

  auto parcel = std::make_unique<Parcel>();
  parcel->portals.reserve(portals.size());

  for (IpczHandle handle : portals) {
    Portal& portal_to_put = ToPortal(handle);

    // Safety check: we can't put ourself or our peer into our own portal.
    if (&portal_to_put == portal() || &portal_to_put == other_portal) {
      result = IPCZ_RESULT_INVALID_ARGUMENT;
      break;
    }

    parcel->portals.push_back(portal_to_put.TakeBackend());
  }

  PortalState& other_state = other_side();
  if (result == IPCZ_RESULT_OK && limits) {
    if (limits->max_queued_parcels > 0 &&
        other_state.num_queued_parcels >= limits->max_queued_parcels) {
      result = IPCZ_RESULT_RESOURCE_EXHAUSTED;
    } else if (limits->max_queued_bytes > 0 &&
               other_state.num_queued_data_bytes + data.size() >
                   limits->max_queued_bytes) {
      result = IPCZ_RESULT_RESOURCE_EXHAUSTED;
    }
  }

  if (result != IPCZ_RESULT_OK) {
    // Give the caller their portals back, because we're going to fail.
    for (size_t i = 0; i < parcel->portals.size(); ++i) {
      ToPortal(portals[i]).SetBackend(std::move(parcel->portals[i]));
    }
    return result;
  }

  parcel->data = std::vector<uint8_t>(data.begin(), data.end());
  parcel->os_handles.reserve(os_handles.size());
  for (const IpczOSHandle& handle : os_handles) {
    parcel->os_handles.push_back(os::Handle::FromIpczOSHandle(handle));
  }

  if (other_state.last_incoming_parcel) {
    Parcel* old_tail = other_state.last_incoming_parcel;
    other_state.last_incoming_parcel = parcel.get();
    old_tail->next_parcel = std::move(parcel);
  } else {
    other_state.last_incoming_parcel = parcel.get();
    other_state.incoming_parcels = std::move(parcel);
  }

  other_state.num_queued_parcels++;
  other_state.num_queued_data_bytes += data.size();

  return IPCZ_RESULT_OK;
}

IpczResult DirectPortalBackend::BeginPut(IpczBeginPutFlags flags,
                                         const IpczPutLimits* limits,
                                         uint32_t& num_data_bytes,
                                         void** data) {
  IpczResult result = IPCZ_RESULT_OK;
  absl::MutexLock lock(&state_->mutex);
  PortalBackend* other_backend = other_side().backend;
  if (!other_backend) {
    return IPCZ_RESULT_NOT_FOUND;
  }

  auto& parcel = this_side().pending_parcel;
  if (parcel) {
    return IPCZ_RESULT_ALREADY_EXISTS;
  }

  PortalState& other_state = other_side();
  if (limits) {
    if (limits->max_queued_parcels > 0 &&
        other_state.num_queued_parcels >= limits->max_queued_parcels) {
      result = IPCZ_RESULT_RESOURCE_EXHAUSTED;
    } else if (limits->max_queued_bytes > 0 &&
               other_state.num_queued_data_bytes + num_data_bytes >
                   limits->max_queued_bytes) {
      if (flags & IPCZ_BEGIN_PUT_ALLOW_PARTIAL &&
          other_state.num_queued_data_bytes < limits->max_queued_bytes) {
        num_data_bytes =
            limits->max_queued_bytes - other_state.num_queued_data_bytes;
      } else {
        result = IPCZ_RESULT_RESOURCE_EXHAUSTED;
      }
    }
  }

  if (result != IPCZ_RESULT_OK) {
    return result;
  }

  parcel = std::make_unique<Parcel>();
  if (data) {
    parcel->data.resize(num_data_bytes);
    *data = parcel->data.data();
  }
  return IPCZ_RESULT_OK;
}

IpczResult DirectPortalBackend::CommitPut(
    uint32_t num_data_bytes_produced,
    absl::Span<const IpczHandle> portals,
    absl::Span<const IpczOSHandle> os_handles) {
  IpczResult result = IPCZ_RESULT_OK;
  absl::MutexLock lock(&state_->mutex);

  auto& parcel = this_side().pending_parcel;
  if (!parcel) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  if (parcel->data.size() < num_data_bytes_produced) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  PortalBackend* other_backend = other_side().backend;
  if (!other_backend) {
    return IPCZ_RESULT_NOT_FOUND;
  }

  Portal* other_portal = other_backend->portal();

  parcel->portals.reserve(portals.size());
  for (IpczHandle handle : portals) {
    Portal& portal_to_put = ToPortal(handle);

    // Safety check: we can't put ourself or our peer into our own portal.
    if (&portal_to_put == portal() || &portal_to_put == other_portal) {
      result = IPCZ_RESULT_INVALID_ARGUMENT;
      break;
    }

    parcel->portals.push_back(portal_to_put.TakeBackend());
  }

  if (result != IPCZ_RESULT_OK) {
    // Give the caller their portals back, because we're going to fail.
    for (size_t i = 0; i < parcel->portals.size(); ++i) {
      ToPortal(portals[i]).SetBackend(std::move(parcel->portals[i]));
    }
    parcel->portals.clear();
    return result;
  }

  parcel->data.resize(num_data_bytes_produced);
  parcel->os_handles.reserve(os_handles.size());
  for (const IpczOSHandle& handle : os_handles) {
    parcel->os_handles.push_back(os::Handle::FromIpczOSHandle(handle));
  }

  PortalState& other_state = other_side();
  other_state.num_queued_parcels++;
  other_state.num_queued_data_bytes += parcel->data.size();

  if (other_state.last_incoming_parcel) {
    Parcel* old_tail = other_state.last_incoming_parcel;
    other_state.last_incoming_parcel = parcel.get();
    old_tail->next_parcel = std::move(parcel);
  } else {
    other_state.last_incoming_parcel = parcel.get();
    other_state.incoming_parcels = std::move(parcel);
  }

  return IPCZ_RESULT_OK;
}

IpczResult DirectPortalBackend::AbortPut() {
  absl::MutexLock lock(&state_->mutex);
  auto& parcel = this_side().pending_parcel;
  if (!parcel) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  parcel.reset();
  return IPCZ_RESULT_OK;
}

IpczResult DirectPortalBackend::Get(void* data,
                                    uint32_t* num_data_bytes,
                                    IpczHandle* portals,
                                    uint32_t* num_portals,
                                    IpczOSHandle* os_handles,
                                    uint32_t* num_os_handles) {
  absl::MutexLock lock(&state_->mutex);
  PortalState& state = this_side();
  if (state.in_two_phase_get) {
    return IPCZ_RESULT_ALREADY_EXISTS;
  }

  auto& next_parcel = state.incoming_parcels;
  const bool empty = !next_parcel;
  if (empty) {
    const bool closed = other_side().backend == nullptr;
    if (closed) {
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

  std::unique_ptr<Parcel> parcel = std::move(next_parcel);
  next_parcel = std::move(parcel->next_parcel);
  if (!next_parcel) {
    this_side().last_incoming_parcel = nullptr;
  }

  memcpy(data, parcel->data.data(), parcel->data.size());
  for (size_t i = 0; i < parcel->portals.size(); ++i) {
    auto portal = std::make_unique<Portal>(std::move(parcel->portals[i]));
    portals[i] = ToHandle(portal.release());
  }
  for (size_t i = 0; i < parcel->os_handles.size(); ++i) {
    os::Handle::ToIpczOSHandle(std::move(parcel->os_handles[i]),
                               &os_handles[i]);
  }
  return IPCZ_RESULT_OK;
}

IpczResult DirectPortalBackend::BeginGet(const void** data,
                                         uint32_t* num_data_bytes,
                                         uint32_t* num_portals,
                                         uint32_t* num_os_handles) {
  absl::MutexLock lock(&state_->mutex);
  PortalState& state = this_side();
  if (state.in_two_phase_get) {
    return IPCZ_RESULT_ALREADY_EXISTS;
  }

  auto& next_parcel = state.incoming_parcels;
  const bool empty = !next_parcel;
  if (empty) {
    const bool closed = other_side().backend == nullptr;
    if (closed) {
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

  state.in_two_phase_get = true;
  return IPCZ_RESULT_OK;
}

IpczResult DirectPortalBackend::CommitGet(uint32_t num_data_bytes_consumed,
                                          IpczHandle* portals,
                                          uint32_t* num_portals,
                                          IpczOSHandle* os_handles,
                                          uint32_t* num_os_handles) {
  absl::MutexLock lock(&state_->mutex);
  PortalState& state = this_side();
  if (!state.in_two_phase_get) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  auto& next_parcel = state.incoming_parcels;
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
    next_parcel = std::move(next_parcel->next_parcel);
    if (!next_parcel) {
      state.last_incoming_parcel = nullptr;
    }
  } else {
    next_parcel->data_offset += num_data_bytes_consumed;
  }
  state.in_two_phase_get = false;
  return IPCZ_RESULT_OK;
}

IpczResult DirectPortalBackend::AbortGet() {
  absl::MutexLock lock(&state_->mutex);
  PortalState& state = this_side();
  if (!state.in_two_phase_get) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  state.in_two_phase_get = false;
  return IPCZ_RESULT_OK;
}

IpczResult DirectPortalBackend::CreateMonitor(
    const IpczMonitorDescriptor& descriptor,
    IpczHandle* handle) {
  return IPCZ_RESULT_UNIMPLEMENTED;
}

DirectPortalBackend::PortalState& DirectPortalBackend::this_side() {
  state_->mutex.AssertHeld();
  return state_->sides[side_];
}

DirectPortalBackend::PortalState& DirectPortalBackend::other_side() {
  state_->mutex.AssertHeld();
  return state_->sides[side_ ^ 1];
}

}  // namespace core
}  // namespace ipcz
