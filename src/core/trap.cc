// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/trap.h"

#include "core/trap_event_dispatcher.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"

namespace ipcz {
namespace core {

Trap::SharedState::SharedState() = default;

Trap::SharedState::~SharedState() = default;

bool Trap::SharedState::DisarmIfArmed() {
  absl::MutexLock lock(&mutex_);
  bool was_armed = is_armed_;
  is_armed_ = false;
  return was_armed;
}

Trap::Trap(const IpczTrapConditions& conditions,
           IpczTrapEventHandler handler,
           uintptr_t context)
    : conditions_(conditions), handler_(handler), context_(context) {}

Trap::~Trap() {
  absl::MutexLock lock(&state_->mutex_);
  state_->is_armed_ = false;
}

IpczResult Trap::Arm(const IpczPortalStatus& status,
                     IpczTrapConditionFlags& flags) {
  absl::MutexLock lock(&state_->mutex_);
  if (state_->is_armed_) {
    return IPCZ_RESULT_ALREADY_EXISTS;
  }

  flags = GetEventFlags(status);
  if (flags != 0) {
    return IPCZ_RESULT_FAILED_PRECONDITION;
  }

  state_->is_armed_ = true;
  return IPCZ_RESULT_OK;
}

void Trap::MaybeNotify(TrapEventDispatcher& dispatcher,
                       const IpczPortalStatus& status) {
  absl::MutexLock lock(&state_->mutex_);
  if (!state_->is_armed_) {
    return;
  }

  const IpczTrapConditionFlags event_flags = GetEventFlags(status);
  if (event_flags != 0) {
    dispatcher.DeferEvent(handler_, context_, event_flags, status, *state_);
  }
}

void Trap::MaybeNotifyDestroyed(TrapEventDispatcher& dispatcher,
                                const IpczPortalStatus& status) {
  if (conditions_.flags & IPCZ_TRAP_CONDITION_DESTROYED) {
    dispatcher.DeferEvent(handler_, context_, IPCZ_TRAP_CONDITION_DESTROYED,
                          status, *state_);
  }
}

IpczTrapConditionFlags Trap::GetEventFlags(const IpczPortalStatus& status) {
  IpczTrapConditionFlags event_flags = 0;
  if ((conditions_.flags & IPCZ_TRAP_CONDITION_PEER_CLOSED) &&
      (status.flags & IPCZ_PORTAL_STATUS_PEER_CLOSED)) {
    event_flags |= IPCZ_TRAP_CONDITION_PEER_CLOSED;
  }
  if ((conditions_.flags & IPCZ_TRAP_CONDITION_DEAD) &&
      (status.flags & IPCZ_PORTAL_STATUS_PEER_CLOSED) &&
      status.num_local_parcels == 0) {
    event_flags |= IPCZ_TRAP_CONDITION_DEAD;
  }
  if ((conditions_.flags & IPCZ_TRAP_CONDITION_LOCAL_PARCELS) &&
      status.num_local_parcels >= conditions_.min_local_parcels) {
    event_flags |= IPCZ_TRAP_CONDITION_LOCAL_PARCELS;
  }
  if ((conditions_.flags & IPCZ_TRAP_CONDITION_LOCAL_BYTES) &&
      status.num_local_bytes >= conditions_.min_local_bytes) {
    event_flags |= IPCZ_TRAP_CONDITION_LOCAL_BYTES;
  }
  if ((conditions_.flags & IPCZ_TRAP_CONDITION_REMOTE_PARCELS) &&
      status.num_remote_parcels < conditions_.max_remote_parcels) {
    event_flags |= IPCZ_TRAP_CONDITION_REMOTE_PARCELS;
  }
  if ((conditions_.flags & IPCZ_TRAP_CONDITION_REMOTE_BYTES) &&
      status.num_remote_bytes < conditions_.max_remote_bytes) {
    event_flags |= IPCZ_TRAP_CONDITION_REMOTE_BYTES;
  }
  return event_flags;
}

TrapSet::TrapSet() = default;

TrapSet::~TrapSet() = default;

IpczResult TrapSet::Add(std::unique_ptr<Trap> trap) {
  traps_.insert(std::move(trap));
  return IPCZ_RESULT_OK;
}

IpczResult TrapSet::Remove(Trap& trap) {
  auto it = traps_.find(&trap);
  if (it == traps_.end()) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  traps_.erase(it);
  return IPCZ_RESULT_OK;
}

void TrapSet::MaybeNotify(TrapEventDispatcher& dispatcher,
                          const IpczPortalStatus& status) {
  for (const auto& trap : traps_) {
    trap->MaybeNotify(dispatcher, status);
  }
}

}  // namespace core
}  // namespace ipcz
