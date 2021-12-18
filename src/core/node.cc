// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/node.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "core/driver_transport.h"
#include "core/link_side.h"
#include "core/link_type.h"
#include "core/message_internal.h"
#include "core/node_link.h"
#include "core/node_link_buffer.h"
#include "core/node_messages.h"
#include "core/portal.h"
#include "core/router.h"
#include "core/routing_id.h"
#include "debug/log.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/memory.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/handle_util.h"

namespace ipcz {
namespace core {

namespace {

// Helper which listens on the application-provided DriverTransport given to a
// ConnectNode() API call. This listens for an initial handshake to retrieve the
// remote node's name as well as the local node's assigned name
class ConnectListener : public DriverTransport::Listener,
                        public mem::RefCounted {
 public:
  ConnectListener(mem::Ref<Node> node,
                  NodeName local_node_name,
                  NodeName remote_node_name,
                  mem::Ref<DriverTransport> transport,
                  std::vector<mem::Ref<Portal>> waiting_portals,
                  os::Memory::Mapping link_buffer_mapping)
      : node_(std::move(node)),
        local_node_name_(local_node_name),
        remote_node_name_(remote_node_name),
        transport_(std::move(transport)),
        waiting_portals_(std::move(waiting_portals)),
        link_buffer_mapping_(std::move(link_buffer_mapping)) {
    if (node_->type() == Node::Type::kBroker) {
      ABSL_ASSERT(local_node_name.is_valid());
      ABSL_ASSERT(remote_node_name.is_valid());
      ABSL_ASSERT(link_buffer_mapping_.is_valid());
    } else {
      ABSL_ASSERT(!local_node_name.is_valid());
      ABSL_ASSERT(!remote_node_name.is_valid());
      ABSL_ASSERT(!link_buffer_mapping_.is_valid());
    }
  }

  using ConnectHandler = std::function<void(mem::Ref<NodeLink> link)>;

  void Listen(ConnectHandler handler) { handler_ = std::move(handler); }

  // DriverTransport::Listener:
  IpczResult OnTransportMessage(
      const DriverTransport::Message& message) override {
    mem::Ref<NodeLink> new_link;
    const auto& header =
        *reinterpret_cast<const internal::MessageHeader*>(message.data.data());
    if (node_->type() == Node::Type::kBroker &&
        header.message_id == msg::ConnectToBroker::kId) {
      msg::ConnectToBroker connect;
      if (!connect.Deserialize(message)) {
        handler_ = nullptr;
        return IPCZ_RESULT_INVALID_ARGUMENT;
      }
      return AcceptNewLink(mem::MakeRefCounted<NodeLink>(
          node_, local_node_name_, remote_node_name_, Node::Type::kNormal,
          connect.params.protocol_version, transport_,
          std::move(link_buffer_mapping_)));
    }

    if (node_->type() == Node::Type::kNormal &&
        header.message_id == msg::ConnectFromBroker::kId) {
      msg::ConnectFromBroker connect;
      if (!connect.Deserialize(message)) {
        handler_ = nullptr;
        return IPCZ_RESULT_INVALID_ARGUMENT;
      }
      os::Memory memory(std::move(connect.handles.link_state_memory),
                        sizeof(NodeLinkBuffer));
      return AcceptNewLink(mem::MakeRefCounted<NodeLink>(
          node_, connect.params.receiver_name, connect.params.broker_name,
          Node::Type::kBroker, connect.params.protocol_version, transport_,
          memory.Map()));
    }

    return IPCZ_RESULT_UNIMPLEMENTED;
  }

  IpczResult AcceptNewLink(mem::Ref<NodeLink> link) {
    if (!handler_) {
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    ConnectHandler handler = std::move(handler_);
    handler(link);

    // By convention, bootstrapped router links use side A on the node with the
    // the numerically lesser NodeName, and side B on the other node.
    const LinkSide link_side =
        link->local_node_name() < link->remote_node_name() ? LinkSide::kA
                                                           : LinkSide::kB;
    for (size_t i = 0; i < waiting_portals_.size(); ++i) {
      const mem::Ref<Router>& router = waiting_portals_[i]->router();
      router->SetOutwardLink(link->AddRoute(
          static_cast<RoutingId>(i), i, LinkType::kCentral, link_side, router));
    }
    return IPCZ_RESULT_OK;
  }

  void OnTransportError() override {
    if (handler_) {
      handler_(nullptr);
      handler_ = nullptr;
    }
  }

 private:
  ~ConnectListener() override = default;

  const mem::Ref<Node> node_;
  const NodeName local_node_name_;
  const NodeName remote_node_name_;
  const mem::Ref<DriverTransport> transport_;
  const std::vector<mem::Ref<Portal>> waiting_portals_;
  os::Memory::Mapping link_buffer_mapping_;
  ConnectHandler handler_;
};

}  // namespace

Node::Node(Type type, const IpczDriver& driver, IpczDriverHandle driver_node)
    : type_(type), driver_(driver), driver_node_(driver_node) {
  if (type_ == Type::kBroker) {
    // Only brokers assign their own names.
    assigned_name_ = NodeName{NodeName::kRandom};
    DVLOG(4) << "Created new broker node " << assigned_name_.ToString();
  } else {
    DVLOG(4) << "Created new non-broker node " << this;
  }
}

Node::~Node() = default;

void Node::ShutDown() {
  absl::flat_hash_map<NodeName, mem::Ref<NodeLink>> node_links;
  {
    absl::MutexLock lock(&mutex_);
    node_links = std::move(node_links_);
    node_links_.clear();
  }

  for (const auto& entry : node_links) {
    entry.second->Deactivate();
  }
}

IpczResult Node::ConnectNode(IpczDriverHandle driver_transport,
                             Type remote_node_type,
                             os::Process remote_process,
                             absl::Span<IpczHandle> initial_portals) {
  std::vector<mem::Ref<Portal>> buffering_portals(initial_portals.size());
  for (size_t i = 0; i < initial_portals.size(); ++i) {
    auto portal = mem::MakeRefCounted<Portal>(mem::WrapRefCounted(this),
                                              mem::MakeRefCounted<Router>());
    buffering_portals[i] = portal;
    initial_portals[i] = ToHandle(portal.release());
  }

  const uint32_t num_portals = static_cast<uint32_t>(initial_portals.size());
  NodeName local_node_name;
  NodeName remote_node_name;
  msg::ConnectFromBroker connect_from_broker;
  msg::ConnectToBroker connect_to_broker;
  os::Memory::Mapping link_state_mapping;
  if (type_ == Type::kBroker && remote_node_type == Type::kNormal) {
    os::Memory link_state_memory(sizeof(NodeLinkBuffer));
    absl::MutexLock lock(&mutex_);
    local_node_name = assigned_name_;
    remote_node_name = NodeName{NodeName::kRandom};
    connect_from_broker.params.broker_name = local_node_name;
    connect_from_broker.params.receiver_name = remote_node_name;
    connect_from_broker.params.protocol_version = msg::kProtocolVersion;
    connect_from_broker.params.num_initial_portals = num_portals;

    link_state_mapping = link_state_memory.Map();
    NodeLinkBuffer::Init(link_state_mapping.base(), num_portals);
    connect_from_broker.handles.link_state_memory =
        link_state_memory.TakeHandle();
  } else if (type_ == Type::kNormal && remote_node_type == Type::kBroker) {
    connect_to_broker.params.protocol_version = msg::kProtocolVersion;
    connect_to_broker.params.num_initial_portals = num_portals;
  } else {
    LOG(ERROR) << "ConnectNode() not implemented between two non-broker nodes.";
    return IPCZ_RESULT_UNIMPLEMENTED;
  }

  auto transport =
      mem::MakeRefCounted<DriverTransport>(driver_, driver_transport);
  auto listener = mem::MakeRefCounted<ConnectListener>(
      mem::WrapRefCounted(this), local_node_name, remote_node_name, transport,
      std::move(buffering_portals), std::move(link_state_mapping));
  listener->Listen([node = mem::WrapRefCounted(this), listener,
                    remote_node_type](mem::Ref<NodeLink> link) {
    node->AddLink(link->remote_node_name(), link);
    if (remote_node_type == Type::kBroker) {
      absl::MutexLock lock(&node->mutex_);
      DVLOG(4) << "Node " << node.get() << " assigned name "
               << link->local_node_name().ToString() << " by broker.";
      node->assigned_name_ = link->local_node_name();
      node->broker_link_ = std::move(link);
    }
  });

  transport->set_listener(listener.get());
  transport->Activate();

  if (type_ == Type::kBroker) {
    transport->Transmit(connect_from_broker);
  } else {
    transport->Transmit(connect_to_broker);
  }

  return IPCZ_RESULT_OK;
}

Portal::Pair Node::OpenPortals() {
  return Portal::CreatePair(mem::WrapRefCounted(this));
}

mem::Ref<NodeLink> Node::GetLink(const NodeName& name) {
  absl::MutexLock lock(&mutex_);
  auto it = node_links_.find(name);
  if (it == node_links_.end()) {
    return nullptr;
  }
  return it->second;
}

void Node::EstablishLink(const NodeName& name, EstablishLinkCallback callback) {
  mem::Ref<NodeLink> link;
  {
    absl::MutexLock lock(&mutex_);
    auto it = node_links_.find(name);
    if (it != node_links_.end()) {
      link = it->second;
    }
  }

  if (!link && type_ == Type::kBroker) {
    DLOG(ERROR) << "Broker cannot establish link to unknown node "
                << name.ToString();
    callback(nullptr);
    return;
  }

  if (link) {
    callback(link.get());
    return;
  }

  mem::Ref<NodeLink> broker;
  {
    absl::MutexLock lock(&mutex_);
    if (broker_link_) {
      auto result = pending_introductions_.try_emplace(
          name, std::vector<EstablishLinkCallback>());
      result.first->second.push_back(std::move(callback));
      if (!result.second) {
        // An introduction has already been requested for this name.
        return;
      }

      DVLOG(4) << "Node " << assigned_name_.ToString()
               << " requesting introduction to " << name.ToString();

      broker = broker_link_;
    }
  }

  if (!broker) {
    DLOG(ERROR) << "Non-broker cannot establish link to unknown node "
                << name.ToString() << " without a broker link.";
    callback(nullptr);
    return;
  }

  broker->RequestIntroduction(name);
}

bool Node::OnRequestIntroduction(NodeLink& from_node_link,
                                 const msg::RequestIntroduction& request) {
  if (type_ != Type::kBroker) {
    return false;
  }

  mem::Ref<NodeLink> other_node_link;
  {
    absl::MutexLock lock(&mutex_);
    auto it = node_links_.find(request.params.name);
    if (it != node_links_.end()) {
      other_node_link = it->second;
    }
  }

  if (!other_node_link) {
    from_node_link.IntroduceNode(request.params.name, nullptr, os::Memory());
    return true;
  }

  os::Memory link_buffer_memory(sizeof(NodeLinkBuffer));
  os::Memory::Mapping link_buffer_mapping = link_buffer_memory.Map();
  NodeLinkBuffer::Init(link_buffer_mapping.base(), /*num_initial_portals=*/0);
  std::pair<mem::Ref<DriverTransport>, mem::Ref<DriverTransport>> transports =
      DriverTransport::CreatePair(driver_, driver_node_);
  other_node_link->IntroduceNode(from_node_link.remote_node_name(),
                                 std::move(transports.first),
                                 link_buffer_memory.Clone());
  from_node_link.IntroduceNode(request.params.name,
                               std::move(transports.second),
                               std::move(link_buffer_memory));
  return true;
}

bool Node::OnIntroduceNode(
    const NodeName& name,
    bool known,
    os::Memory link_buffer_memory,
    absl::Span<const uint8_t> serialized_transport_data,
    absl::Span<os::Handle> serialized_transport_handles) {
  mem::Ref<DriverTransport> transport;
  mem::Ref<NodeLink> new_link;
  if (known) {
    transport = DriverTransport::Deserialize(driver_, driver_node_,
                                             serialized_transport_data,
                                             serialized_transport_handles);
    if (transport) {
      absl::MutexLock lock(&mutex_);
      ABSL_ASSERT(assigned_name_.is_valid());
      DVLOG(3) << "Node " << assigned_name_.ToString()
               << " received introduction to " << name.ToString();
      new_link = mem::MakeRefCounted<NodeLink>(
          mem::WrapRefCounted(this), assigned_name_, name, Type::kNormal, 0,
          transport, link_buffer_memory.Map());
    }
  }

  std::vector<EstablishLinkCallback> callbacks;
  {
    absl::MutexLock lock(&mutex_);
    auto result = node_links_.try_emplace(name, new_link);
    if (!result.second) {
      // Already introduced. Nothing to do.
      return true;
    }

    auto it = pending_introductions_.find(name);
    if (it != pending_introductions_.end()) {
      callbacks = std::move(it->second);
      pending_introductions_.erase(it);
    }
  }

  if (transport) {
    transport->Activate();
  }

  for (EstablishLinkCallback& callback : callbacks) {
    callback(new_link.get());
  }
  return true;
}

bool Node::OnBypassProxy(NodeLink& from_node_link,
                         const msg::BypassProxy& bypass) {
  mem::Ref<NodeLink> proxy_node_link = GetLink(bypass.params.proxy_name);
  if (!proxy_node_link) {
    return true;
  }

  mem::Ref<Router> proxy_peer =
      proxy_node_link->GetRouter(bypass.params.proxy_routing_id);
  if (!proxy_peer) {
    DLOG(ERROR) << "Invalid BypassProxy request for unknown link from "
                << proxy_node_link->local_node_name().ToString() << " to "
                << proxy_node_link->remote_node_name().ToString()
                << " on routing ID " << bypass.params.proxy_routing_id;
    return false;
  }

  // By convention, the initiator of a bypass uses the side A of the bypass
  // link. The receiver of the bypass request uses side B. Bypass links always
  // connect one half of their route to the other.
  mem::Ref<RemoteRouterLink> new_peer_link = from_node_link.AddRoute(
      bypass.params.new_routing_id, bypass.params.new_routing_id,
      LinkType::kCentral, LinkSide::kB, proxy_peer);
  return proxy_peer->BypassProxyWithNewRemoteLink(
      new_peer_link, bypass.params.proxy_outbound_sequence_length);
}

bool Node::AddLink(const NodeName& remote_node_name, mem::Ref<NodeLink> link) {
  absl::MutexLock lock(&mutex_);
  auto result = node_links_.try_emplace(remote_node_name, std::move(link));
  return result.second;
}

}  // namespace core
}  // namespace ipcz
