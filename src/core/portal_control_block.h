// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_PORTAL_CONTROL_BLOCK_H_
#define IPCZ_SRC_CORE_PORTAL_CONTROL_BLOCK_H_

#include <atomic>

#include "core/side.h"

namespace ipcz {
namespace core {

// Structure which lives in shared memory and is used by both ends of an
// entangled portal pair to synchronously query and reflect portal state.
struct PortalControlBlock {
  // Conveys the basic status of one of the portals.
  enum PortalStatus : uint8_t {
    // The portal is ready to receive messages.
    kReady = 0,

    // The portal has been closed.
    kClosed,

    // The portal is being shipped off to another node somewhere and cannot
    // accept new messages at the moment.
    kMoving,
  };

  PortalControlBlock();
  ~PortalControlBlock();

  // Status for each side.
  TwoSidedArray<PortalStatus> status;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_PORTAL_CONTROL_BLOCK_H_
