// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/portal_link.h"

#include <utility>

#include "core/node_link.h"

namespace ipcz {
namespace core {

PortalLink::PortalLink(mem::Ref<NodeLink> node,
                       RouteId route,
                       os::Memory::Mapping state_mapping)
    : node_(std::move(node)),
      route_(route),
      state_mapping_(std::move(state_mapping)) {}

PortalLink::~PortalLink() = default;

void PortalLink::SendParcel(Parcel& parcel) {
  node_->SendParcel(route_, parcel);
}

void PortalLink::NotifyClosed(SequenceNumber sequence_length) {
  node_->SendPeerClosed(route_, sequence_length);
}

}  // namespace core
}  // namespace ipcz
