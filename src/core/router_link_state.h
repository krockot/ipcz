// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_ROUTER_LINK_STATE_
#define IPCZ_SRC_CORE_ROUTER_LINK_STATE_

#include <atomic>
#include <cstdint>

#include "core/side.h"
#include "ipcz/ipcz.h"
#include "third_party/abseil-cpp/absl/numeric/int128.h"

namespace ipcz {
namespace core {

// Structure which lives in shared memory and is used by both ends of a
// RouterLink to synchronously query and reflect router state. Note that each
// instance of this structure is only shared between at most two routers on two
// nodes.
struct IPCZ_ALIGN(16) RouterLinkState {
  RouterLinkState();
  ~RouterLinkState();

  // In-place initialization of a new RouterLinkState at `where`.
  static RouterLinkState& Initialize(void* where);

  // A key which can be used by one side of the link to bypass the other. This
  // must only be set by one side, and only after successfully entering a
  // decaying state (i.e. setting `status` to kLeftDecaying or kRightDecaying as
  // described below.)
  //
  // This key is also shared with a router (via
  // RouterLink::RequestProxyBypassInitiation) on the other side of the decaying
  // router, and that router will use it to authenticate its bypass request to
  // the other (non-decaying) router on this link:
  //
  //          /- this RouterLinkState, with the bypass key in shared memory
  //         |
  //         |     /- decaying router
  //         v    v
  //     L0 ---- R0 ---- R1
  //                     ^___ peer who will use the bypass key to authenticate
  //                          its bypass request to L0.
  //
  absl::uint128 bypass_key;

  // Link status which both sides atomically update to coordinate proxy decay.
  // The link's status is only relevant for a transverse link -- that is, a
  // link which links a left-side route to a right-side router. At any moment,
  // every route has at most one transverse link (and zero only if the route is
  // dead).
  //
  // Every route begins life with a single transverse link whose status is
  // kReady.
  //
  // The only other time a transverse link is created is for proxy bypass, where
  // the new link is created with a kNotReady status. As each side of the bypass
  // link loses its decaying proxy link, it attempts to set its own ready
  // status. For example if the left side is ready it will attempt to atomically
  // change a kNotReady status to a kLeftReady status. If the right side was
  // ready first, the left side may have to try again, e.g. to atomically change
  // a kRightReady status to kReady.
  //
  // Only once a link is ready can either side attempt to lock itself in as the
  // next decay candidate by atomically changing the status from kReady to
  // kLeftDecaying or kRightDecaying.
  enum Status : uint8_t {
    kNotReady = 0,
    kLeftReady = 1,
    kRightReady = 2,
    kReady = 3,
    kLeftDecaying = 4,
    kRightDecaying = 5,
  };

  std::atomic<Status> status{kNotReady};

  bool is_link_ready() const {
    return status.load(std::memory_order_relaxed) == kReady;
  }

  bool is_decaying(Side side) const {
    return side == Side::kLeft ? (status == kLeftDecaying)
                               : (status == kRightDecaying);
  }

  // Updates the status to reflect that the given `side` is ready, meaning it
  // has fully decayed any decaying links which preceded this link. Returns true
  // if successful or false on failure. This can only fail if the link state has
  // an unexpected status, implying either memory corruption or a misbehaving
  // node..
  bool SetSideReady(Side side);

  // Attempts to update the status to reflect that the given `side` is decaying.
  // In order for this to succeed, the link must have a kReady status. Returns
  // true on success or false on failure.
  bool TryToDecay(Side side);
};

static_assert(sizeof(RouterLinkState) == 32, "Invalid RouterLinkState size");

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_ROUTER_LINK_STATE_
