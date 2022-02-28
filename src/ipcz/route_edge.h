// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_ROUTE_EDGE_
#define IPCZ_SRC_IPCZ_ROUTE_EDGE_

#include "ipcz/ipcz.h"
#include "ipcz/node_name.h"
#include "ipcz/parcel.h"
#include "ipcz/sequence_number.h"
#include "ipcz/sublink_id.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "util/ref_counted.h"

namespace ipcz {

class NodeLink;
class ParcelQueue;
class Router;
class RouterLink;

// A RouteEdge is responsible for parcel and control message ingress and
// egress on one (inward-facing or outward-facing) side of a router.
//
// Over the course of its lifetime a RouteEdge may utilize many different
// RouterLinks, but at any moment it can only have two: at most one "primary"
// link and one "decaying" link.
//
// An active decaying link's usage is restricted to transmission and receipt of
// a limited range of parcels based on SequenceNumber, and once all expected
// parcels are sent and received, the link is dropped.
//
// When a RouteEdge has no decaying link, it may be able to transition its
// primary link to a decaying link, while adopting a new primary link to take
// its place. This process of incremental link replacement is the basis for ipcz
// route reduction.
class RouteEdge {
 public:
  RouteEdge();
  RouteEdge(const RouteEdge&) = delete;
  RouteEdge& operator=(const RouteEdge&) = delete;
  ~RouteEdge();

  const Ref<RouterLink>& primary_link() const { return primary_link_; }
  const Ref<RouterLink>& decaying_link() const { return decaying_link_; }

  // Indicates whether this edge is stable, meaning it is not currently decaying
  // a link and it has a valid primary link.
  bool is_stable() const {
    return primary_link_ && !decaying_link_ && !was_decay_deferred_;
  }

  // Indicates whether this edge is exclusively decaying with no replacement
  // primary link anticipated. This means it's on its way out of existence.
  bool is_decaying() const {
    return (decaying_link_ || was_decay_deferred_) && !primary_link_;
  }

  // Set limits on the current (or imminent) decaying link's usage.
  void set_length_to_decaying_link(SequenceNumber length) {
    ABSL_ASSERT(!is_stable());
    ABSL_ASSERT(!length_to_decaying_link_);
    length_to_decaying_link_ = length;
  }
  void set_length_from_decaying_link(SequenceNumber length) {
    ABSL_ASSERT(!is_stable());
    ABSL_ASSERT(!length_from_decaying_link_);
    length_from_decaying_link_ = length;
  }
  void set_length_to_and_from_decaying_link(SequenceNumber length_to,
                                            SequenceNumber length_from) {
    ABSL_ASSERT(!is_stable());
    ABSL_ASSERT(!length_to_decaying_link_);
    ABSL_ASSERT(!length_from_decaying_link_);
    length_to_decaying_link_ = length_to;
    length_from_decaying_link_ = length_from;
  }

  absl::optional<SequenceNumber> length_to_decaying_link() const {
    return length_to_decaying_link_;
  }

  absl::optional<SequenceNumber> length_from_decaying_link() const {
    return length_from_decaying_link_;
  }

  // If we have an active primary link and it goes to a router in the same node
  // as this object, this returns a reference to that router. Otherwise this
  // returns null.
  Ref<Router> GetLocalPeer() const;

  // Returns a the local peer router on the other side of the decaying link, if
  // this edge has a decaying link to a local peer. Otherwise returns null.
  Ref<Router> GetDecayingLocalPeer() const;

  // Sets the primary link for this edge. Only valid to call if the edge does
  // not currently have a primary link.
  void SetPrimaryLink(Ref<RouterLink> link);

  // Releases this edge's primary link and returns a reference to the caller.
  Ref<RouterLink> ReleasePrimaryLink();

  // Releases this edge's decaying link and returns a reference to the caller.
  Ref<RouterLink> ReleaseDecayingLink();

  // Indicates whether either link on this edge is routed over the given
  // NodeLink and SubLinkId.
  bool IsRoutedThrough(const NodeLink& link, SublinkId sublink) const;

  // Flushes any transmittable parcels from the given `parcels` queue, based on
  // the current state of this edge. If this edge has a decaying link and
  // one or more parcels at the head of `parcels` fall within the range of
  // SequenceNumbers bound for the decaying link, they'll be enqueued in
  // `parcels_to_decaying_link` and must be sent over `decaying_link_`
  // ASAP.
  //
  // If there are additional parcels at the head of the queue beyond those bound
  // for the decaying link, and this edge has a valid primary link, then those
  // parcels will be popped and enqueued in `parcels_to_primary_link` and must
  // be sent over `primary_link_` ASAP.
  using FlushedParcelQueue = absl::InlinedVector<Parcel, 2>;
  void FlushParcelsFromQueue(ParcelQueue& parcels,
                             FlushedParcelQueue& parcels_to_decaying_link,
                             FlushedParcelQueue& parcels_to_primary_link);

  // Attempts to lock the primary link so that the router on this side of it can
  // coordinate its own bypass.
  bool TryLockPrimaryLinkForBypass(const NodeName& bypass_request_source = {});

  // Indicates whether bypass of this link can legitimately be attempted by
  // a request from `bypass_request_source`. This only returns true if the edge
  // has a central primary link to a proxy who has already initiated bypass with
  // its inward peer router living on the named node.
  bool CanNodeRequestBypassOfPrimaryLink(const NodeName& bypass_request_source);

  // Begins decaying the primary link, optionally setting the final sequence
  // length to and from the decaying link. If there is currently no primary
  // link on this edge, the next primary link it acquires will immediately start
  // decaying. If this edge already has a decaying link, this returns false and
  // nothing changes.
  bool StartDecaying(
      absl::optional<SequenceNumber> length_to_decaying_link = {},
      absl::optional<SequenceNumber> length_from_decaying_link = {});

  // Attempts to reset the decaying link state for this edge. Can succeed if and
  // only if there is currently a decaying link, the sequence length both to and
  // from that link is known, and the given `sequence_length_sent` and
  // `sequence_length_received` are respectively at least as large as those
  // limits.
  bool TryToFinishDecaying(SequenceNumber sequence_length_sent,
                           SequenceNumber sequence_length_received);

  // Logs a description of this RouteEdge for debugging.
  void LogDescription() const;

 private:
  // Indicates whether the given SequenceNumber should be sent on
  // `decaying_link_` if present, or on `primary_link_`.
  bool ShouldSendOnDecayingLink(SequenceNumber n) const;

  // The primary link over which this edge transmits and accepts parcels and
  // control messages. If a decaying link is also present, it is preferred for
  // transmission of all parcels with a SequenceNumber up to but not including
  // `length_to_decaying_link_`.
  Ref<RouterLink> primary_link_;

  // Indicates whether this edge has set its primary link to decay before having
  // a primary link. In this case, the next primary link assigned to this edge
  // will begin to decay immediately.
  bool was_decay_deferred_ = false;

  // If non-null, this is a link to a router which may still send and receive
  // parcels across the edge, limited by definite values in
  // `length_to_decaying_link_` and `length_from_decaying_link_` when present.
  // Once this RouteEdge has facilitated the transmission of all such parcels,
  // `decaying_link_` is reset along with the optional length fields, and
  Ref<RouterLink> decaying_link_;

  // If present, the length of the parcel sequence after which this edge must
  // stop forwarding parcels along `decaying_link_`. If `decaying_link_` is
  // present and this is null, the limit is indefinite and parcels will continue
  // to be transmitted over `decaying_link_` until this gets a value.
  absl::optional<SequenceNumber> length_to_decaying_link_;

  // If present, the length of the parcel sequence after which this edge can
  // stop expecting parcels to be received from `decaying_link_`. If
  // `decaying_link_` is present and this is null, the limit is indefinite and
  // parcels must continue to be accepted from `decaying_link_` until this gets
  // a value.
  absl::optional<SequenceNumber> length_from_decaying_link_;
};

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_ROUTE_EDGE_
