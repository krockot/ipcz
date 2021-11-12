// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/portal_link.h"

#include <utility>

#include "core/node_link.h"

namespace ipcz {
namespace core {

PortalLink::PortalLink(mem::Ref<NodeLink> node_link,
                       RoutingId routing_id,
                       os::Memory::Mapping state_mapping)
    : node_link_(std::move(node_link)),
      routing_id_(routing_id),
      state_mapping_(std::move(state_mapping)) {}

PortalLink::~PortalLink() = default;

void PortalLink::Disconnect() {
  node_link_->DisconnectRoutingId(routing_id_);
}

void PortalLink::AcceptParcel(Parcel& parcel) {
  node_link_->AcceptParcel(routing_id_, parcel);
}

void PortalLink::SideClosed(Side side, SequenceNumber sequence_length) {
  node_link_->SideClosed(routing_id_, side, sequence_length);
}

void PortalLink::StopProxyingTowardSide(Side side,
                                        SequenceNumber sequence_length) {
  node_link_->StopProxyingTowardSide(routing_id_, side, sequence_length);
}

void PortalLink::InitiateProxyBypass(const NodeName& proxy_peer_name,
                                     RoutingId proxy_peer_routing_id,
                                     absl::uint128 bypass_key) {
  node_link_->InitiateProxyBypass(routing_id_, proxy_peer_name,
                                  proxy_peer_routing_id, bypass_key);
}

}  // namespace core
}  // namespace ipcz
