// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_PORTAL_LINK_STATE_
#define IPCZ_SRC_CORE_PORTAL_LINK_STATE_

#include <atomic>

#include "core/node_name.h"
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
  // Conveys the basic mode of operation for a portal on one side of the link.
  enum Mode : uint32_t {
    // The portal is in active use and ready to send and receive parcels. It is
    // a terminal portal along the route.
    kActive = 0,

    // The portal has been closed. It is a terminal portal along the route.
    // Any parcels targeting the portal can be discarded if not yet sent,
    // because they will not be received.
    kClosed,

    // The portal has been shipped off to another node and does not want to
    // receive any more parcels. In this state
    kMoved,
  };

  // The full shared state of a portal on one side of the link.
  struct SideState {
    SideState();
    ~SideState();

    // See the Mode description above.
    Mode mode;
    uint32_t padding;

    // The name of the node which will host our successor if `mode` is kMoved.
    // The peer will expect a message from this node with the key below to
    // unlock a new PortalLink between the two, to eventually replace this link.
    // If this node happens to disappear (e.g. crash) before that can happen,
    // the other side of this link will at least be able to observe its
    // disconnection and deduce that the route is toast.
    NodeName successor_node;

    // A key - set only if `mode` is kMoved - which will also be shared with
    // another node to help it establish a successor to this moved portal. The
    // other node will ssend
    // negotiate a route with the other side
    absl::uint128 successor_key;

    // The length of the sequence of parcels which has already been sent or will
    // imminently be sent from this side.
    //
    // Only valid once `mode` is kClosed or kMoved. Peers can use this to deduce
    // whether to expect additional parcels over the corresponding PortalLink.
    // This is not necessarily the total number of parcels sent by this portal,
    // but it is the total number sent by any portal on the same side of the
    // same route (i.e. any portal in this portal's own chain of relocations.)
    //
    // Each side of a PortalLink upholds an important constraint: they will not
    // increase their own `sequence_length` once the other side is in kMoved
    // mode. Similarly when a side enters kMoved mode, it agrees to retain
    // responsibility for forwarding along ALL parcels sent by the other side
    // with a sequence number up to (but not including) this value.
    //
    // So if a link is established at with one side starting at sequence number
    // and that side sends 5 parcels before the other side moves, the other side
    // will atomically set its own mode to kMoved while also seeing the first
    // side's sequence length of 47. As a result, it it implicitly then agrees
    // agrees to forward parcels 42, 43, 44, 45, and 46 to its new location
    // before forgetting about this link.
    SequenceNumber sequence_length;

    // The number of parcels consumed from the portal on this side. This does
    // not count parcels which are forwarded elsewhere: only parcels which were
    // read by the application with a Get operation.
    //
    // The other side can use this to deduce an accurate upper bound (note: not
    // an exact amount, but an upper bound) on the number of its sent parcels
    // which are still unread. This is used to implement certain limits on Put
    // operations to support backpressure.
    uint64_t num_parcels_consumed;

    // Similar to above but conveys the number of bytes consumed from the
    // portal. This is the sum of byte lengths of the data in all consumed
    // parcels. Also used to implement Put limits.
    uint64_t num_bytes_consumed;
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
