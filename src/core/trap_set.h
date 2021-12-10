// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_TRAP_SET_H_
#define IPCZ_SRC_CORE_TRAP_SET_H_

#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_set.h"

namespace ipcz {
namespace core {

class Trap;
class TrapEventDispatcher;

class TrapSet {
 public:
  TrapSet();
  TrapSet(TrapSet&&);
  TrapSet& operator=(TrapSet&&);
  ~TrapSet();

  void Add(mem::Ref<Trap> trap);
  void Remove(Trap& trap);
  void UpdatePortalStatus(const IpczPortalStatus& status,
                          TrapEventDispatcher& dispatcher);
  void DisableAllAndClear();

 private:
  absl::flat_hash_set<mem::Ref<Trap>> traps_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_TRAP_SET_H_
