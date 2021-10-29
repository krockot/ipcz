// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_TRAP_EVENT_DISPATCHER_H_
#define IPCZ_SRC_CORE_TRAP_EVENT_DISPATCHER_H_

#include <atomic>
#include <memory>

#include "core/trap.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"

namespace ipcz {
namespace core {

// Accumulates IpczTrapEvent dispatches to specific handlers. Handler invocation
// is deferred until DispatchAll() is called or the TrapEventDispatcher is
// destroyed. This allows event dispatches to be accumulated while e.g. Node and
// Portal locks are held, and dispatched later, once such locks are released.
//
// This object is not thread-safe but is generally constructed on the stack and
// passed into whatever might want to accumulate events for dispatch.
class TrapEventDispatcher {
 public:
  TrapEventDispatcher();
  ~TrapEventDispatcher();

  void DeferEvent(const Trap& trap,
                  const IpczTrapConditionFlags condition_flags,
                  const IpczPortalStatus& status);

  void DispatchAll();

 private:
  struct Event {
    Event();
    Event(const Event&) = delete;
    Event(Event&&);
    Event& operator=(const Event&) = delete;
    Event& operator=(Event&&);
    ~Event();

    std::shared_ptr<std::atomic_bool> is_trap_armed;
    IpczTrapEventHandler handler;
    uintptr_t context;
    IpczTrapConditionFlags condition_flags;
    IpczPortalStatus status;
  };

  absl::InlinedVector<Event, 8> events_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_TRAP_EVENT_DISPATCHER_H_
