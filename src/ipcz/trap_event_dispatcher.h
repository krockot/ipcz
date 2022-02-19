// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_TRAP_EVENT_DISPATCHER_H_
#define IPCZ_SRC_IPCZ_TRAP_EVENT_DISPATCHER_H_

#include <cstdint>

#include "ipcz/ipcz.h"
#include "ipcz/trap.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "util/ref_counted.h"

namespace ipcz {

class Trap;

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

  void DeferEvent(Ref<Trap> trap,
                  const IpczTrapConditionFlags condition_flags,
                  const IpczPortalStatus& status);

  void DispatchAll();

 private:
  struct Event {
    Event();
    Event(const Event&);
    Event& operator=(const Event&);
    ~Event();

    Ref<Trap> trap;
    IpczTrapConditionFlags condition_flags;
    IpczPortalStatus status;
  };

  using DeferredEventQueue = absl::InlinedVector<Event, 4>;

  DeferredEventQueue events_;
};

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_TRAP_EVENT_DISPATCHER_H_
