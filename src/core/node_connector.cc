// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/node_connector.h"

#include <algorithm>
#include <utility>

#include "core/direction.h"
#include "core/node_link.h"
#include "core/node_link_buffer.h"
#include "core/portal.h"
#include "core/router.h"
#include "core/routing_id.h"
#include "mem/ref_counted.h"
#include "os/memory.h"
#include "ipcz/ipcz.h"

namespace ipcz {
namespace core {

namespace {

class NodeConnectorForBrokerToNonBroker : public NodeConnector {
 public:
  NodeConnectorForBrokerToNonBroker(
      mem::Ref<Node> node,
      const NodeName& broker_name,
      const NodeName& new_remote_node_name,
      mem::Ref<DriverTransport> transport,
      std::vector<mem::Ref<Portal>> waiting_portals)
      : NodeConnector(std::move(node), std::move(transport),
                      std::move(waiting_portals)) {
      NodeLinkBuffer::Init(initial_link_buffer_mapping_.base(), num_portals());
    }

  ~NodeConnectorForBrokerToNonBroker() = default;

  // NodeConnector:
  void Connect() override {
    ABSL_ASSERT(node_->GetType() == Node::Type::kBroker);

    msg::ConnectFromBroker connect;
    connect.params.broker_name = broker_name_;
    connect.params.receiver_name = new_remote_node_name_;
    connect.params.protocol_version = msg::kProtocolVersion;
    connect.params.num_initial_portals = num_portals();
    connect.handles.link_state_memory =
        initial_link_buffer_memory_.TakeHandle();
    transport_->Transmit(connect);
  }

  bool OnMessage(uint8_t message_id,
                 const DriverTransport::Message& message) override {
    if (message_id != msg::ConnectToBroker) {
      return false;
    }

    msg::ConnectToBroker connect;
    if (!connect.Deserialize(message)) {
      return false;
    }

    AcceptConnection(mem::MakeRefCounted<NodeLink>(
        node_, broker_name_, new_remote_node_name_, Node::Type::kNormal,
        connect.params.protocol_version, transport_,
        std::move(initial_link_buffer_mapping_)),
        LinkSide::kA,
        connect.params.num_initial_portals);
    return true;
  }

 private:
  const NodeName broker_name_{node_->GetAssignedName()};
  const NodeName new_remote_node_name_{NodeName::kRandom};
  os::Memory initial_link_buffer_memory_{sizeof(NodeLinkBuffer)};
  os::Memory::Mapping initial_link_buffer_mapping_{
      initial_link_buffer_memory_.Map()};
};

class NodeConnectorForNonBrokerToBroker : public NodeConnector {
 public:
  NodeConnectorForNonBrokerToBroker(
      mem::Ref<Node> node,
      mem::Ref<DriverTransport> transport,
      std::vector<mem::Ref<Portal>> waiting_portals)
      : NodeConnector(std::move(node), std::move(transport),
                      std::move(waiting_portals)) {

  }

  ~NodeConnectorForNonBrokerToBroker() = default;

  // NodeConnector:
  void Connect() override {}

  bool OnMessage(uint8_t message_id,
                 const DriverTransport::Message& message) override {
    return false;
  }

 private:
};

class NodeConnectorToInheritBroker : public NodeConnector {
 public:
  NodeConnectorToInheritBroker(
      mem::Ref<Node> node,
      mem::Ref<DriverTransport> transport,
      std::vector<mem::Ref<Portal>> waiting_portals)
      : NodeConnector(std::move(node), std::move(transport),
                      std::move(waiting_portals)) {

  }

  ~NodeConnectorToInheritBroker() = default;

  // NodeConnector:
  void Connect() override {}

  bool OnMessage(uint8_t message_id,
                 const DriverTransport::Message& message) override {
    return false;
  }

 private:
};

class NodeConnectorToIntroduceBroker : public NodeConnector {
 public:
  NodeConnectorToIntroduceBroker(
      mem::Ref<Node> node,
      mem::Ref<DriverTransport> transport,
      std::vector<mem::Ref<Portal>> waiting_portals)
      : NodeConnector(std::move(node), std::move(transport),
                      std::move(waiting_portals)) {

  }

  ~NodeConnectorToIntroduceBroker() = default;

  // NodeConnector:
  void Connect() override {}

  bool OnMessage(uint8_t message_id,
                 const DriverTransport::Message& message) override {
    return false;
  }

 private:
};

}  // namespace

// static
mem::ref<NodeConnector> NodeConnector::ConnectBrokerToNonBroker(
    mem::Ref<Node> node,
    const NodeName& broker_name,
    const NodeName& new_remote_node_name,
    mem::Ref<DriverTransport> transport,
    std::vector<mem::Ref<Portal>> waiting_portals) {
  auto connector = mem::MakeRefCounted<NodeConnectorForBrokerToNonBroker>(
      std::move(node), broker_name, new_remote_node_name, std::move(transport),
      std::move(waiting_portals));
  connector->ActivateTransportAndConnect();
  return connector;
}

// static
mem::Ref<NodeConnector> NodeConnector::ConnectNonBrokerToBroker(
    mem::Ref<Node> node,
    mem::Ref<DriverTransport> transport,
    std::vector<mem::Ref<Portal>> waiting_portals) {
  auto connector = mem::MakeRefCounted<NodeConnectorForNonBrokerToBroker>(
      std::move(node), std::move(transport), std::move(waiting_portals));
  connector->ActivateTransportAndConnect();
  return connector;
}

// static
mem::Ref<NodeConnector> NodeConnector::ConnectAndInheritBroker(
    mem::Ref<Node> node,
    mem::Ref<DriverTransport> transport,
    std::vector<mem::Ref<Portal>> waiting_portals) {
  auto connector = mem::MakeRefCounted<NodeConnectorToInheritBroker>(
      std::move(node), std::move(transport), std::move(waiting_portals));
  connector->ActivateTransportAndConnect();
  return connector;
}

// static
mem::Ref<NodeConnector> NodeConnector::ConnectAndIntroduceBroker(
    mem::Ref<Node> node,
    mem::Ref<DriverTransport> transport,
    std::vector<mem::Ref<Portal>> waiting_portals) {
  auto connector = mem::MakeRefCounted<NodeConnectorToIntroduceBroker>(
      std::move(node), std::move(transport), std::move(waiting_portals));
  connector->ActivateTransportAndConnect();
  return connector;
}

NodeConnector::NodeConnector(mem::Ref<Node> node,
                             mem::Ref<DriverTransport> transport,
                             std::vector<mem::Ref<Portal>> waiting_portals)
    : node_(std::move(node)),
      transport_(std::move(transport)),
      waiting_portals_(std::move(waiting_portals)) {}

NodeConnector::~NodeConnector() = default;

void NodeConnector::AcceptConnection(mem::Ref<NodeLink> new_link,
                                     LinkSide link_side,
                                     uint32_t num_remote_portals) {
  node_->AddLink(new_link->remote_node_name(), new_link);
  EstablishWaitingPortals(std::move(new_link), link_side, num_remote_portals);
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
    portals[i]->router()->SetOutwardLink(to_link->AddRoute(
        static_cast<RoutingId>(i), i, LinkType::kCentral, link_side,
        portals[i]->router()));
  }
  for (size_t i = num_valid_portals; i < waiting_portals_.size(); ++i) {
    portals[i]->router()->AcceptRouteClosure(Direction::kOutward, 0);
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
  EstablishWaitingPortals(nullptr, LinkSide::kA, 0);
  active_self_.reset();
}

}  // namespace core
}  // namespace ipcz
