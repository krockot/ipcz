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
#include "core/message_internal.h"
#include "core/node_link.h"
#include "core/node_link_buffer.h"
#include "core/node_messages.h"
#include "core/portal.h"
#include "core/router.h"
#include "core/routing_id.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/memory.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/handle_util.h"

namespace ipcz {
namespace core {

namespace {

class ConnectListener : public DriverTransport::Listener,
                        public mem::RefCounted {
 public:
  ConnectListener(mem::Ref<Node> node,
                  mem::Ref<DriverTransport> transport,
                  std::vector<mem::Ref<Portal>> waiting_portals,
                  os::Memory::Mapping link_buffer_mapping)
      : node_(std::move(node)),
        transport_(std::move(transport)),
        waiting_portals_(std::move(waiting_portals)),
        link_buffer_mapping_(std::move(link_buffer_mapping)) {}

  using ConnectHandler = std::function<void(mem::Ref<NodeLink> link)>;

  void Listen(ConnectHandler handler) { handler_ = std::move(handler); }

  // DriverTransport::Listener:
  IpczResult OnTransportMessage(
      const DriverTransport::Message& message) override {
    const auto& header =
        *reinterpret_cast<const internal::MessageHeader*>(message.data.data());
    if (header.message_id != msg::Connect::kId) {
      return IPCZ_RESULT_UNIMPLEMENTED;
    }

    msg::Connect connect;
    if (!connect.Deserialize(message)) {
      handler_ = nullptr;
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    if (!link_buffer_mapping_.is_valid()) {
      os::Memory memory(std::move(connect.handles.link_state_memory),
                        sizeof(NodeLinkBuffer));
      link_buffer_mapping_ = memory.Map();
    }

    auto node_link = mem::MakeRefCounted<NodeLink>(
        node_, connect.params.name, connect.params.protocol_version, transport_,
        std::move(link_buffer_mapping_));

    if (!handler_) {
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    ConnectHandler handler = std::move(handler_);
    handler(node_link);
    for (size_t i = 0; i < waiting_portals_.size(); ++i) {
      const mem::Ref<Router>& router = waiting_portals_[i]->router();
      router->SetPeer(
          node_link->AddRoute(static_cast<RoutingId>(i), i, router));
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
  const mem::Ref<DriverTransport> transport_;
  const std::vector<mem::Ref<Portal>> waiting_portals_;
  os::Memory::Mapping link_buffer_mapping_;
  ConnectHandler handler_;
};

}  // namespace

Node::Node(Type type, const IpczDriver& driver, IpczDriverHandle driver_node)
    : type_(type), driver_(driver), driver_node_(driver_node) {}

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
  const Side side = type_ == Type::kBroker ? Side::kLeft : Side::kRight;
  for (size_t i = 0; i < initial_portals.size(); ++i) {
    auto portal = mem::MakeRefCounted<Portal>(
        mem::WrapRefCounted(this), mem::MakeRefCounted<Router>(side));
    buffering_portals[i] = portal;
    initial_portals[i] = ToHandle(portal.release());
  }

  const uint32_t num_portals = static_cast<uint32_t>(initial_portals.size());
  os::Memory::Mapping link_state_mapping;
  msg::Connect connect;
  connect.params.protocol_version = msg::kProtocolVersion;
  connect.params.name = name_;
  connect.params.num_initial_portals = num_portals;
  if (type_ == Type::kBroker) {
    os::Memory link_state_memory(sizeof(NodeLinkBuffer));
    link_state_mapping = link_state_memory.Map();
    NodeLinkBuffer::Init(link_state_mapping.base(), num_portals);
    connect.handles.link_state_memory = link_state_memory.TakeHandle();
  }

  auto transport =
      mem::MakeRefCounted<DriverTransport>(driver_, driver_transport);
  auto listener = mem::MakeRefCounted<ConnectListener>(
      mem::WrapRefCounted(this), transport, std::move(buffering_portals),
      std::move(link_state_mapping));
  listener->Listen(
      [node = mem::WrapRefCounted(this), listener](mem::Ref<NodeLink> link) {
        const NodeName name = link->remote_node_name();
        node->AddLink(name, std::move(link));
      });

  transport->set_listener(listener.get());
  transport->Activate();
  transport->Transmit(connect);

  // TODO
  (void)driver_node_;

  return IPCZ_RESULT_OK;
}

std::pair<mem::Ref<Portal>, mem::Ref<Portal>> Node::OpenPortals() {
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
  callback(nullptr);
}

bool Node::AddLink(const NodeName& remote_node_name, mem::Ref<NodeLink> link) {
  absl::MutexLock lock(&mutex_);
  auto result = node_links_.try_emplace(remote_node_name, std::move(link));
  return result.second;
}

}  // namespace core
}  // namespace ipcz
