// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/trap_event_dispatcher.h"

namespace ipcz {

TrapEventDispatcher::TrapEventDispatcher() = default;

TrapEventDispatcher::~TrapEventDispatcher() {
  DispatchAll();
}

void TrapEventDispatcher::DeferEvent(
    Ref<Trap> trap,
    const IpczTrapConditionFlags condition_flags,
    const IpczPortalStatus& status) {
  events_.emplace_back();
  Event& event = events_.back();
  event.trap = std::move(trap);
  event.condition_flags = condition_flags;
  event.status = status;
}

void TrapEventDispatcher::DispatchAll() {
  for (const Event& event : events_) {
    event.trap->MaybeDispatchEvent(event.condition_flags, event.status);
  }
}

TrapEventDispatcher::Event::Event() = default;

TrapEventDispatcher::Event::Event(const Event&) = default;

TrapEventDispatcher::Event& TrapEventDispatcher::Event::operator=(
    const Event&) = default;

TrapEventDispatcher::Event::~Event() = default;

}  // namespace ipcz
