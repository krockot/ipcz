// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/remote_router_link.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "core/node_link.h"
#include "core/node_link_buffer.h"
#include "core/node_messages.h"
#include "core/parcel.h"
#include "core/portal.h"
#include "core/portal_descriptor.h"
#include "core/router.h"
#include "core/router_link_state.h"
#include "core/routing_id.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

RemoteRouterLink::RemoteRouterLink(mem::Ref<NodeLink> node_link,
                                   RoutingId routing_id,
                                   uint32_t link_state_index,
                                   Type type)
    : node_link_(std::move(node_link)),
      routing_id_(routing_id),
      link_state_index_(link_state_index),
      type_(type) {}

RemoteRouterLink::~RemoteRouterLink() = default;

void RemoteRouterLink::Deactivate() {
  node_link_->RemoveRoute(routing_id_);
}

RouterLinkState& RemoteRouterLink::GetLinkState() {
  return node_link_->buffer().router_link_state(link_state_index_);
}

mem::Ref<Router> RemoteRouterLink::GetLocalTarget() {
  return nullptr;
}

bool RemoteRouterLink::IsRemoteLinkTo(NodeLink& node_link,
                                      RoutingId routing_id) {
  return node_link_.get() == &node_link && routing_id_ == routing_id;
}

bool RemoteRouterLink::IsLinkToOtherSide() {
  return type_ == Type::kToOtherSide;
}

bool RemoteRouterLink::WouldParcelExceedLimits(size_t data_size,
                                               const IpczPutLimits& limits) {
  // TODO
  return false;
}

void RemoteRouterLink::AcceptParcel(Parcel& parcel) {
  absl::InlinedVector<uint8_t, 256> serialized_data;
  const size_t num_portals = parcel.portals_view().size();
  const size_t num_os_handles = parcel.os_handles_view().size();
  const size_t serialized_size =
      sizeof(msg::AcceptParcel) + IPCZ_ALIGNED(parcel.data_view().size(), 16) +
      num_portals * sizeof(PortalDescriptor) +
      num_os_handles * sizeof(internal::OSHandleData);
  serialized_data.resize(serialized_size);

  auto& accept = *reinterpret_cast<msg::AcceptParcel*>(serialized_data.data());
  accept.message_header.size = sizeof(accept.message_header);
  accept.message_header.message_id = msg::AcceptParcel::kId;
  accept.routing_id = routing_id_;
  accept.sequence_number = parcel.sequence_number();
  accept.num_bytes = parcel.data_view().size();
  accept.num_portals = static_cast<uint32_t>(num_portals);
  accept.num_os_handles = static_cast<uint32_t>(num_os_handles);
  auto* data = reinterpret_cast<uint8_t*>(&accept + 1);
  memcpy(data, parcel.data_view().data(), parcel.data_view().size());
  auto* descriptors = reinterpret_cast<PortalDescriptor*>(
      data + IPCZ_ALIGNED(accept.num_bytes, 16));

  const absl::Span<mem::Ref<Portal>> portals = parcel.portals_view();
  const RoutingId first_routing_id =
      node_link()->AllocateRoutingIds(num_portals);
  std::vector<os::Handle> os_handles;
  std::vector<mem::Ref<Router>> routers(num_portals);
  std::vector<mem::Ref<RouterLink>> new_links(num_portals);
  os_handles.reserve(num_os_handles);
  for (size_t i = 0; i < num_portals; ++i) {
    const RoutingId routing_id = first_routing_id + i;
    RouterLinkState& state =
        node_link()->buffer().router_link_state(routing_id);
    RouterLinkState::Initialize(&state);
    descriptors[i].new_routing_id = routing_id;
    routers[i] = portals[i]->router();
    mem::Ref<Router> route_listener = routers[i]->Serialize(descriptors[i]);
    if (descriptors[i].route_is_peer) {
      descriptors[i].new_decaying_routing_id =
          node_link()->AllocateRoutingIds(1);
    }
    new_links[i] = node_link()->AddRoute(
        routing_id, routing_id, std::move(route_listener),
        descriptors[i].route_is_peer ? RemoteRouterLink::Type::kToOtherSide
                                     : RemoteRouterLink::Type::kToSameSide);
  }

  node_link()->Transmit(absl::MakeSpan(serialized_data),
                        parcel.os_handles_view());

  for (size_t i = 0; i < num_portals; ++i) {
    mem::Ref<RouterLink> decaying_link;
    if (descriptors[i].route_is_peer) {
      decaying_link = node_link()->AddRoute(
          descriptors[i].new_decaying_routing_id,
          descriptors[i].new_decaying_routing_id, routers[i],
          RemoteRouterLink::Type::kToSameSide);
    }
    routers[i]->BeginProxying(descriptors[i], std::move(new_links[i]),
                              std::move(decaying_link));
    portals[i].reset();
  }
}

void RemoteRouterLink::AcceptRouteClosure(Side side,
                                          SequenceNumber sequence_length) {
  msg::SideClosed side_closed;
  side_closed.params.routing_id = routing_id_;
  side_closed.params.side = side;
  side_closed.params.sequence_length = sequence_length;
  node_link()->Transmit(side_closed);
}

void RemoteRouterLink::StopProxying(SequenceNumber inbound_sequence_length,
                                    SequenceNumber outbound_sequence_length) {
  msg::StopProxying stop;
  stop.params.routing_id = routing_id_;
  stop.params.inbound_sequence_length = inbound_sequence_length;
  stop.params.outbound_sequence_length = outbound_sequence_length;
  node_link()->Transmit(stop);
}

void RemoteRouterLink::RequestProxyBypassInitiation(
    const NodeName& to_new_peer,
    RoutingId proxy_peer_routing_id,
    const absl::uint128& bypass_key) {
  msg::InitiateProxyBypass request;
  request.params.routing_id = routing_id_;
  request.params.proxy_peer_name = to_new_peer;
  request.params.proxy_peer_routing_id = proxy_peer_routing_id;
  request.params.bypass_key = bypass_key;
  node_link()->Transmit(request);
}

void RemoteRouterLink::BypassProxyToSameNode(RoutingId new_routing_id,
                                             SequenceNumber sequence_length) {
  msg::BypassProxyToSameNode bypass;
  bypass.params.routing_id = routing_id_;
  bypass.params.new_routing_id = new_routing_id;
  bypass.params.sequence_length = sequence_length;
  node_link()->Transmit(bypass);
}

void RemoteRouterLink::StopProxyingToLocalPeer(SequenceNumber sequence_length) {
  msg::StopProxyingToLocalPeer stop;
  stop.params.routing_id = routing_id_;
  stop.params.sequence_length = sequence_length;
  node_link()->Transmit(stop);
}

void RemoteRouterLink::LogRouteTrace(Side toward_side) {
  msg::LogRouteTrace log_request;
  log_request.params.routing_id = routing_id_;
  log_request.params.toward_side = toward_side;
  node_link()->Transmit(log_request);
}

}  // namespace core
}  // namespace ipcz
