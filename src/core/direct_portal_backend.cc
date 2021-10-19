// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/direct_portal_backend.h"

#include <limits>
#include <utility>
#include <vector>

#include "core/node.h"
#include "core/portal_backend.h"
#include "core/trap.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_set.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "util/handle_util.h"

namespace ipcz {
namespace core {

namespace {

IpczHandle ToHandle(Portal* portal) {
  return static_cast<IpczHandle>(reinterpret_cast<uintptr_t>(portal));
}

// A parcel passed between two directly connected portals.
struct Parcel {
  std::vector<uint8_t> data;
  std::vector<mem::Ref<Portal>> portals;
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
  explicit PortalState(Portal& portal) : portal(mem::WrapRefCounted(&portal)) {}

  // A reference back to the Portal who effectively owns this state. Closed if
  // the portal is null.
  mem::Ref<Portal> portal;

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

  // Traps installed on this portal.
  absl::flat_hash_set<std::unique_ptr<Trap>> traps;
};

// State shared between two directly connected portals. Both portals' states are
// hosted here behind a common mutex to simplify synchronization when operating
// on either one.
struct DirectPortalBackend::SharedState : public mem::RefCounted {
  explicit SharedState(Portal& portal0, Portal& portal1)
      : sides{PortalState(portal0), PortalState(portal1)} {}

  absl::Mutex mutex;
  PortalState sides[2] ABSL_GUARDED_BY(mutex);

  void EnqueueParcelFrom(size_t sender_side, std::unique_ptr<Parcel> parcel) {
    mutex.AssertHeld();
    const size_t data_size = parcel->data.size();
    PortalState& receiver = sides[sender_side ^ 1];
    Parcel* prev_tail = receiver.last_incoming_parcel;
    receiver.last_incoming_parcel = parcel.get();
    if (prev_tail) {
      prev_tail->next_parcel = std::move(parcel);
    } else {
      receiver.incoming_parcels = std::move(parcel);
    }

    receiver.num_queued_parcels++;
    receiver.num_queued_data_bytes += data_size;
  }

  std::vector<uint8_t> ConsumeParcelFrom(size_t receiver_side,
                                         IpczHandle* portals,
                                         IpczOSHandle* os_handles) {
    mutex.AssertHeld();
    PortalState& receiver = sides[receiver_side];
    std::unique_ptr<Parcel> parcel = std::move(receiver.incoming_parcels);
    if (!parcel->next_parcel) {
      receiver.last_incoming_parcel = nullptr;
    }
    receiver.incoming_parcels = std::move(parcel->next_parcel);
    ConsumePortalsAndHandles(*parcel, portals, os_handles);

    receiver.num_queued_parcels--;
    receiver.num_queued_data_bytes -= parcel->data.size();

    // TODO: scan sender traps for ones monitoring remote queue shrinkage
    // TODO: scan receiver traps for ones monitoring local queue growth

    return std::move(parcel->data);
  }

  void ConsumePartialParcelFrom(size_t receiver_side,
                                size_t num_bytes_consumed,
                                IpczHandle* portals,
                                IpczOSHandle* os_handles) {
    mutex.AssertHeld();
    PortalState& receiver = sides[receiver_side];

    receiver.num_queued_data_bytes -= num_bytes_consumed;

    Parcel* parcel = receiver.incoming_parcels.get();
    ABSL_ASSERT(parcel);
    parcel->data_offset += num_bytes_consumed;

    ConsumePortalsAndHandles(*parcel, portals, os_handles);

    receiver.num_queued_data_bytes -= num_bytes_consumed;

    // TODO: scan sender traps for ones monitoring remote queue shrinkage
    // TODO: scan receiver traps for ones monitoring local queue growth
  }

 private:
  ~SharedState() override = default;

  void ConsumePortalsAndHandles(Parcel& parcel,
                                IpczHandle* portals,
                                IpczOSHandle* os_handles) {
    mutex.AssertHeld();
    for (size_t i = 0; i < parcel.portals.size(); ++i) {
      portals[i] = ToHandle(parcel.portals[i].release());
    }
    for (size_t i = 0; i < parcel.os_handles.size(); ++i) {
      os::Handle::ToIpczOSHandle(std::move(parcel.os_handles[i]),
                                 &os_handles[i]);
    }
  }
};

DirectPortalBackend::DirectPortalBackend(mem::Ref<SharedState> state,
                                         size_t side)
    : state_(std::move(state)), side_(side) {}

DirectPortalBackend::~DirectPortalBackend() = default;

// static
DirectPortalBackend::Pair DirectPortalBackend::CreatePair(Portal& portal0,
                                                          Portal& portal1) {
  auto state = mem::MakeRefCounted<SharedState>(portal0, portal1);
  std::unique_ptr<DirectPortalBackend> backend0(
      new DirectPortalBackend(state, 0));
  std::unique_ptr<DirectPortalBackend> backend1(
      new DirectPortalBackend(state, 1));
  return {std::move(backend0), std::move(backend1)};
}

bool DirectPortalBackend::CanTravelThroughPortal(Portal& sender) {
  absl::MutexLock lock(&state_->mutex);
  return &sender != this_side().portal && &sender != other_side().portal;
}

IpczResult DirectPortalBackend::Close(
    std::vector<mem::Ref<Portal>>& other_portals_to_close) {
  absl::MutexLock lock(&state_->mutex);
  PortalState& state = this_side();
  state.portal.reset();
  state.last_incoming_parcel = nullptr;
  state.num_queued_data_bytes = 0;
  state.num_queued_parcels = 0;

  std::unique_ptr<Parcel> parcel = std::move(state.incoming_parcels);
  while (parcel) {
    for (mem::Ref<Portal>& portal : parcel->portals) {
      other_portals_to_close.emplace_back(std::move(portal));
    }
    parcel = std::move(parcel->next_parcel);
  }

  // TODO: scan remote traps for ones monitoring peer closure or portal death

  return IPCZ_RESULT_OK;
}

IpczResult DirectPortalBackend::QueryStatus(IpczPortalStatus& status) {
  absl::MutexLock lock(&state_->mutex);
  status.flags = other_side().portal ? 0 : IPCZ_PORTAL_STATUS_PEER_CLOSED;
  status.num_local_parcels = this_side().num_queued_parcels;
  status.num_local_bytes = this_side().num_queued_data_bytes;
  status.num_remote_parcels = other_side().num_queued_parcels;
  status.num_remote_bytes = other_side().num_queued_data_bytes;
  return IPCZ_RESULT_OK;
}

IpczResult DirectPortalBackend::Put(Node::LockedRouter& router,
                                    absl::Span<const uint8_t> data,
                                    absl::Span<const IpczHandle> portals,
                                    absl::Span<const IpczOSHandle> os_handles,
                                    const IpczPutLimits* limits) {
  absl::MutexLock lock(&state_->mutex);
  if (!other_side().portal) {
    return IPCZ_RESULT_NOT_FOUND;
  }

  PortalState& state = this_side();
  if (state.pending_parcel) {
    return IPCZ_RESULT_ALREADY_EXISTS;
  }

  auto parcel = std::make_unique<Parcel>();

  PortalState& other_state = other_side();
  if (limits) {
    if (limits->max_queued_parcels > 0 &&
        other_state.num_queued_parcels >= limits->max_queued_parcels) {
      return IPCZ_RESULT_RESOURCE_EXHAUSTED;
    } else if (limits->max_queued_bytes > 0 &&
               other_state.num_queued_data_bytes + data.size() >
                   limits->max_queued_bytes) {
      return IPCZ_RESULT_RESOURCE_EXHAUSTED;
    }
  }

  // At this point success is inevitable, so we can take full ownership of any
  // passed resources. This also means destroying any attached portals, as we've
  // already taken ownership of their backends and their handles must no longer
  // be in use.

  parcel->portals.reserve(portals.size());
  for (IpczHandle portal : portals) {
    // Assume ownership of the IpczHandle's implicit ref since the handle is no
    // longer in use.
    parcel->portals.emplace_back(mem::RefCounted::kAdoptExistingRef,
                                 ToPtr<Portal>(portal));
  }

  parcel->data = std::vector<uint8_t>(data.begin(), data.end());
  parcel->os_handles.reserve(os_handles.size());
  for (const IpczOSHandle& handle : os_handles) {
    parcel->os_handles.push_back(os::Handle::FromIpczOSHandle(handle));
  }

  state_->EnqueueParcelFrom(side_, std::move(parcel));
  return IPCZ_RESULT_OK;
}

IpczResult DirectPortalBackend::BeginPut(IpczBeginPutFlags flags,
                                         const IpczPutLimits* limits,
                                         uint32_t& num_data_bytes,
                                         void** data) {
  absl::MutexLock lock(&state_->mutex);
  PortalState& other_state = other_side();
  if (!other_state.portal) {
    return IPCZ_RESULT_NOT_FOUND;
  }

  auto& parcel = this_side().pending_parcel;
  if (parcel) {
    return IPCZ_RESULT_ALREADY_EXISTS;
  }

  if (limits) {
    if (limits->max_queued_parcels > 0 &&
        other_state.num_queued_parcels >= limits->max_queued_parcels) {
      return IPCZ_RESULT_RESOURCE_EXHAUSTED;
    } else if (limits->max_queued_bytes > 0 &&
               other_state.num_queued_data_bytes + num_data_bytes >
                   limits->max_queued_bytes) {
      if (flags & IPCZ_BEGIN_PUT_ALLOW_PARTIAL &&
          other_state.num_queued_data_bytes < limits->max_queued_bytes) {
        num_data_bytes =
            limits->max_queued_bytes - other_state.num_queued_data_bytes;
      } else {
        return IPCZ_RESULT_RESOURCE_EXHAUSTED;
      }
    }
  }

  parcel = std::make_unique<Parcel>();
  if (data) {
    parcel->data.resize(num_data_bytes);
    *data = parcel->data.data();
  }
  return IPCZ_RESULT_OK;
}

IpczResult DirectPortalBackend::CommitPut(
    Node::LockedRouter& router,
    uint32_t num_data_bytes_produced,
    absl::Span<const IpczHandle> portals,
    absl::Span<const IpczOSHandle> os_handles) {
  absl::MutexLock lock(&state_->mutex);

  auto& parcel = this_side().pending_parcel;
  if (!parcel) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  if (parcel->data.size() < num_data_bytes_produced) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (!other_side().portal) {
    return IPCZ_RESULT_NOT_FOUND;
  }

  parcel->portals.reserve(portals.size());
  for (IpczHandle portal : portals) {
    // Assume ownership of the IpczHandle's implicit ref since the handle is no
    // longer in use.
    parcel->portals.emplace_back(mem::RefCounted::kAdoptExistingRef,
                                 ToPtr<Portal>(portal));
  }

  parcel->data.resize(num_data_bytes_produced);
  parcel->os_handles.reserve(os_handles.size());
  for (const IpczOSHandle& handle : os_handles) {
    parcel->os_handles.push_back(os::Handle::FromIpczOSHandle(handle));
  }

  state_->EnqueueParcelFrom(side_, std::move(parcel));
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
    const bool closed = !other_side().portal;
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

  std::vector<uint8_t> parcel_data =
      state_->ConsumeParcelFrom(side_, portals, os_handles);
  memcpy(data, parcel_data.data(), parcel_data.size());
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
    const bool closed = !other_side().portal;
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
    state_->ConsumeParcelFrom(side_, portals, os_handles);
  } else {
    state_->ConsumePartialParcelFrom(side_, num_data_bytes_consumed, portals,
                                     os_handles);
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

DirectPortalBackend::PortalState& DirectPortalBackend::this_side() {
  state_->mutex.AssertHeld();
  return state_->sides[side_];
}

DirectPortalBackend::PortalState& DirectPortalBackend::other_side() {
  state_->mutex.AssertHeld();
  return state_->sides[side_ ^ 1];
}

IpczResult DirectPortalBackend::AddTrap(std::unique_ptr<Trap> trap) {
  absl::MutexLock lock(&state_->mutex);
  this_side().traps.emplace(std::move(trap));
  return IPCZ_RESULT_OK;
}

IpczResult DirectPortalBackend::ArmTrap(
    Trap& trap,
    IpczTrapConditions* satisfied_conditions,
    IpczPortalStatus* status) {
  absl::MutexLock lock(&state_->mutex);
  if (this_side().traps.find(&trap) == this_side().traps.end()) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  // TODO
  return IPCZ_RESULT_OK;
}

IpczResult DirectPortalBackend::RemoveTrap(Trap& trap) {
  absl::MutexLock lock(&state_->mutex);
  if (this_side().traps.erase(&trap) == 0) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  return IPCZ_RESULT_OK;
}

}  // namespace core
}  // namespace ipcz
