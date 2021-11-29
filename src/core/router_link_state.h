// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_ROUTER_LINK_STATE_
#define IPCZ_SRC_CORE_ROUTER_LINK_STATE_

#include <atomic>

#include "core/node_name.h"
#include "core/sequence_number.h"
#include "core/side.h"
#include "os/memory.h"
#include "third_party/abseil-cpp/absl/numeric/int128.h"

namespace ipcz {
namespace core {

// Structure which lives in shared memory and is used by both ends of a
// PortalLink to synchronously query and reflect portal state. Note that each
// instance of this structure is only shared between the two nodes on either
// of a single PortalLink.
//
// TODO: RouterLinkState data should be rolled into NodeLinkState, and ideally
// each side would be stored in separate cache lines to avoid collisions: a side
// only writes to its own state, and only reads from the other side's state.
struct RouterLinkState {
  // The full shared state of a portal on one side of the link.
  struct SideState {
    SideState();
    ~SideState();

    // Indicates whether the router on this side of the link is decaying.
    bool is_decaying;

    // A key, set only if `is_decaying` is true, which can be used to validate
    // another node's request to replace this link with a link to itself.
    absl::uint128 bypass_key;
  };

  // Provides guarded access to this RouterLinkState's data. Note that access is
  // guarded only by a spinlock, so keep accesses brief.
  class Locked {
   public:
    Locked(RouterLinkState& state, Side side);
    ~Locked();

    SideState& this_side() { return state_.sides_[side_]; }
    SideState& other_side() { return state_.sides_[Opposite(side_)]; }

   private:
    const Side side_;
    RouterLinkState& state_;
  };

  RouterLinkState();
  ~RouterLinkState();

  // Initializes a new RouterLinkState at a given memory address and returns a
  // reference to it.
  static RouterLinkState& Initialize(void* where);

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

#endif  // IPCZ_SRC_CORE_ROUTER_LINK_STATE_
