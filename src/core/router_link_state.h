// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_ROUTER_LINK_STATE_
#define IPCZ_SRC_CORE_ROUTER_LINK_STATE_

#include <atomic>
#include <cstdint>

#include "core/link_side.h"
#include "core/node_name.h"
#include "ipcz/ipcz.h"

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

  // This is populated by a proxying router once it has successfully
  // negotiated its own turn to decay, and it names the node which hosts the
  // proxy's own inward peer. That peer will imminently reach out to the proxy's
  // outward peer directly (who shares this link with the proxy) to establish a
  // bypass link. The outward peer can authenticate the source of that request
  // against the name stored here.
  NodeName allowed_bypass_request_source;

  // Link status which both sides atomically update to coordinate proxy decay.
  // The link's status is only relevant for a transverse link -- that is, a
  // link which links one half of a route to the other. Every route has at most
  // one transverse link, and zero if and only if the route is dead.
  //
  // Every route begins life with a single transverse link whose status is
  // kReady, allowing either side to initiate its own decay if it becomes a
  // proxy by extending the route further along its own half of the route.
  //
  // The only other time a transverse link is created is for proxy bypass, where
  // the new link is created with a kNotReady status. Then as each side of the
  // bypass link loses its decaying proxy link over time, it updates the status
  // to reflect that its side is ready.
  enum Status : uint8_t {
    // This is a new link which was created to bypass a proxy. Both ends of the
    // link still have additional decaying links to the bypassed proxy. As those
    // links fully decay, each side will upgrade the status to kReadyOnA or
    // kReadyOnB and eventually to kReady when both sides are ready.
    kNotReady = 0,

    // Side A of this link has no decaying links, but side B still has some.
    kReadyOnA = 1,

    // Side B of this link has no decaying links, but side A still has some.
    kReadyOnB = 2,

    // Neither side of this link has any decaying links. Decay of either side is
    // possible if other conditions apply.
    kReady = 3,

    // Side A has locked itself in for decay. This link will soon be obsoleted.
    kDecayOnA = 4,

    // Side B has locked itself in for decay. This link will soon be obsoleted.
    kDecayOnB = 5,
  };

  std::atomic<Status> status{kNotReady};

  bool is_link_ready() const {
    return status.load(std::memory_order_relaxed) == kReady;
  }

  bool is_decaying(LinkSide side) const {
    return side == LinkSide::kA ? (status == kDecayOnA) : (status == kDecayOnB);
  }

  // Updates the status to reflect that the given `side` is ready, meaning it
  // has fully decayed any decaying links which preceded this link. Returns true
  // if successful or false on failure. This can only fail if the link state has
  // an unexpected status, implying either memory corruption or a misbehaving
  // node.
  bool SetSideReady(LinkSide side);

  // Attempts to update the status to reflect that the given `side` is decaying.
  // In order for this to succeed, the link must have a kReady status. Returns
  // true on success or false on failure.
  bool TryToDecay(LinkSide side);

  // Cancels a decay condition previously initiated by a successful call to
  // TryToDecay().
  bool CancelDecay();
};

static_assert(sizeof(RouterLinkState) == 32, "Invalid RouterLinkState size");

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_ROUTER_LINK_STATE_
