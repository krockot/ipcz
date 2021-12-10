// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/trap_set.h"

#include <utility>

#include "core/trap.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"

namespace ipcz {
namespace core {

TrapSet::TrapSet() = default;

TrapSet::TrapSet(TrapSet&&) = default;

TrapSet& TrapSet::operator=(TrapSet&&) = default;

TrapSet::~TrapSet() = default;

void TrapSet::Add(mem::Ref<Trap> trap) {
  traps_.insert(std::move(trap));
}

void TrapSet::Remove(Trap& trap) {
  traps_.erase(&trap);
}

void TrapSet::UpdatePortalStatus(const IpczPortalStatus& status,
                                 TrapEventDispatcher& dispatcher) {
  for (const mem::Ref<Trap>& trap : traps_) {
    trap->UpdatePortalStatus(status, dispatcher);
  }
}

void TrapSet::DisableAllAndClear() {
  for (const mem::Ref<Trap>& trap : traps_) {
    trap->Disable(IPCZ_NO_FLAGS);
  }
  traps_.clear();
}

}  // namespace core
}  // namespace ipcz
