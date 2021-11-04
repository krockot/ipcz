// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_PORTAL_LINK_STATE_
#define IPCZ_SRC_CORE_PORTAL_LINK_STATE_

#include <atomic>

#include "core/side.h"
#include "os/memory.h"

namespace ipcz {
namespace core {

// Structure which lives in shared memory and is used by both ends of an
// entangled portal pair to synchronously query and reflect portal state. Note
// that each instance of this structure is only shared between at most two
// non-broker nodes. It may also be shared with the broker (TODO: maybe?)
struct PortalLinkState {
  // Conveys the basic status of one of the portals.
  enum Status : uint8_t {
    // The portal is ready to receive messages.
    kReady = 0,

    // The portal has been closed.
    kClosed,

    // The portal has been shipped off to another node somewhere and cannot
    // accept new messages at the moment.
    //
    // TODO: in this state we need to also expose a key that can be used by our
    // peer to unlock the new portal
    kMoved,
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

  // Provides guarded access to this PortalLinkState's data. Note that access is
  // guarded only by a spinlock, so keep accesses   brief.
  class Locked {
   public:
    Locked(const os::Memory::Mapping& link_state_mapping, Side side);
    Locked(PortalLinkState& link_state, Side side);
    ~Locked();

    SideState& this_side() { return link_state_.sides_[side_]; }
    SideState& other_side() { return link_state_.sides_[Opposite(side_)]; }

   private:
    const Side side_;
    PortalLinkState& link_state_;
  };

  PortalLinkState();
  ~PortalLinkState();

  // Initializes a new PortalLinkState at a given memory address and returns a
  // reference to it.
  static PortalLinkState& Initialize(void* where);

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

#endif  // IPCZ_SRC_CORE_PORTAL_LINK_STATE_
