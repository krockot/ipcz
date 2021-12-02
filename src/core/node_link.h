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
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "os/memory.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_map.h"
#include "third_party/abseil-cpp/absl/numeric/int128.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

class Node;
struct NodeLinkBuffer;
class Router;
class RouterLink;

class NodeLink : public mem::RefCounted, private DriverTransport::Listener {
 public:
  NodeLink(mem::Ref<Node> node,
           const NodeName& remote_node_name,
           Node::Type remote_node_type,
           uint32_t remote_protocol_version,
           mem::Ref<DriverTransport> transport,
           os::Memory::Mapping link_memory);

  const mem::Ref<Node>& node() const { return node_; }
  const NodeName& remote_node_name() const { return remote_node_name_; }
  Node::Type remote_node_type() const { return remote_node_type_; }
  uint32_t remote_protocol_version() const { return remote_protocol_version_; }
  const mem::Ref<DriverTransport>& transport() const { return transport_; }
  NodeLinkBuffer& buffer() const { return *link_memory_.As<NodeLinkBuffer>(); }

  RoutingId AllocateRoutingIds(size_t count);

  mem::Ref<RouterLink> AddRoute(RoutingId routing_id,
                                size_t link_state_index,
                                mem::Ref<Router> router,
                                RemoteRouterLink::Type link_type);
  bool RemoveRoute(RoutingId routing_id);
  mem::Ref<Router> GetRouter(RoutingId routing_id);

  void Deactivate();

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
  // NodeLink with a newly allocated routing ID. `bypass_key` is used to
  // authenticate the request and must have already been shared with the remote
  // node by the proxy itself (the node identified by `proxy_peer_node_name`).
  // Upon success, `new_peer` is updated to use the reconfigured remote router
  // as its peer and (asynchronously) `new_peer` becomes the new peer of the
  // reconfigured remote router.
  bool BypassProxy(const NodeName& proxy_name,
                   RoutingId proxy_routing_id,
                   absl::uint128 bypass_key,
                   mem::Ref<Router> new_peer);

 private:
  ~NodeLink() override;

  // DriverTransport::Listener:
  IpczResult OnTransportMessage(
      const DriverTransport::Message& message) override;
  void OnTransportError() override;

  bool OnAcceptParcel(const DriverTransport::Message& message);
  bool OnSideClosed(const msg::SideClosed& side_closed);
  bool OnIntroduceNode(const DriverTransport::Message& message);
  bool OnStopProxying(const msg::StopProxying& stop);
  bool OnInitiateProxyBypass(const msg::InitiateProxyBypass& request);
  bool OnBypassProxyToSameNode(const msg::BypassProxyToSameNode& bypass);
  bool OnStopProxyingToLocalPeer(const msg::StopProxyingToLocalPeer& stop);

  const mem::Ref<Node> node_;
  const NodeName remote_node_name_;
  const Node::Type remote_node_type_;
  const uint32_t remote_protocol_version_;
  const mem::Ref<DriverTransport> transport_;
  const os::Memory::Mapping link_memory_;

  absl::Mutex mutex_;
  bool active_ = true;
  absl::flat_hash_map<RoutingId, mem::Ref<Router>> routes_
      ABSL_GUARDED_BY(mutex_);
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NODE_LINK_H_
