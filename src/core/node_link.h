// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_NODE_LINK_H_
#define IPCZ_SRC_CORE_NODE_LINK_H_

#include <cstddef>
#include <cstdint>

#include "core/driver_transport.h"
#include "core/node.h"
#include "core/node_messages.h"
#include "core/node_name.h"
#include "core/remote_router_link.h"
#include "core/routing_id.h"
#include "core/sequence_number.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "os/memory.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_map.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

class Node;
struct NodeLinkBuffer;
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
           os::Memory::Mapping link_memory);

  const mem::Ref<Node>& node() const { return node_; }
  const NodeName& local_node_name() const { return local_node_name_; }
  const NodeName& remote_node_name() const { return remote_node_name_; }
  Node::Type remote_node_type() const { return remote_node_type_; }
  uint32_t remote_protocol_version() const { return remote_protocol_version_; }
  const mem::Ref<DriverTransport>& transport() const { return transport_; }
  NodeLinkBuffer& buffer() const { return *link_memory_.As<NodeLinkBuffer>(); }

  // Allocates a routing ID from the NodeLink's shared state. This atomically
  // increments the shared routing ID counter shared by both ends of the link by
  // `count` and returns the value it had just before incrementing. The caller
  // can assume succesful allocation of routing IDs from the returned value N
  // (inclusive) up to `N + count` (exclusive).
  RoutingId AllocateRoutingIds(size_t count);

  // Binds `routing_id` on this NodeLink to the given `router`. `link_side`
  // specifies which side of the link this end identifies as (A or B), and
  // `type` specifies the type of link this is from the receiving router's
  // perspective.
  mem::Ref<RemoteRouterLink> AddRoute(RoutingId routing_id,
                                      size_t link_state_index,
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

  // Asks this link (which must be linked to a remote broker node) to introduce
  // us to the named node. This will always elicit an eventual IntroduceNode
  // message from the broker, even if only to indicate failure.
  void RequestIntroduction(const NodeName& name);

  // Introduces the remote node to the node named `name`, giving it a new
  // DriverTransport it can use to communicate with that node.
  void IntroduceNode(const NodeName& name,
                     mem::Ref<DriverTransport> transport,
                     os::Memory link_buffer_memory);

  // Sends a request to the remote node to have one of its routers reconfigured
  // with a new peer link. Specifically, whatever router has a peer
  // RemoteRouterLink to `proxy_peer_node_name` on `proxy_peer_routing_id` is
  // reconfigured to instead adopt a new RemoteRouterLink as its peer, on this
  // NodeLink with a newly allocated routing ID.  `new_peer` becomes the new
  // peer of the reconfigured remote router, assuming it's able to authenticate
  // the source of this request.
  bool BypassProxy(const NodeName& proxy_name,
                   RoutingId proxy_routing_id,
                   SequenceNumber sequence_length_to_proxy,
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
  bool OnAcceptParcel(const DriverTransport::Message& message);
  bool OnRouteClosed(const msg::RouteClosed& route_closed);
  bool OnIntroduceNode(const DriverTransport::Message& message);
  bool OnStopProxying(const msg::StopProxying& stop);
  bool OnInitiateProxyBypass(const msg::InitiateProxyBypass& request);
  bool OnBypassProxyToSameNode(const msg::BypassProxyToSameNode& bypass);
  bool OnStopProxyingToLocalPeer(const msg::StopProxyingToLocalPeer& stop);
  bool OnProxyWillStop(const msg::ProxyWillStop& will_stop);
  bool OnDecayUnblocked(const msg::DecayUnblocked& unblocked);
  bool OnLogRouteTrace(const msg::LogRouteTrace& log_request);

  const mem::Ref<Node> node_;
  const NodeName local_node_name_;
  const NodeName remote_node_name_;
  const Node::Type remote_node_type_;
  const uint32_t remote_protocol_version_;
  const mem::Ref<DriverTransport> transport_;
  const os::Memory::Mapping link_memory_;

  absl::Mutex mutex_;
  bool active_ = true;
  absl::flat_hash_map<RoutingId, Route> routes_ ABSL_GUARDED_BY(mutex_);
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NODE_LINK_H_
