// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_TRAP_SET_H_
#define IPCZ_SRC_IPCZ_TRAP_SET_H_

#include "ipcz/ipcz.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "util/ref_counted.h"

namespace ipcz {

class TrapEventDispatcher;

// A set of traps installed a portal.
class TrapSet {
 public:
  // The reason for each status update when something happens that might
  // interest a trap. This is useful for observing edge-triggered conditions.
  enum class UpdateReason {
    kInstallTrap,
    kNewLocalParcel,
    kPeerClosed,
    kLocalParcelConsumed,
  };

  TrapSet();
  TrapSet(const TrapSet&) = delete;
  TrapSet& operator=(const TrapSet&) = delete;
  ~TrapSet();

  // Attempts to install a new trap in the set. This effectively implements
  // the ipcz Trap() API. If `conditions` are already met, returns
  // IPCZ_RESULT_FAILED_PRECONDITION and populates `satisfied_condition_flags`
  // and/or `status` if non-null.
  IpczResult Add(const IpczTrapConditions& conditions,
                 IpczTrapEventHandler handler,
                 uint64_t context,
                 const IpczPortalStatus& current_status,
                 IpczTrapConditionFlags* satisfied_condition_flags,
                 IpczPortalStatus* status);
  void UpdatePortalStatus(const IpczPortalStatus& status,
                          UpdateReason reason,
                          TrapEventDispatcher& dispatcher);
  void RemoveAll(TrapEventDispatcher& dispatcher);

 private:
  struct Trap {
    Trap(IpczTrapConditions conditions,
         IpczTrapEventHandler handler,
         uint64_t context);
    ~Trap();

    IpczTrapConditions conditions;
    IpczTrapEventHandler handler;
    uint64_t context;
  };

  using TrapList = absl::InlinedVector<Trap, 4>;
  TrapList traps_;
  IpczPortalStatus last_known_status_ = {.size = sizeof(last_known_status_)};
};

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_TRAP_SET_H_
