// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/node_link.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "core/direction.h"
#include "core/node.h"
#include "core/node_messages.h"
#include "core/portal.h"
#include "core/remote_router_link.h"
#include "core/router.h"
#include "core/router_descriptor.h"
#include "core/router_link.h"
#include "debug/log.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"

namespace ipcz {
namespace core {

// static
mem::Ref<NodeLink> NodeLink::Create(mem::Ref<Node> node,
                                    const NodeName& local_node_name,
                                    const NodeName& remote_node_name,
                                    Node::Type remote_node_type,
                                    uint32_t remote_protocol_version,
                                    mem::Ref<DriverTransport> transport,
                                    mem::Ref<NodeLinkMemory> memory) {
  auto link = mem::WrapRefCounted(new NodeLink(
      std::move(node), local_node_name, remote_node_name, remote_node_type,
      remote_protocol_version, std::move(transport), std::move(memory)));
  link->memory().SetNodeLink(link);
  return link;
}

NodeLink::NodeLink(mem::Ref<Node> node,
                   const NodeName& local_node_name,
                   const NodeName& remote_node_name,
                   Node::Type remote_node_type,
                   uint32_t remote_protocol_version,
                   mem::Ref<DriverTransport> transport,
                   mem::Ref<NodeLinkMemory> memory)
    : node_(std::move(node)),
      local_node_name_(local_node_name),
      remote_node_name_(remote_node_name),
      remote_node_type_(remote_node_type),
      remote_protocol_version_(remote_protocol_version),
      transport_(std::move(transport)),
      memory_(std::move(memory)) {
  transport_->set_listener(this);
}

NodeLink::~NodeLink() {
  Deactivate();
}

mem::Ref<RemoteRouterLink> NodeLink::AddRoute(
    RoutingId routing_id,
    const NodeLinkAddress& link_state_address,
    LinkType type,
    LinkSide side,
    mem::Ref<Router> router) {
  auto link = RemoteRouterLink::Create(mem::WrapRefCounted(this), routing_id,
                                       link_state_address, type, side);

  absl::MutexLock lock(&mutex_);
  auto result = routes_.try_emplace(routing_id,
                                    Route(std::move(link), std::move(router)));
  return result.first->second.link;
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

absl::optional<NodeLink::Route> NodeLink::GetRoute(RoutingId routing_id) {
  absl::MutexLock lock(&mutex_);
  auto it = routes_.find(routing_id);
  if (it == routes_.end()) {
    return absl::nullopt;
  }
  return it->second;
}

mem::Ref<Router> NodeLink::GetRouter(RoutingId routing_id) {
  absl::MutexLock lock(&mutex_);
  auto it = routes_.find(routing_id);
  if (it == routes_.end()) {
    return nullptr;
  }
  return it->second.receiver;
}

void NodeLink::Deactivate() {
  RouteMap routes;
  {
    absl::MutexLock lock(&mutex_);
    routes = std::move(routes_);
    if (!active_) {
      return;
    }

    active_ = false;
  }

  memory_->SetNodeLink(nullptr);
  routes.clear();
  transport_->Deactivate();
}

void NodeLink::Transmit(absl::Span<const uint8_t> data,
                        absl::Span<os::Handle> handles) {
  transport_->TransmitMessage(DriverTransport::Message(data, handles));
}

void NodeLink::RequestIndirectBrokerConnection(
    mem::Ref<DriverTransport> transport,
    os::Process new_node_process,
    size_t num_initial_portals,
    IndirectBrokerConnectionCallback callback) {
  std::vector<uint8_t> serialized_transport_data;
  std::vector<os::Handle> serialized_transport_handles;
  if (transport) {
    IpczResult result = transport->Serialize(serialized_transport_data,
                                             serialized_transport_handles);
    ABSL_ASSERT(result == IPCZ_RESULT_OK);
  }

  uint64_t request_id;
  {
    absl::MutexLock lock(&mutex_);
    request_id = next_request_id_++;
    pending_indirect_broker_connections_[request_id] = std::move(callback);
  }

  absl::InlinedVector<uint8_t, 256> serialized_data;
  size_t num_os_handles = serialized_transport_handles.size() + 1;
  const size_t serialized_size =
      sizeof(msg::RequestIndirectBrokerConnection) +
      serialized_transport_data.size() +
      num_os_handles * sizeof(internal::OSHandleData);
  serialized_data.resize(serialized_size);

  auto& request = *reinterpret_cast<msg::RequestIndirectBrokerConnection*>(
      serialized_data.data());
  new (&request) msg::RequestIndirectBrokerConnection();
  request.message_header.size = sizeof(request.message_header);
  request.message_header.message_id = msg::RequestIndirectBrokerConnection::kId;
  request.request_id = request_id;
  request.num_initial_portals = static_cast<uint32_t>(num_initial_portals);
  request.num_transport_bytes =
      static_cast<uint32_t>(serialized_transport_data.size());
  request.num_transport_os_handles =
      static_cast<uint32_t>(serialized_transport_handles.size());
  memcpy(&request + 1, serialized_transport_data.data(),
         serialized_transport_data.size());

  std::vector<os::Handle> handles(num_os_handles);
  handles[0] = new_node_process.TakeAsHandle();
  for (size_t i = 0; i < serialized_transport_handles.size(); ++i) {
    handles[i + 1] = std::move(serialized_transport_handles[i]);
  }

  Transmit(absl::MakeSpan(serialized_data), absl::MakeSpan(handles));
}

void NodeLink::RequestIntroduction(const NodeName& name) {
  msg::RequestIntroduction request;
  request.params().name = name;
  Transmit(request);
}

void NodeLink::IntroduceNode(const NodeName& name,
                             mem::Ref<DriverTransport> transport,
                             os::Memory link_buffer_memory) {
  std::vector<uint8_t> transport_data;
  std::vector<os::Handle> transport_handles;
  if (transport) {
    IpczResult result = transport->Serialize(transport_data, transport_handles);
    ABSL_ASSERT(result == IPCZ_RESULT_OK);
  }

  msg::IntroduceNode intro;
  intro.params().name = name;
  intro.params().known = (transport != nullptr);
  intro.params().transport_data =
      intro.AllocateArray<uint8_t>(transport_data.size());
  memcpy(intro.GetArrayData(intro.params().transport_data),
         transport_data.data(), transport_data.size());
  intro.params().transport_os_handles =
      intro.AppendHandles(absl::MakeSpan(transport_handles));

  std::vector<os::Handle> buffer_handles;
  if (link_buffer_memory.is_valid()) {
    buffer_handles.push_back(link_buffer_memory.TakeHandle());
  }
  intro.params().buffer_handle =
      intro.AppendHandles(absl::MakeSpan(buffer_handles));

  Transmit(intro);
}

bool NodeLink::BypassProxy(const NodeName& proxy_name,
                           RoutingId proxy_routing_id,
                           SequenceNumber proxy_outbound_sequence_length,
                           mem::Ref<Router> new_peer) {
  // Note that by convention the side which initiates a bypass (this side)
  // adopts side A of the new bypass link. The other end adopts side B.
  const RoutingId new_routing_id = memory().AllocateRoutingIds(1);
  const NodeLinkAddress new_link_state_address =
      memory().AllocateRouterLinkState();
  mem::Ref<RouterLink> new_link =
      AddRoute(new_routing_id, new_link_state_address, LinkType::kCentral,
               LinkSide::kA, new_peer);

  DVLOG(4) << "Sending BypassProxy from " << local_node_name_.ToString()
           << " to " << remote_node_name_.ToString() << " with new routing ID "
           << new_routing_id << " to replace its link to proxy "
           << proxy_name.ToString() << " on routing ID " << proxy_routing_id;

  msg::BypassProxy bypass;
  bypass.params().proxy_name = proxy_name;
  bypass.params().proxy_routing_id = proxy_routing_id;
  bypass.params().new_routing_id = new_routing_id;
  bypass.params().new_link_state_address = new_link_state_address;
  bypass.params().proxy_outbound_sequence_length =
      proxy_outbound_sequence_length;
  Transmit(bypass);

  // This link is only provided after we transmit the bypass request, ensuring
  // that `new_peer` doesn't send anything else over the link until the bypass
  // has been accepted by the remote node.
  new_peer->SetOutwardLink(new_link);

  return true;
}

void NodeLink::AddLinkBuffer(BufferId buffer_id, os::Memory memory) {
  msg::AddLinkBuffer add;
  add.params().buffer_id = buffer_id;
  add.params().buffer_size = static_cast<uint32_t>(memory.size());
  add.params().buffer_handle = add.AppendHandle(memory.TakeHandle());
  Transmit(add);
}

void NodeLink::RequestMemory(uint32_t size, RequestMemoryCallback callback) {
  {
    absl::MutexLock lock(&mutex_);
    pending_memory_requests_[size].push_back(std::move(callback));
  }

  msg::RequestMemory request;
  request.params().size = size;
  Transmit(request);
}

IpczResult NodeLink::OnTransportMessage(
    const DriverTransport::Message& message) {
  const auto& header =
      *reinterpret_cast<const internal::MessageHeader*>(message.data.data());

  switch (header.message_id) {
    case msg::RequestIndirectBrokerConnection::kId:
      if (OnRequestIndirectBrokerConnection(message)) {
        return IPCZ_RESULT_OK;
      }
      return IPCZ_RESULT_INVALID_ARGUMENT;

    case msg::AcceptIndirectBrokerConnection::kId: {
      msg::AcceptIndirectBrokerConnection accept;
      if (accept.Deserialize(message) &&
          OnAcceptIndirectBrokerConnection(accept)) {
        return IPCZ_RESULT_OK;
      }
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    case msg::AcceptParcel::kId: {
      msg::AcceptParcel accept;
      if (accept.Deserialize(message) && OnAcceptParcel(accept)) {
        return IPCZ_RESULT_OK;
      }
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    case msg::RouteClosed::kId: {
      msg::RouteClosed route_closed;
      if (route_closed.Deserialize(message) && OnRouteClosed(route_closed)) {
        return IPCZ_RESULT_OK;
      }
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    case msg::SetRouterLinkStateAddress::kId: {
      msg::SetRouterLinkStateAddress set;
      if (set.Deserialize(message) && OnSetRouterLinkStateAddress(set)) {
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

    case msg::IntroduceNode::kId: {
      msg::IntroduceNode intro;
      if (intro.Deserialize(message) && OnIntroduceNode(intro)) {
        return IPCZ_RESULT_OK;
      }
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    case msg::AddLinkBuffer::kId: {
      msg::AddLinkBuffer add;
      if (add.Deserialize(message) && OnAddLinkBuffer(add)) {
        return IPCZ_RESULT_OK;
      }
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

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

    case msg::NotifyBypassPossible::kId: {
      msg::NotifyBypassPossible notify;
      if (notify.Deserialize(message) && OnNotifyBypassPossible(notify)) {
        return IPCZ_RESULT_OK;
      }
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    case msg::RequestMemory::kId: {
      msg::RequestMemory request;
      if (request.Deserialize(message) && OnRequestMemory(request)) {
        return IPCZ_RESULT_OK;
      }
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    case msg::ProvideMemory::kId: {
      msg::ProvideMemory provide;
      if (provide.Deserialize(message) && OnProvideMemory(provide)) {
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

bool NodeLink::OnRequestIndirectBrokerConnection(
    const DriverTransport::Message& message) {
  if (node_->type() != Node::Type::kBroker) {
    return false;
  }

  if (message.data.size() < sizeof(msg::RequestIndirectBrokerConnection)) {
    return false;
  }
  const auto& request =
      *reinterpret_cast<const msg::RequestIndirectBrokerConnection*>(
          message.data.data());
  const uint32_t num_transport_bytes = request.num_transport_bytes;
  const uint32_t num_transport_os_handles = request.num_transport_os_handles;
  const uint32_t num_os_handles = num_transport_os_handles + 1;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&request + 1);

  const size_t serialized_size =
      sizeof(request) + num_transport_bytes +
      num_os_handles * sizeof(internal::OSHandleData);
  if (message.data.size() < serialized_size) {
    return false;
  }
  if (message.handles.size() < num_transport_os_handles) {
    return false;
  }

  const size_t num_extra_handles =
      message.handles.size() - num_transport_os_handles;
  if (num_extra_handles > 1) {
    return false;
  }

  os::Process new_node_process;
#if defined(OS_WIN) || defined(OS_FUCHSIA)
  if (num_extra_handles == 1 && message.handles[0]) {
    new_node_process = os::Process(message.handles[0].release());
  }
#endif

  auto transport = DriverTransport::Deserialize(
      node_->driver(), node_->driver_node(),
      absl::Span<const uint8_t>(bytes, num_transport_bytes),
      message.handles.subspan(num_extra_handles));
  if (!transport) {
    return false;
  }

  return node_->OnRequestIndirectBrokerConnection(
      *this, request.request_id, std::move(transport),
      std::move(new_node_process), request.num_initial_portals);
}

bool NodeLink::OnAcceptIndirectBrokerConnection(
    const msg::AcceptIndirectBrokerConnection& accept) {
  if (node_->type() == Node::Type::kBroker ||
      remote_node_type_ != Node::Type::kBroker) {
    return false;
  }

  IndirectBrokerConnectionCallback callback;
  {
    absl::MutexLock lock(&mutex_);
    auto it =
        pending_indirect_broker_connections_.find(accept.params().request_id);
    if (it == pending_indirect_broker_connections_.end()) {
      return false;
    }

    callback = std::move(it->second);
    pending_indirect_broker_connections_.erase(it);
  }

  if (!accept.params().success) {
    callback(NodeName(), 0);
  } else {
    callback(accept.params().connected_node_name,
             accept.params().num_remote_portals);
  }
  return true;
}

bool NodeLink::OnAcceptParcel(msg::AcceptParcel& accept) {
  const absl::Span<uint8_t> parcel_data =
      accept.GetArrayView<uint8_t>(accept.params().parcel_data);
  const absl::Span<RouterDescriptor> new_routers =
      accept.GetArrayView<RouterDescriptor>(accept.params().new_routers);
  absl::Span<os::Handle> handles =
      accept.GetHandlesView(accept.params().os_handles);

  Parcel::PortalVector portals(new_routers.size());
  for (size_t i = 0; i < new_routers.size(); ++i) {
    portals[i] = mem::MakeRefCounted<Portal>(
        node_, Router::Deserialize(new_routers[i], *this));
  }

  std::vector<os::Handle> os_handles;
  os_handles.reserve(handles.size());
  for (os::Handle& handle : handles) {
    os_handles.push_back(std::move(handle));
  }

  Parcel parcel(accept.params().sequence_number);
  parcel.SetData(std::vector<uint8_t>(parcel_data.begin(), parcel_data.end()));
  parcel.SetPortals(std::move(portals));
  parcel.SetOSHandles(std::move(os_handles));
  absl::optional<Route> route = GetRoute(accept.params().routing_id);
  if (!route) {
    DVLOG(4) << "Dropping " << parcel.Describe() << " at "
             << local_node_name_.ToString() << ", arriving from "
             << remote_node_name_.ToString() << " via unknown routing ID "
             << accept.params().routing_id;
    return true;
  }

  const LinkType link_type = route->link->GetType();
  if (link_type == LinkType::kCentral ||
      link_type == LinkType::kPeripheralOutward) {
    DVLOG(4) << "Accepting inbound " << parcel.Describe() << " at "
             << route->link->Describe();
    return route->receiver->AcceptInboundParcel(parcel);
  } else {
    ABSL_ASSERT(link_type == LinkType::kPeripheralInward);
    DVLOG(4) << "Accepting outbound " << parcel.Describe() << " at "
             << route->link->Describe();
    return route->receiver->AcceptOutboundParcel(parcel);
  }
}

bool NodeLink::OnRouteClosed(const msg::RouteClosed& route_closed) {
  absl::optional<Route> route = GetRoute(route_closed.params().routing_id);
  if (!route) {
    return true;
  }

  route->receiver->AcceptRouteClosureFrom(
      route->link->GetType().direction(),
      route_closed.params().sequence_length);
  return true;
}

bool NodeLink::OnSetRouterLinkStateAddress(
    const msg::SetRouterLinkStateAddress& set) {
  absl::optional<Route> route = GetRoute(set.params().routing_id);
  if (!route) {
    return true;
  }

  route->link->SetLinkStateAddress(set.params().address);
  return true;
}

bool NodeLink::OnIntroduceNode(msg::IntroduceNode& intro) {
  const NodeName name = intro.params().name;
  const bool known = intro.params().known;
  if (!known) {
    return true;
  }

  absl::Span<uint8_t> transport_data =
      intro.GetArrayView<uint8_t>(intro.params().transport_data);
  absl::Span<os::Handle> transport_os_handles =
      intro.GetHandlesView(intro.params().transport_os_handles);
  absl::Span<os::Handle> buffer_handles =
      intro.GetHandlesView(intro.params().buffer_handle);
  ABSL_ASSERT(!buffer_handles.empty());
  os::Handle buffer_handle = std::move(buffer_handles[0]);
  return node_->OnIntroduceNode(
      name, known, NodeLinkMemory::Adopt(node_, std::move(buffer_handle)),
      transport_data, transport_os_handles);
}

bool NodeLink::OnAddLinkBuffer(msg::AddLinkBuffer& add) {
  memory().AddBuffer(
      add.params().buffer_id,
      os::Memory(std::move(add.GetHandle(add.params().buffer_handle)),
                 add.params().buffer_size));
  return true;
}

bool NodeLink::OnStopProxying(const msg::StopProxying& stop) {
  mem::Ref<Router> router = GetRouter(stop.params().routing_id);
  if (!router) {
    DVLOG(4) << "Received StopProxying for unknown route";
    return true;
  }

  DVLOG(4) << "Received StopProxying on " << local_node_name_.ToString()
           << " routing ID " << stop.params().routing_id << " with inbound"
           << " length " << stop.params().proxy_inbound_sequence_length
           << " and outbound length "
           << stop.params().proxy_outbound_sequence_length;

  return router->StopProxying(stop.params().proxy_inbound_sequence_length,
                              stop.params().proxy_outbound_sequence_length);
}

bool NodeLink::OnInitiateProxyBypass(const msg::InitiateProxyBypass& request) {
  mem::Ref<Router> router = GetRouter(request.params().routing_id);
  if (!router) {
    return true;
  }

  return router->InitiateProxyBypass(*this, request.params().routing_id,
                                     request.params().proxy_peer_name,
                                     request.params().proxy_peer_routing_id);
}

bool NodeLink::OnBypassProxyToSameNode(
    const msg::BypassProxyToSameNode& bypass) {
  mem::Ref<Router> router = GetRouter(bypass.params().routing_id);
  if (!router) {
    return true;
  }

  mem::Ref<RouterLink> new_link = AddRoute(
      bypass.params().new_routing_id, bypass.params().new_link_state_address,
      LinkType::kCentral, LinkSide::kB, router);
  return router->BypassProxyWithNewLinkToSameNode(
      std::move(new_link), bypass.params().proxy_inbound_sequence_length);
}

bool NodeLink::OnStopProxyingToLocalPeer(
    const msg::StopProxyingToLocalPeer& stop) {
  mem::Ref<Router> router = GetRouter(stop.params().routing_id);
  if (!router) {
    return true;
  }
  return router->StopProxyingToLocalPeer(
      stop.params().proxy_outbound_sequence_length);
}

bool NodeLink::OnProxyWillStop(const msg::ProxyWillStop& will_stop) {
  mem::Ref<Router> router = GetRouter(will_stop.params().routing_id);
  if (!router) {
    return true;
  }

  return router->OnProxyWillStop(
      will_stop.params().proxy_inbound_sequence_length);
}

bool NodeLink::OnNotifyBypassPossible(const msg::NotifyBypassPossible& notify) {
  mem::Ref<Router> router = GetRouter(notify.params().routing_id);
  if (!router) {
    return true;
  }

  return router->OnBypassPossible();
}

bool NodeLink::OnRequestMemory(const msg::RequestMemory& request) {
  os::Memory memory(request.params().size);
  msg::ProvideMemory provide;
  provide.params().size = request.params().size;
  provide.params().handle = provide.AppendHandle(memory.TakeHandle());
  Transmit(provide);
  return true;
}

bool NodeLink::OnProvideMemory(msg::ProvideMemory& provide) {
  os::Memory memory(std::move(provide.GetHandle(provide.params().handle)),
                    provide.params().size);
  RequestMemoryCallback callback;
  {
    absl::MutexLock lock(&mutex_);
    auto it = pending_memory_requests_.find(provide.params().size);
    if (it == pending_memory_requests_.end()) {
      return false;
    }

    std::list<RequestMemoryCallback>& callbacks = it->second;
    ABSL_ASSERT(!callbacks.empty());
    callback = std::move(callbacks.front());
    callbacks.pop_front();
    if (callbacks.empty()) {
      pending_memory_requests_.erase(it);
    }
  }

  LOG(ERROR) << "YES!";

  callback(std::move(memory));
  return true;
}

bool NodeLink::OnLogRouteTrace(const msg::LogRouteTrace& log_request) {
  absl::optional<Route> route = GetRoute(log_request.params().routing_id);
  if (!route) {
    return true;
  }

  route->receiver->AcceptLogRouteTraceFrom(route->link->GetType().direction());
  return true;
}

NodeLink::Route::Route(mem::Ref<RemoteRouterLink> link,
                       mem::Ref<Router> receiver)
    : link(std::move(link)), receiver(std::move(receiver)) {}

NodeLink::Route::Route(Route&&) = default;

NodeLink::Route::Route(const Route&) = default;

NodeLink::Route& NodeLink::Route::operator=(Route&&) = default;

NodeLink::Route& NodeLink::Route::operator=(const Route&) = default;

NodeLink::Route::~Route() = default;

}  // namespace core
}  // namespace ipcz
