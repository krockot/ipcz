// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_ROUTER_LINK_STATE_
#define IPCZ_SRC_IPCZ_ROUTER_LINK_STATE_

#include <atomic>
#include <cstdint>

#include "ipcz/ipcz.h"
#include "ipcz/link_side.h"
#include "ipcz/node_name.h"
#include "ipcz/ref_counted_fragment.h"

namespace ipcz {

class ParcelQueue;

// Structure which lives in shared memory and is used by both ends of a
// RouterLink to synchronously query and reflect router state. Note that each
// instance of this structure is only shared between at most two routers on two
// nodes.
struct IPCZ_ALIGN(8) RouterLinkState : public RefCountedFragment {
  RouterLinkState();
  ~RouterLinkState();

  // In-place initialization of a new RouterLinkState at `where`.
  static RouterLinkState& Initialize(void* where);

  // Link status which both sides atomically update to coordinate proxy bypass.
  // The link's status is only relevant for a central link -- that is, a link
  // which links one half of a route to the other. Every route has at most one
  // central transverse link, and zero if and only if the route is dead.
  //
  // Every route begins life with a single central link whose status is kStable,
  // allowing either side to lock the link for bypass if it becomes a proxy.
  //
  // The only other time a central link is created is for proxy bypass, where
  // the new link is created with a kUnstable status. Then as each side of the
  // bypass link loses its decaying links over time, it updates the status to
  // reflect that its side is ready to support another bypass operation if one
  // is needed.
  using Status = uint32_t;

  // This is a fresh link established to bypass a proxy. Each side of the link
  // still has at least one decaying link and is therefore not yet ready to
  // support any potential replacement of this link.
  static constexpr Status kUnstable = 0;

  // Set if side A or B of this link is stable, respectively.
  static constexpr Status kSideAStable = 1 << 0;
  static constexpr Status kSideBStable = 1 << 1;
  static constexpr Status kStable = kSideAStable | kSideBStable;

  // When either side attempts to lock this link and fails because ther other
  // side is still unstable, they set their corresponding "waiting" bit instead.
  // Once the other side is stable, this bit informs the other side that they
  // should send a flush notification back to this side to unblock whatever
  // operation was waiting for a stable link.
  static constexpr Status kSideAWaiting = 1 << 2;
  static constexpr Status kSideBWaiting = 1 << 3;

  // Set if this link has been locked by side A or B, respectively. These bits
  // are always mutually exclusive and may only be set once kStable are set. A
  // A link may be locked to initiate bypass of one side, or to propagate route
  // closure from one side.
  static constexpr Status kLockedBySideA = 1 << 4;
  static constexpr Status kLockedBySideB = 1 << 5;

  std::atomic<Status> status{kUnstable};

  // This is populated by a proxying router once it has successfully
  // negotiated its own turn to be bypassed, and it names the node which hosts
  // the proxy's own inward peer. That peer will imminently reach out to the
  // proxy's outward peer directly (who shares this link with the proxy) to
  // establish a bypass link. The outward peer can authenticate the source of
  // that request against the name stored here.
  NodeName allowed_bypass_request_source;

  // Approximates the number of parcels and data bytes received and queued for
  // retrieval on each side of this link. These values are saturated if the
  // actual values would exceed 2**32-1.
  std::atomic<uint32_t> num_parcels_queued_for_a;
  std::atomic<uint32_t> num_bytes_queued_for_a;
  std::atomic<uint32_t> num_parcels_queued_for_b;
  std::atomic<uint32_t> num_bytes_queued_for_b;

  // Flags for each side to indicate to the other that it wants to be signaled
  // when parcel data is consumed on the other side. These are set when the
  // corresponding router has at least one trap installed to watch for a drop in
  // the quantity of remotely queued parcels or bytes.
  std::atomic<bool> should_signal_a_when_reading_b;
  std::atomic<bool> should_signal_b_when_reading_a;

  // Reserved slots.
  uint8_t reserved0[2];
  uint32_t reserved1[5];

  bool is_locked_by(LinkSide side) const {
    Status s = status.load(std::memory_order_relaxed);
    if (side == LinkSide::kA) {
      return (s & kLockedBySideA) != 0;
    }
    return (s & kLockedBySideB) != 0;
  }

  // Updates the status to reflect that the given `side` is stable, meaning it's
  // no longer holding on to any decaying links.
  void SetSideStable(LinkSide side);

  // Attempts to lock the state of this link from one side so that the router on
  // that side can coordinate its own bypass or propagate its own closure. In
  // order for this to succeed, both kStable bits must be set and the link must
  // not already be locked by the other side. Returns true if the link was
  // locked succesfully, or false otherwise.
  bool TryLock(LinkSide side);

  // Unlocks a link previously locked by a successful call to TryLock() for the
  // same `side`.
  void Unlock(LinkSide side);

  // If both sides of the link are stable AND `side` was marked as waiting for
  // that to happen, this resets that wating bit and returns true. Otherwise
  // this returns false and the link's status is unchanged.
  bool ResetWaitingBit(LinkSide side);

  // Returns a structure describing a best-effort snapshot of the inbound parcel
  // queue state on the given side of this link.
  struct QueueState {
    uint32_t num_inbound_bytes_queued;
    uint32_t num_inbound_parcels_queued;
  };
  QueueState GetQueueState(LinkSide side) const;

  // Updates the queue state for the given side. Values which exceed 2**32-1 are
  // clamped to that value. Returns true iff the opposite side of the link wants
  // to be notified about this update.
  bool UpdateQueueState(LinkSide side, size_t num_bytes, size_t num_parcels);

  // Sets an appropriate bit to indicate whether the router on the given `side`
  // of this link should notify the opposite side after consuming inbound parcel
  // data. Returns the previous value of the modified bit, which may be the same
  // as the new value.
  bool SetSignalOnDataConsumedBy(LinkSide side, bool signal);
};

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_ROUTER_LINK_STATE_
