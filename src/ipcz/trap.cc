// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/trap.h"

#include <cstddef>
#include <cstring>

#include "ipcz/portal.h"
#include "ipcz/router.h"
#include "ipcz/trap_event_dispatcher.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"

namespace ipcz {

Trap::Trap(Ref<Portal> portal,
           const IpczTrapConditions& conditions,
           IpczTrapEventHandler handler,
           uint64_t context)
    : APIObject(kTrap),
      portal_(std::move(portal)),
      conditions_(conditions),
      handler_(handler),
      context_(context) {}

Trap::~Trap() = default;

IpczResult Trap::Close() {
  Disable();
  return IPCZ_RESULT_OK;
}

IpczResult Trap::Arm(IpczTrapConditionFlags* satisfied_condition_flags,
                     IpczPortalStatus* status) {
  Router::Locked locked_router(*portal_->router());
  absl::MutexLock lock(&mutex_);
  if (!is_enabled_) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (is_armed_) {
    return IPCZ_RESULT_ALREADY_EXISTS;
  }

  const IpczPortalStatus& current_status = locked_router.status();
  const IpczTrapConditionFlags flags =
      GetEventFlags(current_status, UpdateReason::kStatusQuery);
  if (flags != 0) {
    if (status) {
      const size_t size = std::min(current_status.size, status->size);
      memcpy(status, &current_status, size);
      status->size = size;
    }
    if (satisfied_condition_flags) {
      *satisfied_condition_flags = flags;
    }
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  is_armed_ = true;
  return IPCZ_RESULT_OK;
}

void Trap::Disable() {
  absl::MutexLock lock(&mutex_);
  is_enabled_ = false;
}

void Trap::UpdatePortalStatus(const IpczPortalStatus& status,
                              UpdateReason reason,
                              TrapEventDispatcher& dispatcher) {
  absl::MutexLock lock(&mutex_);
  if (!is_enabled_ || !is_armed_) {
    return;
  }

  const IpczTrapConditionFlags event_flags = GetEventFlags(status, reason);
  if (event_flags != 0) {
    is_armed_ = false;
    dispatcher.DeferEvent(WrapRefCounted(this), event_flags, status);
  }
}

IpczTrapConditionFlags Trap::GetEventFlags(const IpczPortalStatus& status,
                                           UpdateReason reason) {
  IpczTrapConditionFlags event_flags = 0;
  if ((conditions_.flags & IPCZ_TRAP_PEER_CLOSED) &&
      (status.flags & IPCZ_PORTAL_STATUS_PEER_CLOSED)) {
    event_flags |= IPCZ_TRAP_PEER_CLOSED;
  }
  if ((conditions_.flags & IPCZ_TRAP_DEAD) &&
      (status.flags & IPCZ_PORTAL_STATUS_DEAD)) {
    event_flags |= IPCZ_TRAP_DEAD;
  }
  if ((conditions_.flags & IPCZ_TRAP_ABOVE_MIN_LOCAL_PARCELS) &&
      status.num_local_parcels > conditions_.min_local_parcels) {
    event_flags |= IPCZ_TRAP_ABOVE_MIN_LOCAL_PARCELS;
  }
  if ((conditions_.flags & IPCZ_TRAP_ABOVE_MIN_LOCAL_BYTES) &&
      status.num_local_bytes > conditions_.min_local_bytes) {
    event_flags |= IPCZ_TRAP_ABOVE_MIN_LOCAL_BYTES;
  }
  if ((conditions_.flags & IPCZ_TRAP_BELOW_MAX_REMOTE_PARCELS) &&
      status.num_remote_parcels < conditions_.max_remote_parcels) {
    event_flags |= IPCZ_TRAP_BELOW_MAX_REMOTE_PARCELS;
  }
  if ((conditions_.flags & IPCZ_TRAP_BELOW_MAX_REMOTE_BYTES) &&
      status.num_remote_bytes < conditions_.max_remote_bytes) {
    event_flags |= IPCZ_TRAP_BELOW_MAX_REMOTE_BYTES;
  }
  if (reason == UpdateReason::kParcelReceived &&
      (conditions_.flags & IPCZ_TRAP_NEW_LOCAL_PARCEL)) {
    event_flags |= IPCZ_TRAP_NEW_LOCAL_PARCEL;
  }
  return event_flags;
}

void Trap::MaybeDispatchEvent(IpczTrapConditionFlags condition_flags,
                              const IpczPortalStatus& status) {
  {
    absl::MutexLock lock(&mutex_);
    if (!is_enabled_) {
      return;
    }
  }

  IpczTrapEvent event = {sizeof(event)};
  event.context = context_;
  event.condition_flags = condition_flags;
  event.status = &status;
  handler_(&event);
}

}  // namespace ipcz
