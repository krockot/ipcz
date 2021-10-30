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
  enum Status : uint8_t {
    // The portal is ready to receive messages.
    kReady = 0,

    // The portal has been closed.
    kClosed,

    // The portal is being shipped off to another node somewhere and cannot
    // accept new messages at the moment.
    kMoving,
  };

  struct QueueState {
    uint32_t num_sent_bytes;
    uint32_t num_sent_parcels;
    uint32_t num_read_bytes;
    uint32_t num_read_parcels;
  };

  struct SideState {
    SideState();
    ~SideState();

    Status status{Status::kReady};
    QueueState queue_state;
  };

  class Locked {
   public:
    Locked(PortalControlBlock& block);
    ~Locked();

    SideState& side(Side side) { return block_.sides_[side]; }

    SideState& opposite(Side side) { return block_.sides_[Opposite(side)]; }

   private:
    PortalControlBlock& block_;
  };

  PortalControlBlock();
  ~PortalControlBlock();

  // Initializes a new PortalControlBlock at a given memory address and returns
  // a reference to it.
  static PortalControlBlock& Initialize(void* where);

  SideState& unsafe_side(Side side) { return sides_[side]; }

  SideState& unsafe_opposite(Side side) { return sides_[Opposite(side)]; }

 private:
  void Lock();
  void Unlock();

  // Guards access to `sides_`.
  std::atomic<bool> locked_;

  // Aggregate state for each side of the portal pair. The portal for a given
  // side is the exclusive writer of its SideState and exclusive reader of the
  // other side's SideState.
  TwoSided<SideState> sides_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_PORTAL_CONTROL_BLOCK_H_
