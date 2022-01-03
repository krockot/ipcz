// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_NODE_LINK_H_
#define IPCZ_SRC_CORE_NODE_LINK_H_

#include <cstddef>
#include <cstdint>
#include <functional>

#include "core/buffer_id.h"
#include "core/driver_transport.h"
#include "core/node.h"
#include "core/node_link_address.h"
#include "core/node_link_memory.h"
#include "core/node_messages.h"
#include "core/node_name.h"
#include "core/remote_router_link.h"
#include "core/routing_id.h"
#include "core/sequence_number.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "os/memory.h"
#include "os/process.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_map.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

class Node;
class RemoteRouterLink;
class Router;
class RouterLink;

class NodeLink : public mem::RefCounted, private DriverTransport::Listener {
 public:
  struct Route {
    Route(mem::Ref<RemoteRouterLink> link, mem::Ref<Router> receiver);
    Route(Route&&);
    Route(const Route&);
    Route& operator=(Route&&);
    Route& operator=(const Route&);
    ~Route();

    mem::Ref<RemoteRouterLink> link;
    mem::Ref<Router> receiver;
  };

  NodeLink(mem::Ref<Node> node,
           const NodeName& local_node_name,
           const NodeName& remote_node_name,
           Node::Type remote_node_type,
           uint32_t remote_protocol_version,
           mem::Ref<DriverTransport> transport,
           mem::Ref<NodeLinkMemory> memory);

  const mem::Ref<Node>& node() const { return node_; }
  const NodeName& local_node_name() const { return local_node_name_; }
  const NodeName& remote_node_name() const { return remote_node_name_; }
  Node::Type remote_node_type() const { return remote_node_type_; }
  uint32_t remote_protocol_version() const { return remote_protocol_version_; }
  const mem::Ref<DriverTransport>& transport() const { return transport_; }

  NodeLinkMemory& memory() { return *memory_; }
  const NodeLinkMemory& memory() const { return *memory_; }

  // Allocates a routing ID from the NodeLink's shared state. This atomically
  // increments the shared routing ID counter shared by both ends of the link by
  // `count` and returns the value it had just before incrementing. The caller
  // can assume successful allocation of routing IDs from the returned value N
  // (inclusive) up to `N + count` (exclusive).
  RoutingId AllocateRoutingIds(size_t count);

  // Retrieves the address of the RouterLinkState reserved by the i'th initial
  // portal on this NodeLink.
  NodeLinkAddress GetInitialRouterLinkState(size_t i);

  // Allocates a new RouterLinkState from the NodeLink's shared state.
  NodeLinkAddress AllocateRouterLinkState();

  // Binds `routing_id` on this NodeLink to the given `router`. `link_side`
  // specifies which side of the link this end identifies as (A or B), and
  // `type` specifies the type of link this is from the receiving router's
  // perspective.
  //
  // If `link_state_address` is non-null, the given address identifies the
  // location of the shared RouterLinkState structure for the new route. Only
  // central links require a RouterLinkState.
  mem::Ref<RemoteRouterLink> AddRoute(
      RoutingId routing_id,
      absl::optional<NodeLinkAddress> link_state_address,
      LinkType type,
      LinkSide side,
      mem::Ref<Router> router);

  // Removes the route specified by `routing_id`. Once removed, any messages
  // received for that routing ID are ignored.
  bool RemoveRoute(RoutingId routing_id);

  // Retrieves the Router and RemoteRouterLink currently bound to `routing_id`
  // on this NodeLink.
  absl::optional<Route> GetRoute(RoutingId routing_id);

  // Retrieves only the Router currently bound to `routing_id` on this NodeLink.
  mem::Ref<Router> GetRouter(RoutingId routing_id);

  // Permanently deactivates this NodeLink. Once this call returns the NodeLink
  // will no longer receive transport messages. It may still be used to transmit
  // outgoing messages, but it cannot be reactivated.
  void Deactivate();

  // Transmits a transport message to the other end of this NodeLink.
  void Transmit(absl::Span<const uint8_t> data, absl::Span<os::Handle> handles);

  template <typename T>
  void Transmit(T& message) {
    transport_->Transmit(message);
  }

  // Asks the broker on the other end of this link to accept a new node for
  // introduction into the network. Normally nodes connect directly to a broker,
  // but in some cases a non-broker may connect directly another non-broker to
  // request inheritance of its broker in order to join the same network. This
  // message is sent to the broker by an established non-broker on behalf of the
  // new non-broker attempting to join the network.
  using IndirectBrokerConnectionCallback =
      std::function<void(const NodeName&, uint32_t num_remote_portals)>;
  void RequestIndirectBrokerConnection(
      mem::Ref<DriverTransport> transport,
      os::Process new_node_process,
      size_t num_initial_portals,
      IndirectBrokerConnectionCallback callback);

  // Asks the broker on the other end of this link to introduce the local node
  // on this side of the link to the named node. This will always elicit an
  // eventual IntroduceNode message from the broker, even if only to indicate
  // failure.
  void RequestIntroduction(const NodeName& name);

  // Introduces the remote node to the node named `name`, giving it a new
  // DriverTransport it can use to communicate with that node.
  void IntroduceNode(const NodeName& name,
                     mem::Ref<DriverTransport> transport,
                     os::Memory primary_buffer);

  // Sends a request to the remote node to have one of its routers reconfigured
  // with a new peer link. Specifically, whatever router has a peer
  // RemoteRouterLink to `proxy_peer_node_name` on `proxy_peer_routing_id` is
  // reconfigured to instead adopt a new RemoteRouterLink as its peer, on this
  // NodeLink with a newly allocated routing ID.  `new_peer` becomes the new
  // peer of the reconfigured remote router, assuming it's able to authenticate
  // the source of this request.
  bool BypassProxy(const NodeName& proxy_name,
                   RoutingId proxy_routing_id,
                   SequenceNumber proxy_outbound_sequence_length,
                   mem::Ref<Router> new_peer);

 private:
  ~NodeLink() override;

  // DriverTransport::Listener:
  IpczResult OnTransportMessage(
      const DriverTransport::Message& message) override;
  void OnTransportError() override;

  // All of these methods correspond directly to remote calls from another node,
  // either through NodeLink (for OnIntroduceNode) or via RemoteRouterLink (for
  // everything else).
  bool OnRequestIndirectBrokerConnection(
      const DriverTransport::Message& message);
  bool OnAcceptIndirectBrokerConnection(
      const msg::AcceptIndirectBrokerConnection& accept);
  bool OnAcceptParcel(const DriverTransport::Message& message);
  bool OnRouteClosed(const msg::RouteClosed& route_closed);
  bool OnIntroduceNode(const DriverTransport::Message& message);
  bool OnStopProxying(const msg::StopProxying& stop);
  bool OnInitiateProxyBypass(const msg::InitiateProxyBypass& request);
  bool OnBypassProxyToSameNode(const msg::BypassProxyToSameNode& bypass);
  bool OnStopProxyingToLocalPeer(const msg::StopProxyingToLocalPeer& stop);
  bool OnProxyWillStop(const msg::ProxyWillStop& will_stop);
  bool OnNotifyBypassPossible(const msg::NotifyBypassPossible& notify);
  bool OnLogRouteTrace(const msg::LogRouteTrace& log_request);

  const mem::Ref<Node> node_;
  const NodeName local_node_name_;
  const NodeName remote_node_name_;
  const Node::Type remote_node_type_;
  const uint32_t remote_protocol_version_;
  const mem::Ref<DriverTransport> transport_;
  const mem::Ref<NodeLinkMemory> memory_;

  absl::Mutex mutex_;
  bool active_ = true;

  using RouteMap = absl::flat_hash_map<RoutingId, Route>;
  RouteMap routes_ ABSL_GUARDED_BY(mutex_);

  uint64_t next_request_id_ ABSL_GUARDED_BY(mutex_) = 0;
  using IndirectBrokerConnectionMap =
      absl::flat_hash_map<uint64_t, IndirectBrokerConnectionCallback>;
  IndirectBrokerConnectionMap pending_indirect_broker_connections_
      ABSL_GUARDED_BY(mutex_);
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NODE_LINK_H_
