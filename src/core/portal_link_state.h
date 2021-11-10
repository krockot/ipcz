// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_PORTAL_LINK_STATE_
#define IPCZ_SRC_CORE_PORTAL_LINK_STATE_

#include <atomic>

#include "core/node_name.h"
#include "core/routing_mode.h"
#include "core/sequence_number.h"
#include "core/side.h"
#include "os/memory.h"
#include "third_party/abseil-cpp/absl/numeric/int128.h"

namespace ipcz {
namespace core {

// Structure which lives in shared memory and is used by both ends of an
// entangled portal pair to synchronously query and reflect portal state. Note
// that each instance of this structure is only shared between the two nodes on
// either side of a single NodeLink, and optionally also with the broker process
// if it's not one of them.
//
// TODO: PortalLinkState data should be rolled into NodeLinkState, and ideally
// each side would be stored in separate cache lines to avoid collisions: a side
// only writes to its own state, and only reads from the other side's state.
struct PortalLinkState {
  // The full shared state of a portal on one side of the link.
  struct SideState {
    SideState();
    ~SideState();

    RoutingMode routing_mode;

    // A key, set only if `routing_mode` is kHalfProxy, which can be used to
    // validate another node's request to replace this link with a link to
    // itself.
    absl::uint128 successor_key;

    // The length of the sequence of parcels which has already been sent or will
    // imminently be sent from this side.
    //
    // Only valid once `routing_mode` is kClosed or kHalfProxy. Peers can use
    // this to deduce whether to expect additional parcels over the
    // corresponding PortalLink. This is not necessarily the total number of
    // parcels sent by this portal, but it is the total number sent by any
    // portal on the same side of the same route (i.e. any portal in this
    // portal's own chain of relocations.)
    //
    // Each side of a PortalLink upholds an important constraint: they will not
    // increase their own `sequence_length` once the other side is in kHalfProxy
    // mode. Similarly when a side enters kHalfProxy mode, it assumes
    // responsibility for forwarding along ALL parcels sent by the other side
    // with a sequence number up to (but not including) the other side's value
    // for this field.
    //
    // So if a link is established at with one side starting at sequence number
    // 42 and that side sends 5 parcels before the other side switches to
    // kHalfProxy, the other side's sequence length will also atomically be set
    // to 47. As a result, it ensures that it will forward parcels 42, 43, 44,
    // 45, and 46 to its new location before forgetting about this link.
    SequenceNumber sequence_length;
  };

  // Provides guarded access to this PortalLinkState's data. Note that access is
  // guarded only by a spinlock, so keep accesses   brief.
  class Locked {
   public:
    Locked(PortalLinkState& state, Side side);
    ~Locked();

    SideState& this_side() { return state_.sides_[side_]; }
    SideState& other_side() { return state_.sides_[Opposite(side_)]; }

   private:
    const Side side_;
    PortalLinkState& state_;
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
