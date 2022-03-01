// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/node_link.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include "build/build_config.h"
#include "ipcz/box.h"
#include "ipcz/fragment.h"
#include "ipcz/fragment_descriptor.h"
#include "ipcz/handle_descriptor.h"
#include "ipcz/ipcz.h"
#include "ipcz/message_internal.h"
#include "ipcz/node.h"
#include "ipcz/node_messages.h"
#include "ipcz/portal.h"
#include "ipcz/remote_router_link.h"
#include "ipcz/router.h"
#include "ipcz/router_descriptor.h"
#include "ipcz/router_link.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "util/handle_util.h"
#include "util/log.h"
#include "util/ref_counted.h"

namespace ipcz {

// static
Ref<NodeLink> NodeLink::Create(Ref<Node> node,
                               LinkSide link_side,
                               const NodeName& local_node_name,
                               const NodeName& remote_node_name,
                               Node::Type remote_node_type,
                               uint32_t remote_protocol_version,
                               Ref<DriverTransport> transport,
                               OSProcess remote_process,
                               Ref<NodeLinkMemory> memory) {
  auto link = WrapRefCounted(new NodeLink(
      std::move(node), link_side, local_node_name, remote_node_name,
      remote_node_type, remote_protocol_version, std::move(transport),
      std::move(remote_process), std::move(memory)));
  link->memory().SetNodeLink(link);
  return link;
}

NodeLink::NodeLink(Ref<Node> node,
                   LinkSide link_side,
                   const NodeName& local_node_name,
                   const NodeName& remote_node_name,
                   Node::Type remote_node_type,
                   uint32_t remote_protocol_version,
                   Ref<DriverTransport> transport,
                   OSProcess remote_process,
                   Ref<NodeLinkMemory> memory)
    : node_(std::move(node)),
      link_side_(link_side),
      local_node_name_(local_node_name),
      remote_node_name_(remote_node_name),
      remote_node_type_(remote_node_type),
      remote_protocol_version_(remote_protocol_version),
      transport_(std::move(transport)),
      remote_process_(std::move(remote_process)),
      memory_(std::move(memory)) {
  transport_->set_listener(this);
}

NodeLink::~NodeLink() {
  Deactivate();
}

Ref<RemoteRouterLink> NodeLink::AddRemoteRouterLink(
    SublinkId sublink,
    FragmentRef<RouterLinkState> link_state,
    LinkType type,
    LinkSide side,
    Ref<Router> router) {
  auto link = RemoteRouterLink::Create(WrapRefCounted(this), sublink,
                                       std::move(link_state), type, side);

  absl::MutexLock lock(&mutex_);
  auto [it, added] = sublinks_.try_emplace(
      sublink, Sublink(std::move(link), std::move(router)));
  if (!added) {
    return nullptr;
  }
  return it->second.router_link;
}

bool NodeLink::RemoveRemoteRouterLink(SublinkId sublink) {
  absl::MutexLock lock(&mutex_);
  auto it = sublinks_.find(sublink);
  if (it == sublinks_.end()) {
    return false;
  }

  sublinks_.erase(sublink);
  return true;
}

absl::optional<NodeLink::Sublink> NodeLink::GetSublink(SublinkId sublink) {
  absl::MutexLock lock(&mutex_);
  auto it = sublinks_.find(sublink);
  if (it == sublinks_.end()) {
    return absl::nullopt;
  }
  return it->second;
}

Ref<Router> NodeLink::GetRouter(SublinkId sublink) {
  absl::MutexLock lock(&mutex_);
  auto it = sublinks_.find(sublink);
  if (it == sublinks_.end()) {
    return nullptr;
  }
  return it->second.receiver;
}

void NodeLink::Deactivate() {
  SublinkMap sublinks;
  {
    absl::MutexLock lock(&mutex_);
    sublinks = std::move(sublinks_);
    if (!active_) {
      return;
    }

    active_ = false;
  }

  memory_->SetNodeLink(nullptr);
  sublinks.clear();
  transport_->Deactivate();
}

void NodeLink::RequestIndirectBrokerConnection(
    Ref<DriverTransport> transport,
    OSProcess new_node_process,
    size_t num_initial_portals,
    IndirectBrokerConnectionCallback callback) {
  std::vector<uint8_t> transport_data;
  std::vector<OSHandle> transport_handles;
  if (transport) {
    IpczResult result = transport->Serialize(transport_data, transport_handles);
    ABSL_ASSERT(result == IPCZ_RESULT_OK);
  }

  uint64_t request_id;
  {
    absl::MutexLock lock(&mutex_);
    request_id = next_request_id_++;
    pending_indirect_broker_connections_[request_id] = std::move(callback);
  }

  msg::RequestIndirectBrokerConnection request;
  request.params().request_id = request_id;
  request.params().num_initial_portals =
      static_cast<uint32_t>(num_initial_portals);

  request.params().transport_data =
      request.AllocateArray<uint8_t>(transport_data.size());
  memcpy(request.GetArrayData(request.params().transport_data),
         transport_data.data(), transport_data.size());

  request.params().transport_os_handles =
      request.AppendHandles(absl::MakeSpan(transport_handles));

  OSHandle process_handle = new_node_process.TakeAsHandle();
  std::vector<OSHandle> process_handles;
  if (process_handle.is_valid()) {
    process_handles.push_back(std::move(process_handle));
  }
  request.params().process_handle =
      request.AppendHandles(absl::MakeSpan(process_handles));

  Transmit(request);
}

void NodeLink::RequestIntroduction(const NodeName& name) {
  msg::RequestIntroduction request;
  request.params().name = name;
  Transmit(request);
}

void NodeLink::IntroduceNode(const NodeName& name,
                             LinkSide link_side,
                             Ref<DriverTransport> transport,
                             DriverMemory link_buffer_memory) {
  std::vector<uint8_t> transport_data;
  std::vector<OSHandle> transport_handles;
  if (transport) {
    IpczResult result = transport->Serialize(transport_data, transport_handles);
    ABSL_ASSERT(result == IPCZ_RESULT_OK);
  }

  msg::IntroduceNode intro;
  intro.params().name = name;
  intro.params().known = (transport != nullptr);
  intro.params().link_side = link_side;
  intro.params().transport_data =
      intro.AllocateArray<uint8_t>(transport_data.size());
  memcpy(intro.GetArrayData(intro.params().transport_data),
         transport_data.data(), transport_data.size());
  intro.params().transport_os_handles =
      intro.AppendHandles(absl::MakeSpan(transport_handles));

  auto buffer = intro.AppendSharedMemory(std::move(link_buffer_memory));
  intro.params().set_buffer(buffer);

  Transmit(intro);
}

bool NodeLink::BypassProxy(const NodeName& proxy_name,
                           SublinkId proxy_sublink,
                           SequenceNumber proxy_outbound_sequence_length,
                           Ref<Router> new_peer) {
  // Note that by convention the side which initiates a bypass (this side)
  // adopts side A of the new bypass link. The other end adopts side B.
  const SublinkId new_sublink = memory().AllocateSublinkIds(1);
  FragmentRef<RouterLinkState> state = memory().AllocateRouterLinkState();
  Ref<RouterLink> new_link = AddRemoteRouterLink(
      new_sublink, state, LinkType::kCentral, LinkSide::kA, new_peer);

  DVLOG(4) << "Sending BypassProxy from " << local_node_name_.ToString()
           << " to " << remote_node_name_.ToString() << " with new sublink "
           << new_sublink << " to replace its link to proxy "
           << proxy_name.ToString() << " on sublink " << proxy_sublink;

  msg::BypassProxy bypass;
  bypass.params().proxy_name = proxy_name;
  bypass.params().proxy_sublink = proxy_sublink;
  bypass.params().new_sublink = new_sublink;
  bypass.params().new_link_state_fragment = state.release().descriptor();
  bypass.params().proxy_outbound_sequence_length =
      proxy_outbound_sequence_length;
  Transmit(bypass);

  // This link is only provided after we transmit the bypass request, ensuring
  // that `new_peer` doesn't send anything else over the link until the bypass
  // has been accepted by the remote node.
  new_peer->SetOutwardLink(new_link);

  return true;
}

void NodeLink::AddFragmentAllocatorBuffer(BufferId buffer_id,
                                          uint32_t fragment_size,
                                          DriverMemory memory) {
  msg::AddFragmentAllocatorBuffer add;
  add.params().buffer_id = buffer_id;
  add.params().fragment_size = fragment_size;

  auto buffer = add.AppendSharedMemory(std::move(memory));
  add.params().set_buffer(buffer);

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

bool NodeLink::DispatchRelayedMessage(msg::AcceptRelayedMessage& relay) {
  absl::Span<uint8_t> data = relay.GetArrayView<uint8_t>(relay.params().data);
  absl::Span<OSHandle> handles = relay.GetHandlesView(relay.params().handles);
  internal::MessageHeaderV0& header = internal::GetMessageHeader(data);
  if (!internal::IsMessageHeaderValid(data)) {
    return false;
  }

  absl::Span<const internal::ParamMetadata> metadata;
  switch (header.message_id) {
    case msg::AcceptParcel::kId:
      metadata = absl::MakeSpan(msg::AcceptParcel::kMetadata);
      break;

    case msg::AddFragmentAllocatorBuffer::kId:
      metadata = absl::MakeSpan(msg::AcceptParcel::kMetadata);
      break;

    default:
      DVLOG(2) << "Ignoring unexpected broker message relay for ID "
               << static_cast<int>(header.message_id);
      return true;
  }

  internal::SerializeMessageHandleData(data, handles, metadata, OSProcess());
#if BUILDFLAG(IS_WIN)
  for (OSHandle& handle : handles) {
    handle.release();
  }
  handles = {};
#endif

  IpczResult result =
      DispatchMessage(DriverTransport::Message({data}, handles));
  return result == IPCZ_RESULT_OK;
}

void NodeLink::TransmitMessage(
    internal::MessageBase& message,
    absl::Span<const internal::ParamMetadata> metadata) {
#if BUILDFLAG(IS_WIN)
  // OS handles in messages may only be transmitted if we have a handle to the
  // remote process (implying that we are relatively more privileged), or if
  // the remote process is a broker or otherwise known to be more privileged
  // (and may therefore duplicate our own handles itself). In other cases where
  // relative privilege is equal or ambigious, we must relay OS handles
  // through the broker.
  if (!remote_process_.is_valid() && remote_node_type_ != Node::Type::kBroker &&
      !message.handles_view().empty()) {
    auto broker = node_->GetBrokerLink();
    if (!broker) {
      return;
    }

    broker->RelayMessage(remote_node_name_, message);
    return;
  }
#endif

  message.Serialize(metadata, remote_process_);

  size_t small_size_class = 0;
  if (message.data_view().size() <= 64) {
    small_size_class = 64;
  } else if (message.data_view().size() <= 256) {
    small_size_class = 256;
  }

  if (small_size_class && message.handles_view().empty()) {
    // For messages which are small with no handles, prefer transmission through
    // shared memory.
    Fragment fragment = memory().AllocateFragment(small_size_class);
    if (!fragment.is_null()) {
      message.header().transport_sequence_number =
          transport_sequence_number_.load(std::memory_order_relaxed);
      memcpy(fragment.address(), message.data_view().data(),
             message.data_view().size());
      if (memory().outgoing_message_fragments().Push(fragment.descriptor())) {
        if (!memory().TestAndSetNotificationPending()) {
          msg::FlushLink flush;
          flush.header().transport_sequence_number =
              transport_sequence_number_.fetch_add(1,
                                                   std::memory_order_relaxed);
          transport_->TransmitMessage(DriverTransport::Message(
              DriverTransport::Data(flush.data_view())));
        }
        return;
      } else {
        memory().FreeFragment(fragment);
      }
    }
  }

  message.header().transport_sequence_number =
      transport_sequence_number_.fetch_add(1, std::memory_order_relaxed);
  transport_->TransmitMessage(DriverTransport::Message(
      DriverTransport::Data(message.data_view()), message.handles_view()));
}

void NodeLink::RelayMessage(const NodeName& to_node,
                            internal::MessageBase& message) {
  ABSL_ASSERT(remote_node_type_ == Node::Type::kBroker);
  msg::RelayMessage relay;
  relay.params().destination = to_node;
  relay.params().data =
      relay.AllocateArray<uint8_t>(message.data_view().size());

  relay.params().handles = relay.AppendHandles(message.handles_view());
  memcpy(relay.GetArrayData(relay.params().data), message.data_view().data(),
         message.data_view().size());

  Transmit(relay);
}

IpczResult NodeLink::OnTransportMessage(
    const DriverTransport::Message& message) {
  const auto& header =
      *reinterpret_cast<const internal::MessageHeader*>(message.data.data());
  const uint64_t sequence_number = header.transport_sequence_number;

  IpczResult result = FlushSharedMemoryMessages(sequence_number);
  if (result != IPCZ_RESULT_OK) {
    return result;
  }

  result = DispatchMessage(message);
  if (result != IPCZ_RESULT_OK) {
    return result;
  }

  memory().ClearPendingNotification();
  return FlushSharedMemoryMessages(sequence_number + 1);
}

void NodeLink::OnTransportError() {
  SublinkMap sublinks;
  {
    absl::MutexLock lock(&mutex_);
    broken_ = true;
    std::swap(sublinks, sublinks_);
  }

  for (auto& [id, sublink] : sublinks) {
    sublink.receiver->NotifyLinkDisconnected(*this, id);
  }
}

bool NodeLink::OnConnectFromBrokerToNonBroker(
    msg::ConnectFromBrokerToNonBroker& connect) {
  // Only accepted early in a transport's lifetime, before any NodeLink is
  // listening. See NodeConnector.
  return false;
}

bool NodeLink::OnConnectFromNonBrokerToBroker(
    msg::ConnectFromNonBrokerToBroker& connect) {
  // Only accepted early in a transport's lifetime, before any NodeLink is
  // listening. See NodeConnector.
  return false;
}

bool NodeLink::OnConnectToBrokerIndirect(
    msg::ConnectToBrokerIndirect& connect) {
  // Only accepted early in a transport's lifetime, before any NodeLink is
  // listening. See NodeConnector.
  return false;
}

bool NodeLink::OnConnectFromBrokerIndirect(
    msg::ConnectFromBrokerIndirect& connect) {
  // Only accepted early in a transport's lifetime, before any NodeLink is
  // listening. See NodeConnector.
  return false;
}

bool NodeLink::OnConnectFromBrokerToBroker(
    msg::ConnectFromBrokerToBroker& connect) {
  // Only accepted early in a transport's lifetime, before any NodeLink is
  // listening. See NodeConnector.
  return false;
}

bool NodeLink::OnRequestIndirectBrokerConnection(
    msg::RequestIndirectBrokerConnection& request) {
  if (node_->type() != Node::Type::kBroker) {
    return false;
  }

  absl::Span<uint8_t> transport_data =
      request.GetArrayView<uint8_t>(request.params().transport_data);
  absl::Span<OSHandle> transport_os_handles =
      request.GetHandlesView(request.params().transport_os_handles);
  absl::Span<OSHandle> process_handles =
      request.GetHandlesView(request.params().process_handle);

  if (process_handles.size() > 1) {
    return false;
  }

  OSProcess new_node_process;
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_FUCHSIA)
  if (process_handles.size() == 1) {
    new_node_process = OSProcess(process_handles[0].handle());
    process_handles[0].release();
  }
#endif

  auto transport =
      DriverTransport::Deserialize(node_, transport_data, transport_os_handles);
  if (!transport) {
    return false;
  }

  return node_->OnRequestIndirectBrokerConnection(
      *this, request.params().request_id, std::move(transport),
      std::move(new_node_process), request.params().num_initial_portals);
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
  absl::Span<const uint8_t> parcel_data =
      accept.GetArrayView<uint8_t>(accept.params().parcel_data);
  absl::Span<const HandleDescriptor> handle_descriptors =
      accept.GetArrayView<HandleDescriptor>(accept.params().handle_descriptors);
  absl::Span<const RouterDescriptor> new_routers =
      accept.GetArrayView<RouterDescriptor>(accept.params().new_routers);
  absl::Span<const uint8_t> handle_data =
      accept.GetArrayView<uint8_t>(accept.params().handle_data);
  absl::Span<OSHandle> os_handles =
      accept.GetHandlesView(accept.params().os_handles);

  const size_t num_parcel_os_handles = accept.params().num_parcel_os_handles;
  if (num_parcel_os_handles > os_handles.size()) {
    return false;
  }

  std::vector<OSHandle> parcel_os_handles(num_parcel_os_handles);
  for (size_t i = 0; i < num_parcel_os_handles; ++i) {
    parcel_os_handles[i] = std::move(os_handles[i]);
  }
  os_handles.remove_prefix(num_parcel_os_handles);

  Parcel::ObjectVector objects(handle_descriptors.size());
  for (size_t i = 0; i < handle_descriptors.size(); ++i) {
    const HandleDescriptor& descriptor = handle_descriptors[i];
    if (descriptor.num_bytes > handle_data.size() ||
        descriptor.num_os_handles > os_handles.size()) {
      return false;
    }

    switch (descriptor.type) {
      case HandleDescriptor::kPortal: {
        if (new_routers.empty()) {
          return false;
        }

        Ref<Router> new_router = Router::Deserialize(new_routers[0], *this);
        if (!new_router) {
          return false;
        }

        objects[i] = MakeRefCounted<Portal>(node_, std::move(new_router));
        new_routers.remove_prefix(1);
        break;
      }

      case HandleDescriptor::kBox: {
        absl::InlinedVector<IpczOSHandle, 4> ipcz_os_handles(
            descriptor.num_os_handles);
        for (size_t j = 0; j < descriptor.num_os_handles; ++j) {
          ipcz_os_handles[j].size = sizeof(ipcz_os_handles[j]);
          if (!OSHandle::ToIpczOSHandle(std::move(os_handles[j]),
                                        &ipcz_os_handles[j])) {
            return false;
          }
        }

        IpczDriverHandle driver_handle;
        IpczResult result = node_->driver().Deserialize(
            node_->driver_node(), handle_data.data(), descriptor.num_bytes,
            ipcz_os_handles.data(), descriptor.num_os_handles, IPCZ_NO_FLAGS,
            nullptr, &driver_handle);
        if (result != IPCZ_RESULT_OK) {
          return false;
        }

        objects[i] = MakeRefCounted<Box>(DriverObject(node_, driver_handle));
        break;
      }

      default:
        return false;
    }

    handle_data.remove_prefix(descriptor.num_bytes);
    os_handles.remove_prefix(descriptor.num_os_handles);
  }

  Parcel parcel(accept.params().sequence_number);
  parcel.SetData(std::vector<uint8_t>(parcel_data.begin(), parcel_data.end()));
  parcel.SetObjects(std::move(objects));
  parcel.SetOSHandles(std::move(parcel_os_handles));
  absl::optional<Sublink> sublink = GetSublink(accept.params().sublink);
  if (!sublink) {
    DVLOG(4) << "Dropping " << parcel.Describe() << " at "
             << local_node_name_.ToString() << ", arriving from "
             << remote_node_name_.ToString() << " via unknown sublink "
             << accept.params().sublink;
    return true;
  }

  const LinkType link_type = sublink->router_link->GetType();
  if (link_type == LinkType::kCentral ||
      link_type == LinkType::kPeripheralOutward) {
    DVLOG(4) << "Accepting inbound " << parcel.Describe() << " at "
             << sublink->router_link->Describe();
    return sublink->receiver->AcceptInboundParcel(parcel);
  } else {
    ABSL_ASSERT(link_type == LinkType::kPeripheralInward);
    DVLOG(4) << "Accepting outbound " << parcel.Describe() << " at "
             << sublink->router_link->Describe();
    return sublink->receiver->AcceptOutboundParcel(parcel);
  }
}

bool NodeLink::OnRouteClosed(const msg::RouteClosed& route_closed) {
  absl::optional<Sublink> sublink = GetSublink(route_closed.params().sublink);
  if (!sublink) {
    return true;
  }

  return sublink->receiver->AcceptRouteClosureFrom(
      sublink->router_link->GetType(), route_closed.params().sequence_length);
}

bool NodeLink::OnSetRouterLinkStateFragment(
    const msg::SetRouterLinkStateFragment& set) {
  absl::optional<Sublink> sublink = GetSublink(set.params().sublink);
  if (!sublink) {
    return true;
  }

  sublink->router_link->SetLinkState(
      memory().AdoptFragmentRef<RouterLinkState>(set.params().descriptor));
  return true;
}

bool NodeLink::OnRouteDisconnected(const msg::RouteDisconnected& disconnect) {
  const SublinkId sublink = disconnect.params().sublink;
  Ref<Router> router = GetRouter(sublink);
  if (!router) {
    return true;
  }

  router->NotifyLinkDisconnected(*this, sublink);
  return true;
}

bool NodeLink::OnRequestIntroduction(const msg::RequestIntroduction& request) {
  return node_->OnRequestIntroduction(*this, request);
}

bool NodeLink::OnIntroduceNode(msg::IntroduceNode& intro) {
  if (remote_node_type_ != Node::Type::kBroker) {
    return false;
  }

  const NodeName name = intro.params().name;
  const bool known = intro.params().known;
  if (!known) {
    return true;
  }

  absl::Span<uint8_t> transport_data =
      intro.GetArrayView<uint8_t>(intro.params().transport_data);
  absl::Span<OSHandle> transport_os_handles =
      intro.GetHandlesView(intro.params().transport_os_handles);

  DriverMemory memory = intro.TakeSharedMemory(node_, intro.params().buffer());
  if (!memory.is_valid()) {
    return false;
  }
  return node_->OnIntroduceNode(name, known, intro.params().link_side,
                                NodeLinkMemory::Adopt(node_, std::move(memory)),
                                transport_data, transport_os_handles);
}

bool NodeLink::OnAddFragmentAllocatorBuffer(
    msg::AddFragmentAllocatorBuffer& add) {
  DriverMemory buffer_memory =
      add.TakeSharedMemory(node_, add.params().buffer());
  if (!buffer_memory.is_valid()) {
    return false;
  }

  return memory().AddFragmentAllocatorBuffer(add.params().buffer_id,
                                             add.params().fragment_size,
                                             std::move(buffer_memory));
}

bool NodeLink::OnStopProxying(const msg::StopProxying& stop) {
  Ref<Router> router = GetRouter(stop.params().sublink);
  if (!router) {
    DVLOG(4) << "Received StopProxying for unknown router";
    return true;
  }

  DVLOG(4) << "Received StopProxying on " << local_node_name_.ToString()
           << " sublink " << stop.params().sublink << " with inbound"
           << " length " << stop.params().proxy_inbound_sequence_length
           << " and outbound length "
           << stop.params().proxy_outbound_sequence_length;

  return router->StopProxying(stop.params().proxy_inbound_sequence_length,
                              stop.params().proxy_outbound_sequence_length);
}

bool NodeLink::OnInitiateProxyBypass(const msg::InitiateProxyBypass& request) {
  Ref<Router> router = GetRouter(request.params().sublink);
  if (!router) {
    return true;
  }

  return router->InitiateProxyBypass(*this, request.params().sublink,
                                     request.params().proxy_peer_name,
                                     request.params().proxy_peer_sublink);
}

bool NodeLink::OnBypassProxy(const msg::BypassProxy& bypass) {
  return node_->OnBypassProxy(*this, bypass);
}

bool NodeLink::OnBypassProxyToSameNode(
    const msg::BypassProxyToSameNode& bypass) {
  Ref<Router> router = GetRouter(bypass.params().sublink);
  if (!router) {
    return true;
  }

  Ref<RouterLink> new_link =
      AddRemoteRouterLink(bypass.params().new_sublink,
                          memory().AdoptFragmentRef<RouterLinkState>(
                              bypass.params().new_link_state_fragment),
                          LinkType::kCentral, LinkSide::kB, router);
  return router->BypassProxyWithNewLinkToSameNode(
      std::move(new_link), bypass.params().proxy_inbound_sequence_length);
}

bool NodeLink::OnStopProxyingToLocalPeer(
    const msg::StopProxyingToLocalPeer& stop) {
  Ref<Router> router = GetRouter(stop.params().sublink);
  if (!router) {
    return true;
  }
  return router->StopProxyingToLocalPeer(
      stop.params().proxy_outbound_sequence_length);
}

bool NodeLink::OnProxyWillStop(const msg::ProxyWillStop& will_stop) {
  Ref<Router> router = GetRouter(will_stop.params().sublink);
  if (!router) {
    return true;
  }

  return router->OnProxyWillStop(
      will_stop.params().proxy_inbound_sequence_length);
}

bool NodeLink::OnFlushRouter(const msg::FlushRouter& flush) {
  if (Ref<Router> router = GetRouter(flush.params().sublink)) {
    router->Flush(/*force_bypass_attempt=*/true);
  }
  return true;
}

bool NodeLink::OnRequestMemory(const msg::RequestMemory& request) {
  DriverMemory memory(node_, request.params().size);
  msg::ProvideMemory provide;
  provide.params().size = request.params().size;

  auto buffer = provide.AppendSharedMemory(std::move(memory));
  provide.params().set_buffer(buffer);

  Transmit(provide);
  return true;
}

bool NodeLink::OnProvideMemory(msg::ProvideMemory& provide) {
  DriverMemory memory =
      provide.TakeSharedMemory(node_, provide.params().buffer());
  if (!memory.is_valid()) {
    return false;
  }

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

  callback(std::move(memory));
  return true;
}

bool NodeLink::OnLogRouteTrace(const msg::LogRouteTrace& log_request) {
  absl::optional<Sublink> sublink = GetSublink(log_request.params().sublink);
  if (!sublink) {
    return true;
  }

  sublink->receiver->AcceptLogRouteTraceFrom(sublink->router_link->GetType());
  return true;
}

bool NodeLink::OnRelayMessage(msg::RelayMessage& relay) {
  if (node_->type() != Node::Type::kBroker) {
    return false;
  }

  return node_->RelayMessage(remote_node_name_, relay);
}

bool NodeLink::OnAcceptRelayedMessage(msg::AcceptRelayedMessage& relay) {
  if (remote_node_type_ != Node::Type::kBroker) {
    return false;
  }

  return node_->AcceptRelayedMessage(relay);
}

bool NodeLink::OnFlushLink(const msg::FlushLink& flush) {
  // No-op: this message is only sent to elicit a transport notification, which
  // it's already done.
  return true;
}

IpczResult NodeLink::FlushSharedMemoryMessages(uint64_t max_sequence_number) {
  MpscQueue<FragmentDescriptor>& fragments =
      memory().incoming_message_fragments();
  for (;;) {
    FragmentDescriptor* descriptor = fragments.Peek();
    if (!descriptor) {
      // No messages in shared memory.
      return IPCZ_RESULT_OK;
    }

    Fragment fragment = memory().GetFragment(*descriptor);
    if (fragment.is_pending()) {
      // The message queue is non-empty, but the head of the queue is a fragment
      // whose buffer is not yet mapped by this node. In such cases there must
      // already be a transport message in flight to register the buffer with
      // this node, so the queue can be unblocked once that arrives.
      return IPCZ_RESULT_OK;
    }

    if (fragment.size() < sizeof(internal::MessageHeader)) {
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    auto& header = *static_cast<internal::MessageHeader*>(fragment.address());
    if (header.transport_sequence_number > max_sequence_number) {
      // This is fine, but we're not ready to dispatch this message yet.
      return IPCZ_RESULT_OK;
    }

    IpczResult result =
        DispatchMessage(DriverTransport::Message(fragment.bytes()));
    if (result != IPCZ_RESULT_OK) {
      return result;
    }

    fragments.Pop();
    memory().FreeFragment(fragment);
  }
}

IpczResult NodeLink::DispatchMessage(const DriverTransport::Message& message) {
  const auto& header =
      *reinterpret_cast<const internal::MessageHeader*>(message.data.data());

  switch (header.message_id) {
// clang-format off
#include "ipcz/message_macros/message_dispatch_macros.h"
#include "ipcz/node_message_defs.h"
#include "ipcz/message_macros/undef_message_macros.h"
      // clang-format on

    default:
      DLOG(WARNING) << "Ignoring unknown transport message with ID "
                    << static_cast<int>(header.message_id);
      break;
  }

  return IPCZ_RESULT_OK;
}

NodeLink::Sublink::Sublink(Ref<RemoteRouterLink> router_link,
                           Ref<Router> receiver)
    : router_link(std::move(router_link)), receiver(std::move(receiver)) {}

NodeLink::Sublink::Sublink(Sublink&&) = default;

NodeLink::Sublink::Sublink(const Sublink&) = default;

NodeLink::Sublink& NodeLink::Sublink::operator=(Sublink&&) = default;

NodeLink::Sublink& NodeLink::Sublink::operator=(const Sublink&) = default;

NodeLink::Sublink::~Sublink() = default;

}  // namespace ipcz
