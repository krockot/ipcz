// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_ROUTER_LINK_STATE_
#define IPCZ_SRC_CORE_ROUTER_LINK_STATE_

#include <atomic>

#include "core/node_name.h"
#include "core/sequence_number.h"
#include "core/side.h"
#include "ipcz/ipcz.h"
#include "os/memory.h"
#include "third_party/abseil-cpp/absl/numeric/int128.h"

namespace ipcz {
namespace core {

// Structure which lives in shared memory and is used by both ends of a
// PortalLink to synchronously query and reflect portal state. Note that each
// instance of this structure is only shared between the two nodes on either
// of a single PortalLink.
struct IPCZ_ALIGN(16) RouterLinkState {
  // The full shared state of a portal on one side of the link.
  struct IPCZ_ALIGN(16) SideState {
    SideState();
    ~SideState();

    // A key, set only if `is_blocking_decay` is true, which can be used to
    // validate another node's request to replace this link with a link to
    // itself.
    //
    // TODO: this is a lot of wasted space - SideState could probably just be
    // a busy-bit and a key index, with a small reusable pool of key storage on
    // each side of a NodeLink.
    absl::uint128 bypass_key;

    // Indicates whether the router on this side of the link is blocking any
    // further decay along the route.
    bool is_blocking_decay;
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

  // Aggregate state for each side of the portal pair. The portal for a given
  // side is the exclusive writer of its SideState and exclusive reader of the
  // other side's SideState.
  TwoSided<SideState> sides_;

  // Guards access to `sides_`.
  std::atomic<bool> locked_;
};

static_assert(sizeof(RouterLinkState::SideState) == 32,
              "Invalid SideState size");
static_assert(sizeof(RouterLinkState) == 80, "Invalid RouterLinkState size");

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_ROUTER_LINK_STATE_
