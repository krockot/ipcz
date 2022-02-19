// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/node_connector.h"

#include <algorithm>
#include <utility>

#include "ipcz/driver_memory.h"
#include "ipcz/driver_transport.h"
#include "ipcz/ipcz.h"
#include "ipcz/link_side.h"
#include "ipcz/node_link.h"
#include "ipcz/node_link_memory.h"
#include "ipcz/portal.h"
#include "ipcz/router.h"
#include "ipcz/sublink_id.h"
#include "util/log.h"
#include "util/ref_counted.h"

namespace ipcz {

namespace {

class NodeConnectorForBrokerToNonBroker : public NodeConnector {
 public:
  NodeConnectorForBrokerToNonBroker(Ref<Node> node,
                                    Ref<DriverTransport> transport,
                                    OSProcess remote_process,
                                    IpczConnectNodeFlags flags,
                                    std::vector<Ref<Portal>> waiting_portals,
                                    ConnectCallback callback)
      : NodeConnector(std::move(node),
                      std::move(transport),
                      std::move(remote_process),
                      flags,
                      std::move(waiting_portals),
                      std::move(callback)),
        link_memory_(NodeLinkMemory::Allocate(node_,
                                              num_portals(),
                                              link_memory_to_share_)) {}

  ~NodeConnectorForBrokerToNonBroker() override = default;

  // NodeConnector:
  void Connect() override {
    DVLOG(4) << "Sending direct ConnectFromBrokerToNonBroker from broker "
             << broker_name_.ToString() << " to new node "
             << new_remote_node_name_.ToString() << " with " << num_portals()
             << " initial portals";

    ABSL_ASSERT(node_->type() == Node::Type::kBroker);
    msg::ConnectFromBrokerToNonBroker connect;
    connect.params().broker_name = broker_name_;
    connect.params().receiver_name = new_remote_node_name_;
    connect.params().protocol_version = msg::kProtocolVersion;
    connect.params().num_initial_portals = num_portals();

    auto buffer = connect.AppendSharedMemory(node_->driver(),
                                             std::move(link_memory_to_share_));
    connect.params().set_buffer(buffer);

    transport_->Transmit(connect, remote_process_);
  }

  bool OnMessage(uint8_t message_id,
                 const DriverTransport::Message& message) override {
    if (message_id != msg::ConnectFromNonBrokerToBroker::kId) {
      return false;
    }

    msg::ConnectFromNonBrokerToBroker connect;
    if (!connect.Deserialize(message, remote_process_)) {
      return false;
    }

    DVLOG(4) << "Accepting ConnectFromNonBrokerToBroker on broker "
             << broker_name_.ToString() << " from new node "
             << new_remote_node_name_.ToString();
    AcceptConnection(
        NodeLink::Create(node_, LinkSide::kA, broker_name_,
                         new_remote_node_name_, Node::Type::kNormal,
                         connect.params().protocol_version, transport_,
                         std::move(remote_process_), std::move(link_memory_)),
        LinkSide::kA, connect.params().num_initial_portals);
    return true;
  }

 private:
  const NodeName broker_name_{node_->GetAssignedName()};
  const NodeName new_remote_node_name_{NodeName::kRandom};
  DriverMemory link_memory_to_share_;
  Ref<NodeLinkMemory> link_memory_;
};

class NodeConnectorForNonBrokerToBroker : public NodeConnector {
 public:
  NodeConnectorForNonBrokerToBroker(Ref<Node> node,
                                    Ref<DriverTransport> transport,
                                    OSProcess remote_process,
                                    IpczConnectNodeFlags flags,
                                    std::vector<Ref<Portal>> waiting_portals,
                                    ConnectCallback callback)
      : NodeConnector(std::move(node),
                      std::move(transport),
                      std::move(remote_process),
                      flags,
                      std::move(waiting_portals),
                      std::move(callback)) {}

  ~NodeConnectorForNonBrokerToBroker() override = default;

  // NodeConnector:
  void Connect() override {
    ABSL_ASSERT(node_->type() == Node::Type::kNormal);
    msg::ConnectFromNonBrokerToBroker connect;
    connect.params().protocol_version = msg::kProtocolVersion;
    connect.params().num_initial_portals = num_portals();
    transport_->Transmit(connect, remote_process_);
  }

  bool OnMessage(uint8_t message_id,
                 const DriverTransport::Message& message) override {
    if (message_id != msg::ConnectFromBrokerToNonBroker::kId) {
      return false;
    }

    msg::ConnectFromBrokerToNonBroker connect;
    if (!connect.Deserialize(message, remote_process_)) {
      return false;
    }

    DVLOG(4) << "New node accepting ConnectFromBrokerToNonBroker with assigned "
             << "name " << connect.params().receiver_name.ToString()
             << " from broker " << connect.params().broker_name.ToString();

    DriverMemory buffer_memory =
        connect.TakeSharedMemory(node_->driver(), connect.params().buffer());
    if (!buffer_memory.is_valid()) {
      return false;
    }

    auto new_link = NodeLink::Create(
        node_, LinkSide::kB, connect.params().receiver_name,
        connect.params().broker_name, Node::Type::kBroker,
        connect.params().protocol_version, transport_,
        std::move(remote_process_),
        NodeLinkMemory::Adopt(node_, std::move(buffer_memory)));
    node_->SetAssignedName(connect.params().receiver_name);
    node_->SetBrokerLink(new_link);
    if (flags_ & IPCZ_CONNECT_NODE_TO_ALLOCATION_DELEGATE) {
      node_->SetAllocationDelegate(new_link);
    }
    AcceptConnection(std::move(new_link), LinkSide::kB,
                     connect.params().num_initial_portals);
    return true;
  }
};

class NodeConnectorForIndirectNonBrokerToBroker : public NodeConnector {
 public:
  NodeConnectorForIndirectNonBrokerToBroker(
      Ref<Node> node,
      Ref<DriverTransport> transport,
      OSProcess remote_process,
      IpczConnectNodeFlags flags,
      std::vector<Ref<Portal>> waiting_portals,
      ConnectCallback callback)
      : NodeConnector(std::move(node),
                      std::move(transport),
                      std::move(remote_process),
                      flags,
                      std::move(waiting_portals),
                      std::move(callback)) {}

  ~NodeConnectorForIndirectNonBrokerToBroker() override = default;

  // NodeConnector:
  void Connect() override {
    DVLOG(4) << "Sending ConnectToBrokerIndirect";

    msg::ConnectToBrokerIndirect connect;
    connect.params().protocol_version = msg::kProtocolVersion;
    connect.params().num_initial_portals = num_portals();
    transport_->Transmit(connect, remote_process_);
  }

  bool OnMessage(uint8_t message_id,
                 const DriverTransport::Message& message) override {
    if (message_id != msg::ConnectFromBrokerIndirect::kId) {
      return false;
    }

    msg::ConnectFromBrokerIndirect connect;
    if (!connect.Deserialize(message, remote_process_)) {
      return false;
    }

    DVLOG(4) << "Accepting ConnectFromBrokerIndirect from broker "
             << connect.params().broker_name.ToString() << " on new node "
             << connect.params().receiver_name.ToString() << " referred by "
             << connect.params().connected_node_name.ToString();

    DriverMemory buffer_memory =
        connect.TakeSharedMemory(node_->driver(), connect.params().buffer());
    if (!buffer_memory.is_valid()) {
      return false;
    }

    auto new_link = NodeLink::Create(
        node_, LinkSide::kB, connect.params().receiver_name,
        connect.params().broker_name, Node::Type::kBroker,
        connect.params().protocol_version, transport_,
        std::move(remote_process_),
        NodeLinkMemory::Adopt(node_, std::move(buffer_memory)));

    absl::Span<const Ref<Portal>> portals = absl::MakeSpan(waiting_portals_);
    const uint32_t num_portals =
        std::min(connect.params().num_remote_portals,
                 static_cast<uint32_t>(portals.size()));

    node_->SetPortalsWaitingForLink(connect.params().connected_node_name,
                                    portals.subspan(0, num_portals));
    for (const Ref<Portal>& portal : portals.subspan(num_portals)) {
      portal->router()->AcceptRouteClosureFrom(LinkType::kCentral, 0);
    }

    node_->SetAssignedName(connect.params().receiver_name);
    node_->SetBrokerLink(new_link);
    if (flags_ & IPCZ_CONNECT_NODE_TO_ALLOCATION_DELEGATE) {
      node_->SetAllocationDelegate(new_link);
    }
    node_->AddLink(connect.params().broker_name, std::move(new_link));
    active_self_.reset();
    return true;
  }
};

class NodeConnectorForIndirectBrokerToNonBroker : public NodeConnector {
 public:
  NodeConnectorForIndirectBrokerToNonBroker(Ref<Node> node,
                                            Ref<NodeLink> referrer,
                                            Ref<DriverTransport> transport,
                                            OSProcess remote_process,
                                            uint32_t num_initial_portals,
                                            ConnectIndirectCallback callback)
      : NodeConnector(std::move(node),
                      std::move(transport),
                      std::move(remote_process),
                      IPCZ_NO_FLAGS,
                      {},
                      [this](Ref<NodeLink> link) {
                        indirect_callback_(std::move(link),
                                           num_remote_portals_);
                      }),
        indirect_callback_(std::move(callback)),
        num_initial_portals_(num_initial_portals),
        referrer_(std::move(referrer)),
        link_memory_(
            NodeLinkMemory::Allocate(node_, 0, link_memory_to_share_)) {}

  ~NodeConnectorForIndirectBrokerToNonBroker() override = default;

  // NodeConnector:
  void Connect() override {
    DVLOG(4) << "Sending ConnectFromBrokerIndirect from broker "
             << broker_name_.ToString() << " to new node "
             << new_remote_node_name_.ToString();

    msg::ConnectFromBrokerIndirect connect;
    connect.params().broker_name = broker_name_;
    connect.params().receiver_name = new_remote_node_name_;
    connect.params().connected_node_name = referrer_->remote_node_name();
    connect.params().protocol_version = msg::kProtocolVersion;
    connect.params().num_remote_portals = num_initial_portals_;

    auto buffer = connect.AppendSharedMemory(node_->driver(),
                                             std::move(link_memory_to_share_));
    connect.params().set_buffer(buffer);

    transport_->Transmit(connect, remote_process_);
  }

  bool OnMessage(uint8_t message_id,
                 const DriverTransport::Message& message) override {
    if (message_id != msg::ConnectToBrokerIndirect::kId) {
      return false;
    }

    msg::ConnectToBrokerIndirect connect;
    if (!connect.Deserialize(message, remote_process_)) {
      return false;
    }

    DVLOG(4) << "Accepting ConnectToBrokerIndirect on broker "
             << broker_name_.ToString() << " to new node "
             << new_remote_node_name_.ToString() << " referred by "
             << referrer_->remote_node_name().ToString();

    num_remote_portals_ = connect.params().num_initial_portals;

    AcceptConnection(
        NodeLink::Create(node_, LinkSide::kA, broker_name_,
                         new_remote_node_name_, Node::Type::kNormal,
                         connect.params().protocol_version, transport_,
                         std::move(remote_process_), std::move(link_memory_)),
        LinkSide::kA, num_remote_portals_);
    return true;
  }

 private:
  const NodeName broker_name_{node_->GetAssignedName()};
  const NodeName new_remote_node_name_{NodeName::kRandom};
  const ConnectIndirectCallback indirect_callback_;
  const uint32_t num_initial_portals_;
  const Ref<NodeLink> referrer_;
  uint32_t num_remote_portals_;
  DriverMemory link_memory_to_share_;
  Ref<NodeLinkMemory> link_memory_;
};

class NodeConnectorForBrokerToBroker : public NodeConnector {
 public:
  NodeConnectorForBrokerToBroker(Ref<Node> node,
                                 Ref<DriverTransport> transport,
                                 OSProcess remote_process,
                                 IpczConnectNodeFlags flags,
                                 std::vector<Ref<Portal>> waiting_portals,
                                 ConnectCallback callback)
      : NodeConnector(std::move(node),
                      std::move(transport),
                      std::move(remote_process),
                      flags,
                      std::move(waiting_portals),
                      std::move(callback)),
        our_link_memory_(NodeLinkMemory::Allocate(node_,
                                                  num_portals(),
                                                  our_link_memory_to_share_)) {}

  ~NodeConnectorForBrokerToBroker() override = default;

  // NodeConnector:
  void Connect() override {
    DVLOG(4) << "Sending direct ConnectFromBrokerToBroker from broker "
             << local_name_.ToString() << " with " << num_portals()
             << " initial portals";

    ABSL_ASSERT(node_->type() == Node::Type::kBroker);
    msg::ConnectFromBrokerToBroker connect;
    connect.params().sender_name = local_name_;
    connect.params().protocol_version = msg::kProtocolVersion;
    connect.params().num_initial_portals = num_portals();

    auto buffer = connect.AppendSharedMemory(
        node_->driver(), std::move(our_link_memory_to_share_));
    connect.params().set_buffer(buffer);

    transport_->Transmit(connect, remote_process_);
  }

  bool OnMessage(uint8_t message_id,
                 const DriverTransport::Message& message) override {
    if (message_id != msg::ConnectFromBrokerToBroker::kId) {
      return false;
    }

    msg::ConnectFromBrokerToBroker connect;
    if (!connect.Deserialize(message, remote_process_)) {
      return false;
    }

    DVLOG(4) << "Accepting ConnectFromBrokerToBroker on broker "
             << local_name_.ToString() << " from broker "
             << connect.params().sender_name.ToString();

    DriverMemory buffer_memory =
        connect.TakeSharedMemory(node_->driver(), connect.params().buffer());
    if (!buffer_memory.is_valid()) {
      return false;
    }

    Ref<NodeLinkMemory> their_link_memory =
        NodeLinkMemory::Adopt(node_, std::move(buffer_memory));

    const bool we_win = local_name_ < connect.params().sender_name;
    Ref<NodeLinkMemory> link_memory =
        we_win ? std::move(our_link_memory_) : std::move(their_link_memory);
    LinkSide link_side = we_win ? LinkSide::kA : LinkSide::kB;
    AcceptConnection(
        NodeLink::Create(node_, link_side, local_name_,
                         connect.params().sender_name, Node::Type::kBroker,
                         connect.params().protocol_version, transport_,
                         std::move(remote_process_), std::move(link_memory)),
        link_side, connect.params().num_initial_portals);
    return true;
  }

 private:
  const NodeName local_name_{node_->GetAssignedName()};
  DriverMemory our_link_memory_to_share_;
  Ref<NodeLinkMemory> our_link_memory_;
};

std::pair<Ref<NodeConnector>, IpczResult> CreateConnector(
    Ref<Node> node,
    Ref<DriverTransport> transport,
    OSProcess remote_process,
    IpczCreateNodeFlags flags,
    const std::vector<Ref<Portal>>& initial_portals,
    NodeConnector::ConnectCallback callback) {
  const bool from_broker = node->type() == Node::Type::kBroker;
  const bool to_broker = (flags & IPCZ_CONNECT_NODE_TO_BROKER) != 0;
  const bool inherit_broker = (flags & IPCZ_CONNECT_NODE_INHERIT_BROKER) != 0;
  if (from_broker) {
    if (to_broker) {
      return {
          MakeRefCounted<NodeConnectorForBrokerToBroker>(
              std::move(node), std::move(transport), std::move(remote_process),
              flags, initial_portals, std::move(callback)),
          IPCZ_RESULT_OK};
    }

    return {
        MakeRefCounted<NodeConnectorForBrokerToNonBroker>(
            std::move(node), std::move(transport), std::move(remote_process),
            flags, initial_portals, std::move(callback)),
        IPCZ_RESULT_OK};
  }

  if (to_broker) {
    return {
        MakeRefCounted<NodeConnectorForNonBrokerToBroker>(
            std::move(node), std::move(transport), std::move(remote_process),
            flags, initial_portals, std::move(callback)),
        IPCZ_RESULT_OK};
  }

  if (inherit_broker) {
    return {
        MakeRefCounted<NodeConnectorForIndirectNonBrokerToBroker>(
            std::move(node), std::move(transport), std::move(remote_process),
            flags, initial_portals, std::move(callback)),
        IPCZ_RESULT_OK};
  }

  return {nullptr, IPCZ_RESULT_FAILED_PRECONDITION};
}

}  // namespace

// static
IpczResult NodeConnector::ConnectNode(
    Ref<Node> node,
    Ref<DriverTransport> transport,
    OSProcess remote_process,
    IpczCreateNodeFlags flags,
    const std::vector<Ref<Portal>>& initial_portals,
    ConnectCallback callback) {
  const bool from_broker = node->type() == Node::Type::kBroker;
  const bool to_broker = (flags & IPCZ_CONNECT_NODE_TO_BROKER) != 0;
  const bool inherit_broker = (flags & IPCZ_CONNECT_NODE_INHERIT_BROKER) != 0;
  const bool share_broker = (flags & IPCZ_CONNECT_NODE_SHARE_BROKER) != 0;
  Ref<NodeLink> broker_link = node->GetBrokerLink();
  if (share_broker && (from_broker || to_broker || inherit_broker)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if ((to_broker || from_broker) && (inherit_broker || share_broker)) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }
  if (share_broker) {
    node->AddBrokerCallback([node, transport = std::move(transport),
                             remote_process = std::move(remote_process),
                             initial_portals](
                                Ref<NodeLink> broker_link) mutable {
      DVLOG(4) << "Requesting indirect broker connection from node "
               << node->GetAssignedName().ToString() << " to broker "
               << broker_link->remote_node_name().ToString();

      // In this case we're a non-broker being asked by another non-broker to
      // introduce them to out broker. We forward the transport to the broker
      // who will complete a handshake with the new non-broker before
      // introducing `node` to it and vice versa.
      broker_link->RequestIndirectBrokerConnection(
          std::move(transport), std::move(remote_process),
          initial_portals.size(),
          [node, initial_portals](const NodeName& connected_node_name,
                                  uint32_t num_remote_portals) {
            if (!connected_node_name.is_valid()) {
              DVLOG(4) << "Indirect broker connection failed.";
              for (const Ref<Portal>& portal : initial_portals) {
                portal->router()->AcceptRouteClosureFrom(LinkType::kCentral, 0);
              }
              return;
            }

            absl::Span<const Ref<Portal>> portals =
                absl::MakeSpan(initial_portals);
            const uint32_t num_portals = std::min(
                static_cast<uint32_t>(portals.size()), num_remote_portals);
            // Ensure that these portals get a link as soon as we're introduced
            // to the named node. The introduction will immediately follow the
            // message which invoked this callback from the same link, so
            // correct ordering is assured.
            node->SetPortalsWaitingForLink(connected_node_name,
                                           portals.subspan(0, num_portals));
            for (const Ref<Portal>& dead_portal :
                 portals.subspan(num_portals)) {
              dead_portal->router()->AcceptRouteClosureFrom(LinkType::kCentral,
                                                            0);
            }
          });
    });
    return IPCZ_RESULT_OK;
  }

  auto [connector, result] = CreateConnector(
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
    Ref<Node> node,
    Ref<NodeLink> referrer,
    Ref<DriverTransport> transport,
    OSProcess remote_process,
    uint32_t num_initial_portals,
    ConnectIndirectCallback callback) {
  ABSL_ASSERT(node->type() == Node::Type::kBroker);
  auto connector = MakeRefCounted<NodeConnectorForIndirectBrokerToNonBroker>(
      node, std::move(referrer), std::move(transport),
      std::move(remote_process), num_initial_portals, std::move(callback));
  connector->ActivateTransportAndConnect();
  return IPCZ_RESULT_OK;
}

NodeConnector::NodeConnector(Ref<Node> node,
                             Ref<DriverTransport> transport,
                             OSProcess remote_process,
                             IpczCreateNodeFlags flags,
                             std::vector<Ref<Portal>> waiting_portals,
                             ConnectCallback callback)
    : node_(std::move(node)),
      transport_(std::move(transport)),
      remote_process_(std::move(remote_process)),
      flags_(flags),
      waiting_portals_(std::move(waiting_portals)),
      callback_(std::move(callback)) {}

NodeConnector::~NodeConnector() = default;

void NodeConnector::AcceptConnection(Ref<NodeLink> new_link,
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
  active_self_ = WrapRefCounted(this);
  transport_->set_listener(this);
  transport_->Activate();
  Connect();
}

void NodeConnector::EstablishWaitingPortals(Ref<NodeLink> to_link,
                                            LinkSide link_side,
                                            size_t max_valid_portals) {
  ABSL_ASSERT(to_link != nullptr || max_valid_portals == 0);
  const size_t num_valid_portals =
      std::min(max_valid_portals, waiting_portals_.size());
  for (size_t i = 0; i < num_valid_portals; ++i) {
    const Ref<Router> router = waiting_portals_[i]->router();
    router->SetOutwardLink(to_link->AddRemoteRouterLink(
        static_cast<SublinkId>(i),
        to_link->memory().GetInitialRouterLinkState(i), LinkType::kCentral,
        link_side, router));
  }
  for (size_t i = num_valid_portals; i < waiting_portals_.size(); ++i) {
    waiting_portals_[i]->router()->AcceptRouteClosureFrom(LinkType::kCentral,
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

}  // namespace ipcz
