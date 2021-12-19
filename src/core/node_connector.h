// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_NODE_CONNECTOR_H_
#define IPCZ_SRC_CORE_NODE_CONNECTOR_H_

#include <cstdint>
#include <vector>

#include "core/driver_transport.h"
#include "core/link_side.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"

namespace ipcz {
namespace core {

class Node;
class NodeLink;
class Portal;

// A NodeConnector activates and temporarily attaches itself to a
// DriverTransport to listen listen for and transmit introductory messages
// between two nodes invoking ConnectNode() on opposite ends of the same
// transport pair.
//
// Once an initial handshake is complete the underlying transport is adopted by
// a new NodeLink and handed off to the local Node to communicate with the
// remote node, and this object is destroyed.
class NodeConnector : public mem::RefCounted,
                      public DriverTransport::Listener {
 public:
  // Constructs a new NodeConnector on a broker node expecting a connection from
  // a non-broker node over `transport`. The non-broker node will be assigned
  // `new_remote_node_name` as its name.
  static mem::Ref<NodeConnector> ConnectBrokerToNonBroker(
      mem::Ref<Node> node,
      const NodeName& broker_name,
      const NodeName& new_remote_node_name,
      mem::Ref<DriverTransport> transport,
      std::vector<mem::Ref<Portal>> waiting_portals);

  // Constructs a new NodeConnector on a non-broker node expecting a connection
  // from a broker node over `transport`.
  static mem::Ref<NodeConnector> ConnectNonBrokerToBroker(
      mem::Ref<Node> node,
      mem::Ref<DriverTransport> transport,
      std::vector<mem::Ref<Portal>> waiting_portals);

  // Constructs a new NodeConnector on a non-broker node expecting a connection
  // from another non-broker node over `transport`. The remote non-broker node
  // is expected to already have established a link to a broker, and they will
  // facilitate this connecting node's introduction to that broker.
  static mem::Ref<NodeConnector> ConnectAndInheritBroker(
      mem::Ref<Node> node,
      mem::Ref<DriverTransport> transport,
      std::vector<mem::Ref<Portal>> waiting_portals);

  // Constructs a new NodeConnector on a non-broker node expecting a connection
  // from another non-broker node over `transport`. The calling node most have
  // already established a broker link, and the remote node is expected to want
  // to inherit that same broker from us.
  static mem::Ref<NodeConnector> ConnectAndIntroduceBroker(
      mem::Ref<Node> node,
      mem::Ref<DriverTransport> transport,
      std::vector<mem::Ref<Portal>> waiting_portals);

  virtual void Connect() = 0;
  virtual bool OnMessage(uint8_t message_id,
                         const DriverTransport::Message& message) = 0;

 protected:
  NodeConnector(mem::Ref<Node> node,
                mem::Ref<DriverTransport> transport,
                std::vector<mem::Ref<Portal>> waiting_portals);
  ~NodeConnector() override;

  uint32_t num_portals() const {
    return static_cast<uint32_t>(waiting_portals_);
  }

  // Invoked once by the implementation when it has completed the handshake.
  // `new_link` has already assumed ownership of the underlying transport and
  // is listening for incoming messages on it. Destroys `this`.
  void AcceptConnection(mem::Ref<NodeLink> new_link,
                        LinkSide link_side,
                        uint32_t num_remote_portals);

  const mem::Ref<Node> node_;
  const mem::Ref<DriverTransport> transport_;

 private:
  void ActivateTransportAndConnect();
  void EstablishWaitingPortals(mem::Ref<NodeLink> to_link,
                               LinkSide link_side,
                               size_t max_valid_portals);

  // DriverTransport::Listener:
  IpczResult OnTransportMessage(
      const DriverTransport::Message& message) override;
  void OnTransportError() override;

  const std::vector<mem::Ref<Portal>> waiting_portals_;
  mem::Ref<NodeConnector> active_self_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NODE_CONNECTOR_H_
