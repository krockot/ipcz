// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_TRAP_H_
#define IPCZ_SRC_CORE_TRAP_H_

#include <cstdint>

#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"

namespace ipcz {
namespace core {

class Portal;
class TrapEventDispatcher;

class Trap : public mem::RefCounted {
 public:
  Trap(mem::Ref<Portal> portal,
       const IpczTrapConditions& conditions,
       IpczTrapEventHandler handler,
       uint64_t context);

  const mem::Ref<Portal>& portal() const { return portal_; }

  IpczResult Arm(IpczTrapConditionFlags* satisfied_condition_flags,
                 IpczPortalStatus* status);
  void Disable(IpczDestroyTrapFlags flags);

  void UpdatePortalStatus(const IpczPortalStatus& status,
                          TrapEventDispatcher& dispatcher);

 private:
  friend class TrapEventDispatcher;

  ~Trap() override;

  IpczTrapConditionFlags GetEventFlags(const IpczPortalStatus& status);
  void MaybeDispatchEvent(IpczTrapConditionFlags condition_flags,
                          const IpczPortalStatus& status);
  bool HasNoCurrentDispatches() const;

  const mem::Ref<Portal> portal_;
  const IpczTrapConditions conditions_;
  const IpczTrapEventHandler handler_;
  const uint64_t context_;

  absl::Mutex mutex_;
  bool is_enabled_ ABSL_GUARDED_BY(mutex_) = true;
  bool is_armed_ ABSL_GUARDED_BY(mutex_) = false;
  size_t num_current_dispatches_ ABSL_GUARDED_BY(mutex_) = 0;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_TRAP_H_
