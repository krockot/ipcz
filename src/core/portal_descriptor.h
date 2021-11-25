// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_PORTAL_DESCRIPTOR_H_
#define IPCZ_SRC_CORE_PORTAL_DESCRIPTOR_H_

#include "core/routing_id.h"
#include "core/sequence_number.h"
#include "core/side.h"
#include "ipcz/ipcz.h"

namespace ipcz {
namespace core {

// Serialized representation of a Portal sent in a parcel.
struct IPCZ_ALIGN(16) PortalDescriptor {
  PortalDescriptor();
  PortalDescriptor(const PortalDescriptor&);
  PortalDescriptor& operator=(const PortalDescriptor&);
  ~PortalDescriptor();

  Side side;
  bool route_is_peer : 1;
  RoutingId new_routing_id;
  SequenceNumber next_outgoing_sequence_number;
  SequenceNumber next_incoming_sequence_number;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_PORTAL_DESCRIPTOR_H_
