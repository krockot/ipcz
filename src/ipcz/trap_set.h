// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_TRAP_SET_H_
#define IPCZ_SRC_IPCZ_TRAP_SET_H_

#include "ipcz/ipcz.h"
#include "ipcz/trap.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_set.h"
#include "util/ref_counted.h"

namespace ipcz {

class TrapEventDispatcher;

// A set of Trap objects managed on a single portal.
class TrapSet {
 public:
  TrapSet();
  TrapSet(TrapSet&&);
  TrapSet& operator=(TrapSet&&);
  ~TrapSet();

  void Add(Ref<Trap> trap);
  void Remove(Trap& trap);
  void UpdatePortalStatus(const IpczPortalStatus& status,
                          Trap::UpdateReason reason,
                          TrapEventDispatcher& dispatcher);
  void DisableAllAndClear();

 private:
  absl::flat_hash_set<Ref<Trap>> traps_;
};

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_TRAP_SET_H_
