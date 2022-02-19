// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_TRAP_H_
#define IPCZ_SRC_IPCZ_TRAP_H_

#include <cstdint>

#include "ipcz/ipcz.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "util/ref_counted.h"

namespace ipcz {

class Portal;
class TrapEventDispatcher;

// Encapsulates state and behavior of an individual trap object as created and
// managed by public ipcz API calls. A trap watches for state changes on a
// single portal and invokes a fixed callback when certain changes are observed.
// Traps must be armed in order to invoke a callback, and arming is only
// possible while interesting conditions remain unmet.
class Trap : public RefCounted {
 public:
  enum class UpdateReason {
    kParcelReceived,
    kParcelConsumed,
    kStatusQuery,
    kRouteClosed,
  };

  // Constructs a trap to watch for `conditions` on `portal`. If such conditions
  // become met while the trap is armed, `handler` is invoked with `context`.
  Trap(Ref<Portal> portal,
       const IpczTrapConditions& conditions,
       IpczTrapEventHandler handler,
       uint64_t context);

  const Ref<Portal>& portal() const { return portal_; }

  // Attempts to arm this trap, which can only succeed if its observed
  // conditions are currently unmet on the portal. If this fails due to the
  // conditions being met already, it returns IPCZ_RESULT_FAILED_PRECONDITION
  // and `satisfied_condition_flags` and/or `status` are populated if non-null.
  IpczResult Arm(IpczTrapConditionFlags* satisfied_condition_flags,
                 IpczPortalStatus* status);

  // Disables the trap to immediately and permanently prevent any further
  // handler invocations.
  void Disable(IpczDestroyTrapFlags flags);

  // Notifies the trap about a state change on its observed portal. If the trap
  // is currently armed and the state change should elicit an event handler
  // invocation, said invocation is queued on `dispatcher`.
  void UpdatePortalStatus(const IpczPortalStatus& status,
                          UpdateReason reason,
                          TrapEventDispatcher& dispatcher);

 private:
  friend class TrapEventDispatcher;

  ~Trap() override;

  IpczTrapConditionFlags GetEventFlags(const IpczPortalStatus& status,
                                       UpdateReason reason);
  void MaybeDispatchEvent(IpczTrapConditionFlags condition_flags,
                          const IpczPortalStatus& status);
  bool HasNoCurrentDispatches() const;

  const Ref<Portal> portal_;
  const IpczTrapConditions conditions_;
  const IpczTrapEventHandler handler_;
  const uint64_t context_;

  absl::Mutex mutex_;
  bool is_enabled_ ABSL_GUARDED_BY(mutex_) = true;
  bool is_armed_ ABSL_GUARDED_BY(mutex_) = false;
  size_t num_current_dispatches_ ABSL_GUARDED_BY(mutex_) = 0;
};

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_TRAP_H_
