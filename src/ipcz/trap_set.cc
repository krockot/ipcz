// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/trap_set.h"

#include <utility>

#include "ipcz/ipcz.h"
#include "ipcz/trap.h"
#include "util/ref_counted.h"

namespace ipcz {

TrapSet::TrapSet() = default;

TrapSet::TrapSet(TrapSet&&) = default;

TrapSet& TrapSet::operator=(TrapSet&&) = default;

TrapSet::~TrapSet() = default;

void TrapSet::Add(Ref<Trap> trap) {
  traps_.insert(std::move(trap));
}

void TrapSet::Remove(Trap& trap) {
  traps_.erase(&trap);
}

void TrapSet::UpdatePortalStatus(const IpczPortalStatus& status,
                                 Trap::UpdateReason reason,
                                 TrapEventDispatcher& dispatcher) {
  for (const Ref<Trap>& trap : traps_) {
    trap->UpdatePortalStatus(status, reason, dispatcher);
  }
}

void TrapSet::DisableAllAndClear() {
  for (const Ref<Trap>& trap : traps_) {
    trap->Disable();
  }
  traps_.clear();
}

}  // namespace ipcz
