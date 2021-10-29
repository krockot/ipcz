// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_TRAP_H_
#define IPCZ_SRC_CORE_TRAP_H_

#include <cstdint>

#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"

namespace ipcz {
namespace core {

class TrapEventDispatcher;

class Trap {
 public:
  struct SharedState final : mem::RefCounted {
    SharedState();

    bool DisarmIfArmed();

   private:
    friend class Trap;

    ~SharedState() final;

    absl::Mutex mutex_;
    bool is_armed_ ABSL_GUARDED_BY(mutex_) = false;
  };

  Trap(const IpczTrapConditions& conditions,
       IpczTrapEventHandler handler,
       uintptr_t context);
  ~Trap();

  IpczResult Arm(const IpczPortalStatus& status, IpczTrapConditionFlags& flags);

  const IpczTrapConditions& conditions() const { return conditions_; }
  IpczTrapEventHandler handler() const { return handler_; }
  uintptr_t context() const { return context_; }

  void MaybeNotify(TrapEventDispatcher& dispatcher,
                   const IpczPortalStatus& status);
  void MaybeNotifyDestroyed(TrapEventDispatcher& dispatcher,
                            const IpczPortalStatus& status);

 private:
  IpczTrapConditionFlags GetEventFlags(const IpczPortalStatus& status);

  const IpczTrapConditions conditions_;
  const IpczTrapEventHandler handler_;
  const uintptr_t context_;
  const mem::Ref<SharedState> state_{mem::MakeRefCounted<SharedState>()};
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_TRAP_H_
