// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/trap_event_dispatcher.h"

#include "debug/log.h"

namespace ipcz {
namespace core {

TrapEventDispatcher::TrapEventDispatcher() = default;

TrapEventDispatcher::~TrapEventDispatcher() {
  DispatchAll();
}

void TrapEventDispatcher::DeferEvent(const Trap& trap,
                                     IpczTrapConditionFlags condition_flags,
                                     const IpczPortalStatus& status) {
  events_.emplace_back();
  Event& event = events_.back();
  event.is_trap_armed = trap.is_armed_flag();
  event.handler = trap.handler();
  event.context = trap.context();
  event.condition_flags = condition_flags;
  event.status = status;
}

void TrapEventDispatcher::DispatchAll() {
  absl::InlinedVector<Event, 8> events;
  std::swap(events, events_);
  for (const Event& event : events) {
    // TODO: this is not sufficient - it's possible for the trap to be destroyed
    // after this flag is checked but before the handler is invoked, which is
    // a violation of the API contract.
    if (event.is_trap_armed->load(std::memory_order_acquire)) {
      IpczTrapEvent e = {sizeof(e)};
      e.context = event.context;
      e.condition_flags = event.condition_flags;
      e.status = &event.status;
      event.handler(&e);
    }
  }
}

TrapEventDispatcher::Event::Event() = default;

TrapEventDispatcher::Event::Event(Event&&) = default;

TrapEventDispatcher::Event& TrapEventDispatcher::Event::operator=(Event&&) =
    default;

TrapEventDispatcher::Event::~Event() = default;

}  // namespace core
}  // namespace ipcz
