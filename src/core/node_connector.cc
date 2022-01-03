// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/node_connector.h"

#include <algorithm>
#include <utility>

#include "core/direction.h"
#include "core/driver_transport.h"
#include "core/node_link.h"
#include "core/node_link_memory.h"
#include "core/portal.h"
#include "core/router.h"
#include "core/routing_id.h"
#include "debug/log.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/memory.h"

namespace ipcz {
namespace core {

namespace {

class NodeConnectorForBrokerToNonBroker : public NodeConnector {
 public:
  NodeConnectorForBrokerToNonBroker(
      mem::Ref<Node> node,
      mem::Ref<DriverTransport> transport,
      std::vector<mem::Ref<Portal>> waiting_portals,
      ConnectCallback callback)
      : NodeConnector(std::move(node),
                      std::move(transport),
                      std::move(waiting_portals),
                      std::move(callback)),
        link_memory_(
            NodeLinkMemory::Allocate(num_portals(), link_memory_to_share_)) {}

  ~NodeConnectorForBrokerToNonBroker() override = default;

  // NodeConnector:
  void Connect() override {
    DVLOG(4) << "Sending direct ConnectFromBrokerToNonBroker from broker "
             << broker_name_.ToString() << " to new node "
             << new_remote_node_name_.ToString() << " with " << num_portals()
             << " initial portals";

    ABSL_ASSERT(node_->type() == Node::Type::kBroker);
    msg::ConnectFromBrokerToNonBroker connect;
    connect.params.broker_name = broker_name_;
    connect.params.receiver_name = new_remote_node_name_;
    connect.params.protocol_version = msg::kProtocolVersion;
    connect.params.num_initial_portals = num_portals();
    connect.handles.primary_buffer_memory = link_memory_to_share_.TakeHandle();
    transport_->Transmit(connect);
  }

  bool OnMessage(uint8_t message_id,
                 const DriverTransport::Message& message) override {
    if (message_id != msg::ConnectFromNonBrokerToBroker::kId) {
      return false;
    }

    msg::ConnectFromNonBrokerToBroker connect;
    if (!connect.Deserialize(message)) {
      return false;
    }

    DVLOG(4) << "Accepting ConnectFromNonBrokerToBroker on broker "
             << broker_name_.ToString() << " from new node "
             << new_remote_node_name_.ToString();
    AcceptConnection(mem::MakeRefCounted<NodeLink>(
                         node_, broker_name_, new_remote_node_name_,
                         Node::Type::kNormal, connect.params.protocol_version,
                         transport_, std::move(link_memory_)),
                     LinkSide::kA, connect.params.num_initial_portals);
    return true;
  }

 private:
  const NodeName broker_name_{node_->GetAssignedName()};
  const NodeName new_remote_node_name_{NodeName::kRandom};
  os::Memory link_memory_to_share_;
  NodeLinkMemory link_memory_;
};

class NodeConnectorForNonBrokerToBroker : public NodeConnector {
 public:
  NodeConnectorForNonBrokerToBroker(
      mem::Ref<Node> node,
      mem::Ref<DriverTransport> transport,
      std::vector<mem::Ref<Portal>> waiting_portals,
      ConnectCallback callback)
      : NodeConnector(std::move(node),
                      std::move(transport),
                      std::move(waiting_portals),
                      std::move(callback)) {}

  ~NodeConnectorForNonBrokerToBroker() override = default;

  // NodeConnector:
  void Connect() override {
    ABSL_ASSERT(node_->type() == Node::Type::kNormal);
    msg::ConnectFromNonBrokerToBroker connect;
    connect.params.protocol_version = msg::kProtocolVersion;
    connect.params.num_initial_portals = num_portals();
    transport_->Transmit(connect);
  }

  bool OnMessage(uint8_t message_id,
                 const DriverTransport::Message& message) override {
    if (message_id != msg::ConnectFromBrokerToNonBroker::kId) {
      return false;
    }

    msg::ConnectFromBrokerToNonBroker connect;
    if (!connect.Deserialize(message)) {
      return false;
    }

    DVLOG(4) << "New node accepting ConnectFromBrokerToNonBroker with assigned "
             << "name " << connect.params.receiver_name.ToString()
             << " from broker " << connect.params.broker_name.ToString();

    auto new_link = mem::MakeRefCounted<NodeLink>(
        node_, connect.params.receiver_name, connect.params.broker_name,
        Node::Type::kBroker, connect.params.protocol_version, transport_,
        NodeLinkMemory::Adopt(
            std::move(connect.handles.primary_buffer_memory)));
    node_->SetBrokerLink(new_link);
    node_->SetAssignedName(connect.params.receiver_name);
    AcceptConnection(std::move(new_link), LinkSide::kB,
                     connect.params.num_initial_portals);
    return true;
  }
};

class NodeConnectorForIndirectNonBrokerToBroker : public NodeConnector {
 public:
  NodeConnectorForIndirectNonBrokerToBroker(
      mem::Ref<Node> node,
      mem::Ref<DriverTransport> transport,
      std::vector<mem::Ref<Portal>> waiting_portals,
      ConnectCallback callback)
      : NodeConnector(std::move(node),
                      std::move(transport),
                      std::move(waiting_portals),
                      std::move(callback)) {}

  ~NodeConnectorForIndirectNonBrokerToBroker() override = default;

  // NodeConnector:
  void Connect() override {
    DVLOG(4) << "Sending ConnectToBrokerIndirect";

    msg::ConnectToBrokerIndirect connect;
    connect.params.protocol_version = msg::kProtocolVersion;
    connect.params.num_initial_portals = num_portals();
    transport_->Transmit(connect);
  }

  bool OnMessage(uint8_t message_id,
                 const DriverTransport::Message& message) override {
    if (message_id != msg::ConnectFromBrokerIndirect::kId) {
      return false;
    }

    msg::ConnectFromBrokerIndirect connect;
    if (!connect.Deserialize(message)) {
      return false;
    }

    DVLOG(4) << "Accepting ConnectFromBrokerIndirect from broker "
             << connect.params.broker_name.ToString() << " on new node "
             << connect.params.receiver_name.ToString() << " referred by "
             << connect.params.connected_node_name.ToString();

    auto new_link = mem::MakeRefCounted<NodeLink>(
        node_, connect.params.receiver_name, connect.params.broker_name,
        Node::Type::kBroker, connect.params.protocol_version, transport_,
        NodeLinkMemory::Adopt(
            std::move(connect.handles.primary_buffer_memory)));

    absl::Span<const mem::Ref<Portal>> portals =
        absl::MakeSpan(waiting_portals_);
    const uint32_t num_portals =
        std::min(connect.params.num_remote_portals,
                 static_cast<uint32_t>(portals.size()));

    node_->SetPortalsWaitingForLink(connect.params.connected_node_name,
                                    portals.subspan(0, num_portals));
    for (const mem::Ref<Portal>& portal : portals.subspan(num_portals)) {
      portal->router()->AcceptRouteClosureFrom(Direction::kOutward, 0);
    }

    node_->SetBrokerLink(new_link);
    node_->SetAssignedName(connect.params.receiver_name);
    node_->AddLink(connect.params.broker_name, new_link);
    active_self_.reset();
    return true;
  }
};

class NodeConnectorForIndirectBrokerToNonBroker : public NodeConnector {
 public:
  NodeConnectorForIndirectBrokerToNonBroker(mem::Ref<Node> node,
                                            mem::Ref<NodeLink> referrer,
                                            mem::Ref<DriverTransport> transport,
                                            os::Process remote_process,
                                            uint32_t num_initial_portals,
                                            ConnectIndirectCallback callback)
      : NodeConnector(std::move(node),
                      std::move(transport),
                      {},
                      [this](mem::Ref<NodeLink> link) {
                        indirect_callback_(std::move(link),
                                           num_remote_portals_);
                      }),
        indirect_callback_(std::move(callback)),
        num_initial_portals_(num_initial_portals),
        referrer_(std::move(referrer)),
        remote_process_(std::move(remote_process)),
        link_memory_(NodeLinkMemory::Allocate(0, link_memory_to_share_)) {}

  ~NodeConnectorForIndirectBrokerToNonBroker() override = default;

  // NodeConnector:
  void Connect() override {
    DVLOG(4) << "Sending ConnectFromBrokerIndirect from broker "
             << broker_name_.ToString() << " to new node "
             << new_remote_node_name_.ToString();

    msg::ConnectFromBrokerIndirect connect;
    connect.params.broker_name = broker_name_;
    connect.params.receiver_name = new_remote_node_name_;
    connect.params.connected_node_name = referrer_->remote_node_name();
    connect.params.protocol_version = msg::kProtocolVersion;
    connect.params.num_remote_portals = num_initial_portals_;
    connect.handles.primary_buffer_memory = link_memory_to_share_.TakeHandle();
    transport_->Transmit(connect);
  }

  bool OnMessage(uint8_t message_id,
                 const DriverTransport::Message& message) override {
    if (message_id != msg::ConnectToBrokerIndirect::kId) {
      return false;
    }

    msg::ConnectToBrokerIndirect connect;
    if (!connect.Deserialize(message)) {
      return false;
    }

    DVLOG(4) << "Accepting ConnectToBrokerIndirect on broker "
             << broker_name_.ToString() << " to new node "
             << new_remote_node_name_.ToString() << " referred by "
             << referrer_->remote_node_name().ToString();

    num_remote_portals_ = connect.params.num_initial_portals;

    AcceptConnection(mem::MakeRefCounted<NodeLink>(
                         node_, broker_name_, new_remote_node_name_,
                         Node::Type::kNormal, connect.params.protocol_version,
                         transport_, std::move(link_memory_)),
                     LinkSide::kA, num_remote_portals_);
    return true;
  }

 private:
  const NodeName broker_name_{node_->GetAssignedName()};
  const NodeName new_remote_node_name_{NodeName::kRandom};
  const ConnectIndirectCallback indirect_callback_;
  const uint32_t num_initial_portals_;
  const mem::Ref<NodeLink> referrer_;
  uint32_t num_remote_portals_;
  os::Process remote_process_;
  os::Memory link_memory_to_share_;
  NodeLinkMemory link_memory_;
};

class NodeConnectorForBrokerToBroker : public NodeConnector {
 public:
  NodeConnectorForBrokerToBroker(mem::Ref<Node> node,
                                 mem::Ref<DriverTransport> transport,
                                 std::vector<mem::Ref<Portal>> waiting_portals,
                                 ConnectCallback callback)
      : NodeConnector(std::move(node),
                      std::move(transport),
                      std::move(waiting_portals),
                      std::move(callback)),
        our_link_memory_(NodeLinkMemory::Allocate(num_portals(),
                                                  our_link_memory_to_share_)) {}

  ~NodeConnectorForBrokerToBroker() override = default;

  // NodeConnector:
  void Connect() override {
    DVLOG(4) << "Sending direct ConnectFromBrokerToBroker from broker "
             << local_name_.ToString() << " with " << num_portals()
             << " initial portals";

    ABSL_ASSERT(node_->type() == Node::Type::kBroker);
    msg::ConnectFromBrokerToBroker connect;
    connect.params.sender_name = local_name_;
    connect.params.protocol_version = msg::kProtocolVersion;
    connect.params.num_initial_portals = num_portals();
    connect.handles.primary_buffer_memory =
        our_link_memory_to_share_.TakeHandle();
    transport_->Transmit(connect);
  }

  bool OnMessage(uint8_t message_id,
                 const DriverTransport::Message& message) override {
    if (message_id != msg::ConnectFromBrokerToBroker::kId) {
      return false;
    }

    msg::ConnectFromBrokerToBroker connect;
    if (!connect.Deserialize(message)) {
      return false;
    }

    DVLOG(4) << "Accepting ConnectFromBrokerToBroker on broker "
             << local_name_.ToString() << " from broker "
             << connect.params.sender_name.ToString();

    NodeLinkMemory their_link_memory =
        NodeLinkMemory::Adopt(std::move(connect.handles.primary_buffer_memory));

    const bool we_win = local_name_ < connect.params.sender_name;
    NodeLinkMemory link_memory =
        we_win ? std::move(our_link_memory_) : std::move(their_link_memory);
    LinkSide link_side = we_win ? LinkSide::kA : LinkSide::kB;
    AcceptConnection(mem::MakeRefCounted<NodeLink>(
                         node_, local_name_, connect.params.sender_name,
                         Node::Type::kBroker, connect.params.protocol_version,
                         transport_, std::move(link_memory)),
                     link_side, connect.params.num_initial_portals);
    return true;
  }

 private:
  const NodeName local_name_{node_->GetAssignedName()};
  os::Memory our_link_memory_to_share_;
  NodeLinkMemory our_link_memory_;
};

std::pair<mem::Ref<NodeConnector>, IpczResult> CreateConnector(
    mem::Ref<Node> node,
    mem::Ref<DriverTransport> transport,
    os::Process remote_process,
    IpczCreateNodeFlags flags,
    const std::vector<mem::Ref<Portal>>& initial_portals,
    NodeConnector::ConnectCallback callback) {
  const bool from_broker = node->type() == Node::Type::kBroker;
  const bool to_broker = (flags & IPCZ_CONNECT_NODE_TO_BROKER) != 0;
  const bool inherit_broker = (flags & IPCZ_CONNECT_NODE_INHERIT_BROKER) != 0;
  if (from_broker) {
    if (to_broker) {
      return {mem::MakeRefCounted<NodeConnectorForBrokerToBroker>(
                  std::move(node), std::move(transport), initial_portals,
                  std::move(callback)),
              IPCZ_RESULT_OK};
    }

    return {mem::MakeRefCounted<NodeConnectorForBrokerToNonBroker>(
                std::move(node), std::move(transport), initial_portals,
                std::move(callback)),
            IPCZ_RESULT_OK};
  }

  if (to_broker) {
    return {mem::MakeRefCounted<NodeConnectorForNonBrokerToBroker>(
                std::move(node), std::move(transport), initial_portals,
                std::move(callback)),
            IPCZ_RESULT_OK};
  }

  if (inherit_broker) {
    return {mem::MakeRefCounted<NodeConnectorForIndirectNonBrokerToBroker>(
                std::move(node), std::move(transport), initial_portals,
                std::move(callback)),
            IPCZ_RESULT_OK};
  }

  return {nullptr, IPCZ_RESULT_FAILED_PRECONDITION};
}

}  // namespace

// static
IpczResult NodeConnector::ConnectNode(
    mem::Ref<Node> node,
    mem::Ref<DriverTransport> transport,
    os::Process remote_process,
    IpczCreateNodeFlags flags,
    const std::vector<mem::Ref<Portal>>& initial_portals,
    ConnectCallback callback) {
  const bool from_broker = node->type() == Node::Type::kBroker;
  const bool to_broker = (flags & IPCZ_CONNECT_NODE_TO_BROKER) != 0;
  const bool inherit_broker = (flags & IPCZ_CONNECT_NODE_INHERIT_BROKER) != 0;
  const bool share_broker = (flags & IPCZ_CONNECT_NODE_SHARE_BROKER) != 0;
  mem::Ref<NodeLink> broker_link = node->GetBrokerLink();
  if (share_broker && (from_broker || to_broker || inherit_broker)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if ((to_broker || from_broker) && (inherit_broker || share_broker)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (share_broker) {
    node->AddBrokerCallback([node, transport = std::move(transport),
                             // remote_process=std::move(remote_process),
                             initial_portals](mem::Ref<NodeLink> broker_link) {
      DVLOG(4) << "Requesting indirect broker connection from node "
               << node->GetAssignedName().ToString() << " to broker "
               << broker_link->remote_node_name().ToString();

      // In this case we're a non-broker being asked by another non-broker to
      // introduce them to out broker. We forward the transport to the broker
      // who will complete a handshake with the new non-broker before
      // introducing `node` to it and vice versa.
      broker_link->RequestIndirectBrokerConnection(
          std::move(transport), os::Process(),  // std::move(remote_process),
          initial_portals.size(),
          [node, initial_portals](const NodeName& connected_node_name,
                                  uint32_t num_remote_portals) {
            if (!connected_node_name.is_valid()) {
              DVLOG(4) << "Indirect broker connection failed.";
              for (const mem::Ref<Portal>& portal : initial_portals) {
                portal->router()->AcceptRouteClosureFrom(Direction::kOutward,
                                                         0);
              }
              return;
            }

            absl::Span<const mem::Ref<Portal>> portals =
                absl::MakeSpan(initial_portals);
            const uint32_t num_portals = std::min(
                static_cast<uint32_t>(portals.size()), num_remote_portals);
            // Ensure that these portals get a link as soon as we're introduced
            // to the named node. The introduction will immediately follow the
            // message which invoked this callback from tje same link, so
            // correct ordering is assured.
            node->SetPortalsWaitingForLink(connected_node_name,
                                           portals.subspan(0, num_portals));
            for (const mem::Ref<Portal>& dead_portal :
                 portals.subspan(num_portals)) {
              dead_portal->router()->AcceptRouteClosureFrom(Direction::kOutward,
                                                            0);
            }
          });
    });
    return IPCZ_RESULT_OK;
  }

  mem::Ref<NodeConnector> connector;
  IpczResult result;
  std::tie(connector, result) = CreateConnector(
      std::move(node), std::move(transport), std::move(remote_process), flags,
      initial_portals, std::move(callback));
  if (result != IPCZ_RESULT_OK) {
    return result;
  }

  connector->ActivateTransportAndConnect();
  return IPCZ_RESULT_OK;
}

// static
IpczResult NodeConnector::ConnectNodeIndirect(
    mem::Ref<Node> node,
    mem::Ref<NodeLink> referrer,
    mem::Ref<DriverTransport> transport,
    os::Process remote_process,
    uint32_t num_initial_portals,
    ConnectIndirectCallback callback) {
  ABSL_ASSERT(node->type() == Node::Type::kBroker);
  auto connector =
      mem::MakeRefCounted<NodeConnectorForIndirectBrokerToNonBroker>(
          node, std::move(referrer), std::move(transport),
          std::move(remote_process), num_initial_portals, std::move(callback));
  connector->ActivateTransportAndConnect();
  return IPCZ_RESULT_OK;
}

NodeConnector::NodeConnector(mem::Ref<Node> node,
                             mem::Ref<DriverTransport> transport,
                             std::vector<mem::Ref<Portal>> waiting_portals,
                             ConnectCallback callback)
    : node_(std::move(node)),
      transport_(std::move(transport)),
      waiting_portals_(std::move(waiting_portals)),
      callback_(std::move(callback)) {}

NodeConnector::~NodeConnector() = default;

void NodeConnector::AcceptConnection(mem::Ref<NodeLink> new_link,
                                     LinkSide link_side,
                                     uint32_t num_remote_portals) {
  node_->AddLink(new_link->remote_node_name(), new_link);
  if (callback_) {
    callback_(new_link);
  }
  EstablishWaitingPortals(std::move(new_link), link_side, num_remote_portals);
  active_self_.reset();
}

void NodeConnector::RejectConnection() {
  if (callback_) {
    callback_(nullptr);
  }
  EstablishWaitingPortals(nullptr, LinkSide::kA, 0);
  active_self_.reset();
}

void NodeConnector::ActivateTransportAndConnect() {
  active_self_ = mem::WrapRefCounted(this);
  transport_->set_listener(this);
  transport_->Activate();
  Connect();
}

void NodeConnector::EstablishWaitingPortals(mem::Ref<NodeLink> to_link,
                                            LinkSide link_side,
                                            size_t max_valid_portals) {
  ABSL_ASSERT(to_link != nullptr || max_valid_portals == 0);
  const size_t num_valid_portals =
      std::min(max_valid_portals, waiting_portals_.size());
  for (size_t i = 0; i < num_valid_portals; ++i) {
    const mem::Ref<Router> router = waiting_portals_[i]->router();
    router->SetOutwardLink(to_link->AddRoute(
        static_cast<RoutingId>(i), to_link->GetInitialRouterLinkState(i),
        LinkType::kCentral, link_side, router));
  }
  for (size_t i = num_valid_portals; i < waiting_portals_.size(); ++i) {
    waiting_portals_[i]->router()->AcceptRouteClosureFrom(Direction::kOutward,
                                                          0);
  }
}

IpczResult NodeConnector::OnTransportMessage(
    const DriverTransport::Message& message) {
  const auto& header =
      *reinterpret_cast<const internal::MessageHeader*>(message.data.data());
  if (!OnMessage(header.message_id, message)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  return IPCZ_RESULT_OK;
}

void NodeConnector::OnTransportError() {
  RejectConnection();
}

}  // namespace core
}  // namespace ipcz
