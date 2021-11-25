// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_ROUTING_MODE_H_
#define IPCZ_SRC_CORE_ROUTING_MODE_H_

#include <cstdint>

namespace ipcz {
namespace core {

// Indicates how a Router is currently dealing with incoming and outgoing
// parcels, and how it uses whatever links it has available to other Routers.
enum RoutingMode : uint32_t {
  // The Router is bound to a Portal which may use it to transmit outgoing
  // parcels to this Router's peer (preferred) or its predecessor. If the Router
  // has neither a peer nor a predecessor, outgoing parcels are queued within
  // Router until this changes.
  //
  // Incoming parcels may come from the peer or predecessor link if present, and
  // they are queued and exposed to the Portal for retrieval by the embedding
  // application.
  kActive = 0,

  // The Router is in a half-proxy mode. This means it expects and handles no
  // outgoing parcels, but incoming parcels -- which may come from a peer or
  // predecessor -- are forwarded to its successor if present. If no successor
  // is present yet, incoming parcels are queued until this changes.
  //
  // A Router is always given a successor shortly after switching to half-proxy
  // mode, and it will only continue to exist beyond that point until it has
  // forwarded its last message to its successor, either because the other side
  // of the route has closed or because the successor has conspired successfully
  // with a peer Router to bypass this one.
  //
  // Only one side of a route may have half-proxying Routers at any moment. A
  // Router can only enter half-proxy mode by decaying from full proxy mode (see
  // below) or if it extends its own side of the route while its peer is already
  // known to be closed.
  kHalfProxy = 2,

  // The Router is in a full proxy mode. Any outgoing parcels received from its
  // successor (if present) are forwarded to its peer or predecessor if present.
  // If the Router has no peer or predecessor, outgoing parcels are queued
  // within the Router until this changes.
  //
  // Incoming parcels received from the Router's peer or predecessor are
  // forwarded to its successor, or queued within the Router until it has a
  // successor.
  //
  // A proxy decays into a half-proxy as soon as it acquires a peer link to
  // another Router which is not a half-proxy.
  kProxy = 3,
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_ROUTING_MODE_H_
