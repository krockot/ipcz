// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/direct_portal_backend.h"

#include <limits>
#include <utility>
#include <vector>

#include "core/buffering_portal_backend.h"
#include "core/node.h"
#include "core/parcel.h"
#include "core/parcel_queue.h"
#include "core/portal_backend.h"
#include "core/portal_backend_state.h"
#include "core/side.h"
#include "core/trap.h"
#include "debug/log.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_set.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "util/handle_util.h"

namespace ipcz {
namespace core {

// State shared between two directly connected portals. Both portals' states are
// hosted here behind a common mutex to simplify synchronization when operating
// on either one.
struct DirectPortalBackend::SharedState : public mem::RefCounted {
  explicit SharedState(Portal& portal0, Portal& portal1)
      : portals{mem::WrapRefCounted(&portal0), mem::WrapRefCounted(&portal1)} {}

  absl::Mutex mutex;
  TwoSided<mem::Ref<Portal>> portals ABSL_GUARDED_BY(mutex);
  TwoSided<PortalBackendState> state ABSL_GUARDED_BY(mutex);

 private:
  ~SharedState() override = default;
};

DirectPortalBackend::DirectPortalBackend(mem::Ref<SharedState> state, Side side)
    : state_(std::move(state)), side_(side) {}

DirectPortalBackend::~DirectPortalBackend() = default;

// static
DirectPortalBackend::Pair DirectPortalBackend::CreatePair(Portal& portal0,
                                                          Portal& portal1) {
  auto state = mem::MakeRefCounted<SharedState>(portal0, portal1);
  std::unique_ptr<DirectPortalBackend> backend0(
      new DirectPortalBackend(state, Side::kLeft));
  std::unique_ptr<DirectPortalBackend> backend1(
      new DirectPortalBackend(state, Side::kRight));
  return {std::move(backend0), std::move(backend1)};
}

mem::Ref<Portal> DirectPortalBackend::GetLocalPeer() {
  absl::MutexLock lock(&state_->mutex);
  return other_portal();
}

// static
std::pair<std::unique_ptr<BufferingPortalBackend>,
          std::unique_ptr<BufferingPortalBackend>>
DirectPortalBackend::Split(DirectPortalBackend& backend,
                           DirectPortalBackend* peer_backend) {
  // They must indeed be peers.
  ABSL_ASSERT(!peer_backend || peer_backend->state_ == backend.state_);

  PortalBackendState state;
  PortalBackendState peer_state;
  {
    absl::MutexLock lock(&backend.state_->mutex);
    state = std::move(backend.state_->state[backend.side_]);
    peer_state = std::move(backend.state_->state[Opposite(backend.side_)]);
  }

  auto new_backend = std::make_unique<BufferingPortalBackend>(backend.side_);
  {
    absl::MutexLock lock(&new_backend->mutex_);
    new_backend->state_ = std::move(state);
    new_backend->routed_name_ = PortalName(PortalName::kRandom);
  }
  std::unique_ptr<BufferingPortalBackend> new_peer_backend;
  if (peer_backend) {
    new_peer_backend =
        std::make_unique<BufferingPortalBackend>(peer_backend->side_);
    absl::MutexLock lock(&new_peer_backend->mutex_);
    new_peer_backend->state_ = std::move(peer_state);
    new_peer_backend->routed_name_ = PortalName(PortalName::kRandom);
  }

  return {std::move(new_backend), std::move(new_peer_backend)};
}

PortalBackend::Type DirectPortalBackend::GetType() const {
  return Type::kDirect;
}

bool DirectPortalBackend::CanTravelThroughPortal(Portal& sender) {
  absl::MutexLock lock(&state_->mutex);
  return &sender != other_portal();
}

void DirectPortalBackend::PrepareForTravel(PortalInTransit& portal_in_transit) {
  // We should never call PrepareForTravel on a DirectPortalBackend.
  ABSL_ASSERT(false);
}

bool DirectPortalBackend::AcceptParcel(Parcel& parcel,
                                       TrapEventDispatcher& dispatcher) {
  // Parcels are only exchanged directly between local DirectPortalBackends, so
  // it's absurd for this method to ever be invoked.
  ABSL_ASSERT(false);
  return false;
}

bool DirectPortalBackend::NotifyPeerClosed(TrapEventDispatcher& dispatcher) {
  ABSL_ASSERT(false);
  return false;
}

IpczResult DirectPortalBackend::Close(
    std::vector<mem::Ref<Portal>>& other_portals_to_close) {
  absl::MutexLock lock(&state_->mutex);
  PortalBackendState& state = this_side();
  state.closed = true;
  this_portal().reset();

  IpczPortalStatus& other_status = other_side().status;
  other_status.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
  other_status.num_remote_bytes = 0;
  other_status.num_remote_parcels = 0;

  for (Parcel& parcel : state.incoming_parcels.TakeParcels()) {
    for (PortalInTransit& portal : parcel.TakePortals()) {
      ABSL_ASSERT(portal.portal);
      other_portals_to_close.emplace_back(std::move(portal.portal));
    }
  }

  // TODO: scan remote traps for ones monitoring peer closure or portal death

  return IPCZ_RESULT_OK;
}

IpczResult DirectPortalBackend::QueryStatus(IpczPortalStatus& status) {
  absl::MutexLock lock(&state_->mutex);
  status.flags = this_side().status.flags;
  status.num_local_bytes = this_side().status.num_local_bytes;
  status.num_local_parcels = this_side().status.num_local_parcels;
  status.num_remote_bytes = other_side().status.num_local_bytes;
  status.num_remote_parcels = other_side().status.num_local_parcels;
  return IPCZ_RESULT_OK;
}

IpczResult DirectPortalBackend::Put(absl::Span<const uint8_t> data,
                                    absl::Span<PortalInTransit> portals,
                                    absl::Span<const IpczOSHandle> os_handles,
                                    const IpczPutLimits* limits) {
  absl::MutexLock lock(&state_->mutex);
  PortalBackendState& state = this_side();
  PortalBackendState& other_state = other_side();
  if (other_state.closed) {
    return IPCZ_RESULT_NOT_FOUND;
  }

  if (state.pending_parcel) {
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

  if (max_queued_parcels > 0 &&
      other_state.status.num_local_parcels >= max_queued_parcels) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  } else if (max_queued_bytes <= other_state.status.num_local_bytes) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  } else if (max_queued_bytes - other_state.status.num_local_bytes <
             data.size()) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  }

  // At this point success is inevitable, so we can take full ownership of any
  // passed resources. This also means destroying any attached portals, as we've
  // already taken ownership of their backends and their handles must no longer
  // be in use.

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
  other_state.incoming_parcels.push(std::move(parcel));
  ++other_state.status.num_local_parcels;
  other_state.status.num_local_bytes += data.size();
  return IPCZ_RESULT_OK;
}

IpczResult DirectPortalBackend::BeginPut(IpczBeginPutFlags flags,
                                         const IpczPutLimits* limits,
                                         uint32_t& num_data_bytes,
                                         void** data) {
  absl::MutexLock lock(&state_->mutex);
  PortalBackendState& other_state = other_side();
  if (other_state.closed) {
    return IPCZ_RESULT_NOT_FOUND;
  }

  absl::optional<Parcel>& parcel = this_side().pending_parcel;
  if (parcel) {
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

  if (other_state.status.num_local_parcels >= max_queued_parcels) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  } else if (max_queued_bytes <= other_state.status.num_local_bytes) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  } else if (max_queued_bytes - other_state.status.num_local_bytes <
             num_data_bytes) {
    if (flags & IPCZ_BEGIN_PUT_ALLOW_PARTIAL) {
      num_data_bytes = max_queued_bytes - other_state.status.num_local_bytes;
    } else {
      return IPCZ_RESULT_RESOURCE_EXHAUSTED;
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
    uint32_t num_data_bytes_produced,
    absl::Span<PortalInTransit> portals,
    absl::Span<const IpczOSHandle> os_handles) {
  absl::MutexLock lock(&state_->mutex);

  PortalBackendState& state = this_side();
  absl::optional<Parcel>& parcel = state.pending_parcel;
  if (!parcel) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  if (parcel->data_view().size() < num_data_bytes_produced) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  PortalBackendState& other_state = other_side();
  if (other_state.closed) {
    return IPCZ_RESULT_NOT_FOUND;
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

  parcel->ResizeData(num_data_bytes_produced);
  parcel->SetPortals(std::move(parcel_portals));
  parcel->SetOSHandles(std::move(parcel_os_handles));
  other_state.incoming_parcels.push(std::move(*parcel));
  ++other_state.status.num_local_parcels;
  other_state.status.num_local_bytes += num_data_bytes_produced;
  state.pending_parcel.reset();
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
  PortalBackendState& state = this_side();
  if (state.in_two_phase_get) {
    return IPCZ_RESULT_ALREADY_EXISTS;
  }

  PortalBackendState& other_state = other_side();
  if (state.incoming_parcels.empty()) {
    if (other_state.closed) {
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
  state.status.num_local_bytes -= parcel.data_view().size();
  --state.status.num_local_parcels;

  memcpy(data, parcel.data_view().data(), parcel.data_view().size());
  parcel.Consume(portals, os_handles);
  return IPCZ_RESULT_OK;
}

IpczResult DirectPortalBackend::BeginGet(const void** data,
                                         uint32_t* num_data_bytes,
                                         uint32_t* num_portals,
                                         uint32_t* num_os_handles) {
  absl::MutexLock lock(&state_->mutex);
  PortalBackendState& state = this_side();
  if (state.in_two_phase_get) {
    return IPCZ_RESULT_ALREADY_EXISTS;
  }

  if (state.incoming_parcels.empty()) {
    if (other_side().closed) {
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
  PortalBackendState& state = this_side();
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
    state.status.num_local_bytes -= parcel.data_view().size();
    --state.status.num_local_parcels;
    parcel.Consume(portals, os_handles);
  } else {
    Parcel& parcel = state.incoming_parcels.front();
    state.status.num_local_bytes -= num_data_bytes_consumed;
    parcel.ConsumePartial(num_data_bytes_consumed, portals, os_handles);
  }
  state.in_two_phase_get = false;
  return IPCZ_RESULT_OK;
}

IpczResult DirectPortalBackend::AbortGet() {
  absl::MutexLock lock(&state_->mutex);
  PortalBackendState& state = this_side();
  if (!state.in_two_phase_get) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  state.in_two_phase_get = false;
  return IPCZ_RESULT_OK;
}

PortalBackendState& DirectPortalBackend::this_side() {
  state_->mutex.AssertHeld();
  return state_->state[side_];
}

PortalBackendState& DirectPortalBackend::other_side() {
  state_->mutex.AssertHeld();
  return state_->state[Opposite(side_)];
}

mem::Ref<Portal>& DirectPortalBackend::this_portal() {
  state_->mutex.AssertHeld();
  return state_->portals[side_];
}

mem::Ref<Portal>& DirectPortalBackend::other_portal() {
  state_->mutex.AssertHeld();
  return state_->portals[Opposite(side_)];
}

IpczResult DirectPortalBackend::AddTrap(std::unique_ptr<Trap> trap) {
  absl::MutexLock lock(&state_->mutex);
  this_side().traps.Add(std::move(trap));
  return IPCZ_RESULT_OK;
}

IpczResult DirectPortalBackend::ArmTrap(
    Trap& trap,
    IpczTrapConditionFlags* satisfied_condition_flags,
    IpczPortalStatus* status) {
  absl::MutexLock lock(&state_->mutex);
  // TODO
  return IPCZ_RESULT_OK;
}

IpczResult DirectPortalBackend::RemoveTrap(Trap& trap) {
  absl::MutexLock lock(&state_->mutex);
  return this_side().traps.Remove(trap);
}

}  // namespace core
}  // namespace ipcz
