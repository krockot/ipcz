// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_ROUTING_MODE_H_
#define IPCZ_SRC_CORE_ROUTING_MODE_H_

#include <cstdint>

namespace ipcz {
namespace core {

// Indicates how a portal is currently dealing with incoming and outgoing
// parcels, and how it uses whatever links it has available to other portals.
enum RoutingMode : uint32_t {
  // The portal has at least a peer link or a predecessor link -- possibly
  // both -- and definitely has no successor link. If a peer link is present,
  // all outgoing parcels are sent on it. Incoming parcels may also be received
  // from the peer link, as well as from the predecessor link if present. If
  // only a predecessor link is present, all outgoing parcels are sent on it.
  kActive = 0,

  // The portal has no peer or predecessor link and therefore no place to route
  // outgoing parcels. All outgoing parcels -- from either a Put() call if this
  // portal is exposed to the application (e.g. some portals returned from
  // ConnectNode()) or as forwarded from a successor -- are queued for later
  // transmission. There is no source of incoming parcels in this case, so they
  // are not handled.
  kBuffering = 1,

  // The portal has been closed. It may still have outgoing parcels queued and
  // pending transmission, but any incoming parcels destined for it should be
  // discarded. A closed portal with outgoing parcels queued may persist until
  // it is given a peer or predecessor link on which the parcels can be
  // forwarded.
  kClosed = 2,

  // The portal is in a half-proxying mode. This means it has both a peer and
  // successor link, and definitely no predecessor link. All incoming parcels
  // from the peer are forwarded to the successor, and no outgoing parcels are
  // handled at all.
  //
  // A portal in this mode decays to non-existence as soon as its sucessor and
  // peer can establish a link to bypass it, and it has forwarded all in-flight
  // incoming parcels to its successor.
  //
  // A portal cannot enter half-proxy mode if its peer is currently buffering or
  // half-proxying. In such cases the moved portal will instead switch to
  // full-proxying mode.
  kHalfProxy = 3,

  // The portal is in a full-proxying mode. This means it has a successor link
  // as well as EITHER a peer OR predecessor link; and like half-proxying mode,
  // incoming parcels from the peer (or the predecessor here) are forwarded to
  // the successor; but unlike half-proxying mode, outgoing parcels may also be
  // accepted from the successor and fowarded to the peer or predecessor.
  //
  // Full-proxy mode is always a safe mode for a portal to enter when moving it
  // to extend the route to another node, but it is not ideal since it requires
  // an extra hop in both directions.
  //
  // A full-proxying portal decays into a half-proxying portal as soon as it has
  // a peer link whose other side is not buffering or half-proxying.
  kFullProxy = 4,
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_ROUTING_MODE_H_
