// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_PORTAL_IN_TRANSIT_H_
#define IPCZ_SRC_CORE_PORTAL_IN_TRANSIT_H_

#include "core/route_id.h"
#include "core/side.h"
#include "mem/ref_counted.h"
#include "os/memory.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace ipcz {
namespace core {

class Portal;
class PortalLink;

struct PortalInTransit {
  PortalInTransit();
  PortalInTransit(PortalInTransit&&);
  PortalInTransit& operator=(PortalInTransit&&);
  ~PortalInTransit();

  mem::Ref<Portal> portal;
  Side side;

  // The route assigned to this portal along the transmitting NodeLink, if the
  // parcel carring this portal was actually transmitted. On the receiving side
  // of portal transit this is deserialized as the peer link.
  //
  // On the sending side (after serialization and parcel transmission) this is
  // the forwarding link. See Portal::FinishAfterTransit() for how it's used in
  // that case.
  mem::Ref<PortalLink> link;

  // The portal's local peer prior to initiating transit. Transit may be
  // cancelled before the containing parcel is shipped off, and if this portal
  // was part of a local pair prior to the transit attempt, we use this link to
  // restore both portals to a working state.
  mem::Ref<Portal> local_peer_before_transit;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_PORTAL_IN_TRANSIT_H_