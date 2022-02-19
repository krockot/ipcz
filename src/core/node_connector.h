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
#include "os/process.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/function.h"

namespace ipcz {
namespace core {

class Node;
class NodeLink;
class Portal;

// A NodeConnector activates and temporarily attaches itself to a
// DriverTransport to listen for and transmit introductory messages between two
// nodes invoking ConnectNode() on opposite ends of the same transport pair.
//
// Once an initial handshake is complete the underlying transport is adopted by
// a new NodeLink and handed off to the local Node to communicate with the
// remote node, and this object is destroyed.
class NodeConnector : public mem::RefCounted, public DriverTransport::Listener {
 public:
  // Constructs a new NodeConnector to send and receive a handshake on
  // `transport`. The specific type of connector to create is determined by a
  // combination of the Node::Type of `node` and the value of `flags`.
  //
  // If a connection is successfully established, `transport` will eventually
  // be adopted by a NodeLink and passed to `node` for use. Otherwise, all
  // `initial_portals` will observe peer closure.
  //
  // In either case this object invokes `callback` if non-null and then destroys
  // itself once the handshake is complete. If this fails, the NodeLink given
  // to the callback will be null.
  using ConnectCallback = Function<void(mem::Ref<NodeLink> new_link)>;
  static IpczResult ConnectNode(
      mem::Ref<Node> node,
      mem::Ref<DriverTransport> transport,
      os::Process remote_process,
      IpczCreateNodeFlags flags,
      const std::vector<mem::Ref<Portal>>& initial_portals,
      ConnectCallback callback = nullptr);

  // Constructs a new NodeConnector to send and receive a handshake on
  // `transport`, which was passed to this (broker) node by one non-broker
  // referring another non-broker to the network. On the other side of this
  // transport must be the latter non-broker, issuing its own ConnectNode() call
  //  with the IPCZ_CONNECT_NODE_INHERIT_BROKER flag specified.
  using ConnectIndirectCallback =
      Function<void(mem::Ref<NodeLink> new_link, uint32_t num_remote_portals)>;
  static IpczResult ConnectNodeIndirect(mem::Ref<Node> node,
                                        mem::Ref<NodeLink> referrer,
                                        mem::Ref<DriverTransport> transport,
                                        os::Process remote_process,
                                        uint32_t num_initial_portals,
                                        ConnectIndirectCallback callback);

  virtual void Connect() = 0;
  virtual bool OnMessage(uint8_t message_id,
                         const DriverTransport::Message& message) = 0;

 protected:
  NodeConnector(mem::Ref<Node> node,
                mem::Ref<DriverTransport> transport,
                IpczCreateNodeFlags flags,
                std::vector<mem::Ref<Portal>> waiting_portals,
                ConnectCallback callback);
  ~NodeConnector() override;

  uint32_t num_portals() const {
    return static_cast<uint32_t>(waiting_portals_.size());
  }

  // Invoked once by the implementation when it has completed the handshake.
  // `new_link` has already assumed ownership of the underlying transport and
  // is listening for incoming messages on it. Destroys `this`.
  void AcceptConnection(mem::Ref<NodeLink> new_link,
                        LinkSide link_side,
                        uint32_t num_remote_portals);
  void RejectConnection();

  const mem::Ref<Node> node_;
  const mem::Ref<DriverTransport> transport_;
  const IpczCreateNodeFlags flags_;
  const std::vector<mem::Ref<Portal>> waiting_portals_;
  mem::Ref<NodeConnector> active_self_;

 private:
  void ActivateTransportAndConnect();
  void EstablishWaitingPortals(mem::Ref<NodeLink> to_link,
                               LinkSide link_side,
                               size_t max_valid_portals);

  // DriverTransport::Listener:
  IpczResult OnTransportMessage(
      const DriverTransport::Message& message) override;
  void OnTransportError() override;

  const ConnectCallback callback_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NODE_CONNECTOR_H_
