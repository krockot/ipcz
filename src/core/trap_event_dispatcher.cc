// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/trap_event_dispatcher.h"

namespace ipcz {
namespace core {

TrapEventDispatcher::TrapEventDispatcher() = default;

TrapEventDispatcher::~TrapEventDispatcher() {
  DispatchAll();
}

void TrapEventDispatcher::DeferEvent(
    IpczTrapEventHandler handler,
    uintptr_t context,
    const IpczTrapConditionFlags condition_flags,
    const IpczPortalStatus& status,
    Trap::SharedState& trap_state) {
  events_.emplace_back();
  Event& event = events_.back();
  event.trap_state = mem::WrapRefCounted(&trap_state);
  event.handler = handler;
  event.context = context;
  event.condition_flags = condition_flags;
  event.status = status;
}

void TrapEventDispatcher::DispatchAll() {
  absl::InlinedVector<Event, 8> events;
  std::swap(events, events_);
  for (const Event& event : events) {
    if (event.trap_state->DisarmIfArmed()) {
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
