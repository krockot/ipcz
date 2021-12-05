// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/node_link.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "core/node.h"
#include "core/node_link_buffer.h"
#include "core/node_messages.h"
#include "core/portal.h"
#include "core/portal_descriptor.h"
#include "core/remote_router_link.h"
#include "core/router.h"
#include "core/router_link.h"
#include "debug/log.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"

namespace ipcz {
namespace core {

NodeLink::NodeLink(mem::Ref<Node> node,
                   const NodeName& remote_node_name,
                   Node::Type remote_node_type,
                   uint32_t remote_protocol_version,
                   mem::Ref<DriverTransport> transport,
                   os::Memory::Mapping link_memory)
    : node_(std::move(node)),
      remote_node_name_(remote_node_name),
      remote_node_type_(remote_node_type),
      remote_protocol_version_(remote_protocol_version),
      transport_(std::move(transport)),
      link_memory_(std::move(link_memory)) {
  transport_->set_listener(this);
}

NodeLink::~NodeLink() {
  Deactivate();
}

RoutingId NodeLink::AllocateRoutingIds(size_t count) {
  return buffer().AllocateRoutingIds(count);
}

mem::Ref<RouterLink> NodeLink::AddRoute(RoutingId routing_id,
                                        size_t link_state_index,
                                        mem::Ref<Router> router,
                                        RemoteRouterLink::Type link_type) {
  absl::MutexLock lock(&mutex_);
  auto result = routes_.try_emplace(routing_id, router);
  ABSL_ASSERT(result.second);
  return mem::MakeRefCounted<RemoteRouterLink>(
      mem::WrapRefCounted(this), routing_id, link_state_index, link_type);
}

bool NodeLink::RemoveRoute(RoutingId routing_id) {
  absl::MutexLock lock(&mutex_);
  auto it = routes_.find(routing_id);
  if (it == routes_.end()) {
    return false;
  }

  routes_.erase(routing_id);
  return true;
}

mem::Ref<Router> NodeLink::GetRouter(RoutingId routing_id) {
  absl::MutexLock lock(&mutex_);
  auto it = routes_.find(routing_id);
  if (it == routes_.end()) {
    return nullptr;
  }
  return it->second;
}

void NodeLink::Deactivate() {
  {
    absl::MutexLock lock(&mutex_);
    routes_.clear();
    if (!active_) {
      return;
    }

    active_ = false;
  }

  transport_->Deactivate();
}

void NodeLink::Transmit(absl::Span<const uint8_t> data,
                        absl::Span<os::Handle> handles) {
  transport_->TransmitMessage(DriverTransport::Message(data, handles));
}

void NodeLink::RequestIntroduction(const NodeName& name) {
  msg::RequestIntroduction request;
  request.params.name = name;
  Transmit(request);
}

void NodeLink::IntroduceNode(const NodeName& name,
                             mem::Ref<DriverTransport> transport,
                             os::Memory link_buffer_memory) {
  std::vector<uint8_t> serialized_transport_data;
  std::vector<os::Handle> serialized_transport_handles;
  if (transport) {
    IpczResult result = transport->Serialize(serialized_transport_data,
                                             serialized_transport_handles);
    ABSL_ASSERT(result == IPCZ_RESULT_OK);
  }

  const size_t num_memory_handles = link_buffer_memory.is_valid() ? 1 : 0;
  absl::InlinedVector<uint8_t, 256> serialized_data;
  const size_t serialized_size =
      sizeof(msg::IntroduceNode) + serialized_transport_data.size() +
      (serialized_transport_handles.size() + num_memory_handles) *
          sizeof(internal::OSHandleData);
  serialized_data.resize(serialized_size);

  auto& intro = *reinterpret_cast<msg::IntroduceNode*>(serialized_data.data());
  new (&intro) msg::IntroduceNode();
  intro.message_header.size = sizeof(intro.message_header);
  intro.message_header.message_id = msg::IntroduceNode::kId;
  intro.known = (transport != nullptr);
  intro.name = name;
  intro.num_transport_bytes =
      static_cast<uint32_t>(serialized_transport_data.size());
  intro.num_transport_os_handles =
      static_cast<uint32_t>(serialized_transport_handles.size());
  memcpy(&intro + 1, serialized_transport_data.data(),
         serialized_transport_data.size());

  std::vector<os::Handle> handles(serialized_transport_handles.size() +
                                  num_memory_handles);
  if (link_buffer_memory.is_valid()) {
    handles[0] = link_buffer_memory.TakeHandle();
  }
  if (!serialized_transport_handles.empty()) {
    std::move(serialized_transport_handles.begin(),
              serialized_transport_handles.end(), &handles[num_memory_handles]);
  }

  Transmit(absl::MakeSpan(serialized_data), absl::MakeSpan(handles));
}

bool NodeLink::BypassProxy(const NodeName& proxy_name,
                           RoutingId proxy_routing_id,
                           absl::uint128 bypass_key,
                           mem::Ref<Router> new_peer) {
  RoutingId new_routing_id = AllocateRoutingIds(1);
  mem::Ref<RouterLink> new_link =
      AddRoute(new_routing_id, new_routing_id, new_peer,
               RemoteRouterLink::Type::kToOtherSide);

  // The receiver using the other side of this link will always start with a
  // decaying link, so we preemptively mark their side as busy. If our side is
  // busy, the bit will be set in SetOutwardLink() below when we adopt the link.
  RouterLinkState& state = new_link->GetLinkState();
  state.unsafe_sides()[new_peer->side().opposite()].is_blocking_decay = true;

  // We don't want `new_peer` transmitting any outgoing parcels until we've
  // transmitted the BypassProxy message; otherwise the parcels would arrive on
  // an unrecognized route and be rejected.
  new_peer->PauseOutboundTransmission(true);
  const SequenceNumber proxied_outbound_sequence_length =
      new_peer->SetOutwardLink(new_link);

  msg::BypassProxy bypass;
  bypass.params.proxy_name = proxy_name;
  bypass.params.proxy_routing_id = proxy_routing_id;
  bypass.params.new_routing_id = new_routing_id;
  bypass.params.bypass_key = bypass_key;
  bypass.params.proxied_outbound_sequence_length =
      proxied_outbound_sequence_length;
  Transmit(bypass);

  new_peer->PauseOutboundTransmission(false);
  return true;
}

IpczResult NodeLink::OnTransportMessage(
    const DriverTransport::Message& message) {
  const auto& header =
      *reinterpret_cast<const internal::MessageHeader*>(message.data.data());

  switch (header.message_id) {
    case msg::AcceptParcel::kId:
      if (OnAcceptParcel(message)) {
        return IPCZ_RESULT_OK;
      }
      return IPCZ_RESULT_INVALID_ARGUMENT;

    case msg::SideClosed::kId: {
      msg::SideClosed side_closed;
      if (side_closed.Deserialize(message) && OnSideClosed(side_closed)) {
        return IPCZ_RESULT_OK;
      }
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    case msg::RequestIntroduction::kId: {
      msg::RequestIntroduction request;
      if (request.Deserialize(message) &&
          node_->OnRequestIntroduction(*this, request)) {
        return IPCZ_RESULT_OK;
      }
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    case msg::IntroduceNode::kId:
      if (OnIntroduceNode(message)) {
        return IPCZ_RESULT_OK;
      }
      return IPCZ_RESULT_INVALID_ARGUMENT;

    case msg::BypassProxy::kId: {
      msg::BypassProxy bypass;
      if (bypass.Deserialize(message) && node_->OnBypassProxy(*this, bypass)) {
        return IPCZ_RESULT_OK;
      }
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    case msg::StopProxying::kId: {
      msg::StopProxying stop;
      if (stop.Deserialize(message) && OnStopProxying(stop)) {
        return IPCZ_RESULT_OK;
      }
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    case msg::StopProxyingToLocalPeer::kId: {
      msg::StopProxyingToLocalPeer stop;
      if (stop.Deserialize(message) && OnStopProxyingToLocalPeer(stop)) {
        return IPCZ_RESULT_OK;
      }
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    case msg::InitiateProxyBypass::kId: {
      msg::InitiateProxyBypass request;
      if (request.Deserialize(message) && OnInitiateProxyBypass(request)) {
        return IPCZ_RESULT_OK;
      }
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    case msg::BypassProxyToSameNode::kId: {
      msg::BypassProxyToSameNode bypass;
      if (bypass.Deserialize(message) && OnBypassProxyToSameNode(bypass)) {
        return IPCZ_RESULT_OK;
      }
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    case msg::ProxyWillStop::kId: {
      msg::ProxyWillStop will_stop;
      if (will_stop.Deserialize(message) && OnProxyWillStop(will_stop)) {
        return IPCZ_RESULT_OK;
      }
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    case msg::DecayUnblocked::kId: {
      msg::DecayUnblocked unblocked;
      if (unblocked.Deserialize(message) && OnDecayUnblocked(unblocked)) {
        return IPCZ_RESULT_OK;
      }
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    case msg::LogRouteTrace::kId: {
      msg::LogRouteTrace log_request;
      if (log_request.Deserialize(message) && OnLogRouteTrace(log_request)) {
        return IPCZ_RESULT_OK;
      }
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    default:
      DLOG(WARNING) << "Ignoring unknown transport message with ID "
                    << static_cast<int>(header.message_id);
      break;
  }

  return IPCZ_RESULT_OK;
}

void NodeLink::OnTransportError() {}

bool NodeLink::OnAcceptParcel(const DriverTransport::Message& message) {
  if (message.data.size() < sizeof(msg::AcceptParcel)) {
    return false;
  }
  const auto& accept =
      *reinterpret_cast<const msg::AcceptParcel*>(message.data.data());
  const uint32_t num_bytes = accept.num_bytes;
  const uint32_t num_portals = accept.num_portals;
  const uint32_t num_os_handles = accept.num_os_handles;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&accept + 1);
  const auto* descriptors = reinterpret_cast<const PortalDescriptor*>(
      bytes + IPCZ_ALIGNED(num_bytes, 16));

  if (message.handles.size() != num_os_handles) {
    return false;
  }

  Parcel::PortalVector portals(num_portals);
  for (size_t i = 0; i < num_portals; ++i) {
    portals[i] = mem::MakeRefCounted<Portal>(
        node_, Router::Deserialize(descriptors[i], *this));
  }

  std::vector<os::Handle> os_handles(num_os_handles);
  for (size_t i = 0; i < num_os_handles; ++i) {
    os_handles[i] = std::move(message.handles[i]);
  }

  Parcel parcel(accept.sequence_number);
  parcel.SetData(std::vector<uint8_t>(bytes, bytes + num_bytes));
  parcel.SetPortals(std::move(portals));
  parcel.SetOSHandles(std::move(os_handles));
  mem::Ref<Router> receiver = GetRouter(accept.routing_id);
  if (!receiver) {
    return true;
  }

  return receiver->AcceptParcelFrom(*this, accept.routing_id, parcel);
}

bool NodeLink::OnSideClosed(const msg::SideClosed& side_closed) {
  mem::Ref<Router> receiver = GetRouter(side_closed.params.routing_id);
  if (!receiver) {
    return true;
  }

  receiver->AcceptRouteClosure(side_closed.params.side,
                               side_closed.params.sequence_length);
  return true;
}

bool NodeLink::OnIntroduceNode(const DriverTransport::Message& message) {
  if (remote_node_type_ != Node::Type::kBroker) {
    return false;
  }

  if (message.data.size() < sizeof(msg::IntroduceNode)) {
    return false;
  }
  const auto& intro =
      *reinterpret_cast<const msg::IntroduceNode*>(message.data.data());
  const uint32_t num_transport_bytes = intro.num_transport_bytes;
  const uint32_t num_transport_os_handles = intro.num_transport_os_handles;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&intro + 1);

  const size_t serialized_size =
      sizeof(intro) + num_transport_bytes +
      (num_transport_os_handles + 1) * sizeof(internal::OSHandleData);
  if (message.data.size() < serialized_size) {
    return false;
  }
  if (message.handles.size() != num_transport_os_handles + 1) {
    return false;
  }

  os::Memory link_buffer_memory(std::move(message.handles[0]),
                                sizeof(NodeLinkBuffer));
  return node_->OnIntroduceNode(
      intro.name, intro.known, std::move(link_buffer_memory),
      absl::Span<const uint8_t>(bytes, num_transport_bytes),
      message.handles.subspan(1));
}

bool NodeLink::OnStopProxying(const msg::StopProxying& stop) {
  mem::Ref<Router> router = GetRouter(stop.params.routing_id);
  if (!router) {
    DVLOG(4) << "Received StopProxying for unknown route";
    return true;
  }

  DVLOG(4) << "Received StopProxying on " << node_->name().ToString()
           << " routing ID " << stop.params.routing_id << " with inbound"
           << " length " << stop.params.inbound_sequence_length
           << " and outbound length " << stop.params.outbound_sequence_length;

  return router->StopProxying(stop.params.inbound_sequence_length,
                              stop.params.outbound_sequence_length);
}

bool NodeLink::OnInitiateProxyBypass(const msg::InitiateProxyBypass& request) {
  mem::Ref<Router> router = GetRouter(request.params.routing_id);
  if (!router) {
    return true;
  }

  return router->InitiateProxyBypass(
      *this, request.params.routing_id, request.params.proxy_peer_name,
      request.params.proxy_peer_routing_id, request.params.bypass_key);
}

bool NodeLink::OnBypassProxyToSameNode(
    const msg::BypassProxyToSameNode& bypass) {
  mem::Ref<Router> router = GetRouter(bypass.params.routing_id);
  if (!router) {
    return true;
  }

  mem::Ref<RouterLink> new_link =
      AddRoute(bypass.params.new_routing_id, bypass.params.new_routing_id,
               router, RemoteRouterLink::Type::kToOtherSide);
  return router->BypassProxyWithNewLinkToSameNode(
      std::move(new_link), bypass.params.sequence_length);
}

bool NodeLink::OnStopProxyingToLocalPeer(
    const msg::StopProxyingToLocalPeer& stop) {
  mem::Ref<Router> router = GetRouter(stop.params.routing_id);
  if (!router) {
    return true;
  }
  return router->StopProxyingToLocalPeer(stop.params.sequence_length);
}

bool NodeLink::OnProxyWillStop(const msg::ProxyWillStop& will_stop) {
  mem::Ref<Router> router = GetRouter(will_stop.params.routing_id);
  if (!router) {
    return true;
  }

  return router->OnProxyWillStop(will_stop.params.sequence_length);
}

bool NodeLink::OnDecayUnblocked(const msg::DecayUnblocked& unblocked) {
  mem::Ref<Router> router = GetRouter(unblocked.params.routing_id);
  if (!router) {
    return true;
  }

  return router->OnDecayUnblocked();
}

bool NodeLink::OnLogRouteTrace(const msg::LogRouteTrace& log_request) {
  mem::Ref<Router> router = GetRouter(log_request.params.routing_id);
  if (!router) {
    return true;
  }

  router->LogRouteTrace(log_request.params.toward_side);
  return true;
}

}  // namespace core
}  // namespace ipcz
