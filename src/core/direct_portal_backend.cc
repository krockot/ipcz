// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/direct_portal_backend.h"

#include <limits>
#include <utility>
#include <vector>

#include "core/node.h"
#include "core/parcel.h"
#include "core/parcel_queue.h"
#include "core/portal_backend.h"
#include "core/trap.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_set.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "util/handle_util.h"

namespace ipcz {
namespace core {

// State for one side of a directly connected portal pair.
struct DirectPortalBackend::PortalState {
  explicit PortalState(Portal& portal) : portal(mem::WrapRefCounted(&portal)) {}

  // A reference back to the Portal who effectively owns this state. Closed if
  // the portal is null.
  mem::Ref<Portal> portal;

  // Incoming parcel queue; messages from the other side are placed here
  // directly.
  ParcelQueue incoming_parcels;

  // Counters exposed by QueryStatus and used to enforce limits during put
  // operations.
  uint64_t num_queued_data_bytes = 0;

  // Indicates whether a two-phase get is in progress.
  bool in_two_phase_get = false;

  // A pending parcel for an in-progress two-phase put.
  absl::optional<Parcel> pending_parcel;

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

 private:
  ~SharedState() override = default;
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

PortalBackend::Type DirectPortalBackend::GetType() const {
  return Type::kDirect;
}

bool DirectPortalBackend::CanTravelThroughPortal(Portal& sender) {
  absl::MutexLock lock(&state_->mutex);
  return &sender != other_side().portal;
}

IpczResult DirectPortalBackend::Close(
    Node::LockedRouter& router,
    std::vector<mem::Ref<Portal>>& other_portals_to_close) {
  absl::MutexLock lock(&state_->mutex);
  PortalState& state = this_side();
  state.portal.reset();
  state.num_queued_data_bytes = 0;

  for (Parcel& parcel : state.incoming_parcels.TakeParcels()) {
    for (mem::Ref<Portal>& portal : parcel.TakePortals()) {
      other_portals_to_close.emplace_back(std::move(portal));
    }
  }

  // TODO: scan remote traps for ones monitoring peer closure or portal death

  return IPCZ_RESULT_OK;
}

IpczResult DirectPortalBackend::QueryStatus(IpczPortalStatus& status) {
  absl::MutexLock lock(&state_->mutex);
  status.flags = other_side().portal ? 0 : IPCZ_PORTAL_STATUS_PEER_CLOSED;
  status.num_local_parcels = this_side().incoming_parcels.size();
  status.num_local_bytes = this_side().num_queued_data_bytes;
  status.num_remote_parcels = other_side().incoming_parcels.size();
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

  PortalState& other_state = other_side();
  if (limits) {
    if (limits->max_queued_parcels > 0 &&
        other_state.incoming_parcels.size() >= limits->max_queued_parcels) {
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

  std::vector<mem::Ref<Portal>> parcel_portals;
  parcel_portals.reserve(portals.size());
  for (IpczHandle portal : portals) {
    // Assume ownership of the IpczHandle's implicit ref since the handle is no
    // longer in use.
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

  PortalState& receiver = state_->sides[side_ ^ 1];
  receiver.num_queued_data_bytes += data.size();
  receiver.incoming_parcels.push(std::move(parcel));
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

  absl::optional<Parcel>& parcel = this_side().pending_parcel;
  if (parcel) {
    return IPCZ_RESULT_ALREADY_EXISTS;
  }

  if (limits) {
    if (limits->max_queued_parcels > 0 &&
        other_state.incoming_parcels.size() >= limits->max_queued_parcels) {
      return IPCZ_RESULT_RESOURCE_EXHAUSTED;
    } else if (limits->max_queued_bytes > 0 &&
               other_state.num_queued_data_bytes + num_data_bytes >
                   limits->max_queued_bytes) {
      if ((flags & IPCZ_BEGIN_PUT_ALLOW_PARTIAL) &&
          other_state.num_queued_data_bytes < limits->max_queued_bytes) {
        num_data_bytes =
            limits->max_queued_bytes - other_state.num_queued_data_bytes;
      } else {
        return IPCZ_RESULT_RESOURCE_EXHAUSTED;
      }
    }
  }

  parcel.emplace();
  if (data) {
    parcel->SetData(std::vector<uint8_t>(num_data_bytes));
    *data = parcel->data_view().data();
  }
  return IPCZ_RESULT_OK;
}

IpczResult DirectPortalBackend::CommitPut(
    Node::LockedRouter& router,
    uint32_t num_data_bytes_produced,
    absl::Span<const IpczHandle> portals,
    absl::Span<const IpczOSHandle> os_handles) {
  absl::MutexLock lock(&state_->mutex);

  absl::optional<Parcel>& parcel = this_side().pending_parcel;
  if (!parcel) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  if (parcel->data_view().size() < num_data_bytes_produced) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (!other_side().portal) {
    return IPCZ_RESULT_NOT_FOUND;
  }

  std::vector<mem::Ref<Portal>> parcel_portals;
  parcel_portals.reserve(portals.size());
  for (IpczHandle portal : portals) {
    // Assume ownership of the IpczHandle's implicit ref since the handle is no
    // longer in use.
    parcel_portals.emplace_back(mem::RefCounted::kAdoptExistingRef,
                                ToPtr<Portal>(portal));
  }

  std::vector<os::Handle> parcel_os_handles;
  parcel_os_handles.reserve(os_handles.size());
  for (const IpczOSHandle& handle : os_handles) {
    parcel_os_handles.push_back(os::Handle::FromIpczOSHandle(handle));
  }

  parcel->ResizeData(num_data_bytes_produced);
  parcel->SetPortals(std::move(parcel_portals));
  parcel->SetOSHandles(std::move(parcel_os_handles));

  PortalState& receiver = state_->sides[side_ ^ 1];
  receiver.num_queued_data_bytes += num_data_bytes_produced;
  receiver.incoming_parcels.push(std::move(*parcel));
  return IPCZ_RESULT_OK;
}

IpczResult DirectPortalBackend::AbortPut() {
  absl::MutexLock lock(&state_->mutex);
  absl::optional<Parcel>& parcel = this_side().pending_parcel;
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

  if (state.incoming_parcels.empty()) {
    const bool closed = !other_side().portal;
    if (closed) {
      return IPCZ_RESULT_NOT_FOUND;
    }
    return IPCZ_RESULT_UNAVAILABLE;
  }

  Parcel& next_parcel = state.incoming_parcels.front();
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

  // TODO: scan sender traps for ones monitoring remote queue shrinkage
  // TODO: scan receiver traps for ones monitoring local queue growth

  Parcel parcel = state.incoming_parcels.pop();
  state.num_queued_data_bytes -= parcel.data_view().size();

  memcpy(data, parcel.data_view().data(), parcel.data_view().size());
  parcel.Consume(portals, os_handles);
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

  if (state.incoming_parcels.empty()) {
    const bool closed = !other_side().portal;
    if (closed) {
      return IPCZ_RESULT_NOT_FOUND;
    }
    return IPCZ_RESULT_UNAVAILABLE;
  }

  Parcel& next_parcel = state.incoming_parcels.front();
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

  Parcel& next_parcel = state.incoming_parcels.front();
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
    Parcel parcel = state.incoming_parcels.pop();
    state.num_queued_data_bytes -= parcel.data_view().size();
    parcel.Consume(portals, os_handles);
  } else {
    Parcel& parcel = state.incoming_parcels.front();
    state.num_queued_data_bytes -= num_data_bytes_consumed;
    parcel.ConsumePartial(num_data_bytes_consumed, portals, os_handles);
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
