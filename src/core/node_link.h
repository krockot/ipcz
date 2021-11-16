// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_NODE_LINK_H_
#define IPCZ_SRC_CORE_NODE_LINK_H_

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include "core/node.h"
#include "core/node_link_state.h"
#include "core/node_messages.h"
#include "core/node_name.h"
#include "core/routing_id.h"
#include "core/sequence_number.h"
#include "core/transport.h"
#include "mem/ref_counted.h"
#include "os/memory.h"
#include "os/process.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_map.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_set.h"
#include "third_party/abseil-cpp/absl/numeric/int128.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace ipcz {
namespace core {

class Node;
class Parcel;
class Portal;
class PortalLink;

// NodeLink provides both a client and service interface from one Node to
// another via a driver-operated Transport and potentially other internally
// negotiated media like shared memory queues.
//
// All messages defined by node_messages.h are implemented here and proxied to
// the local Node with any relevant context derived from the remote node's
// identity.
//
// NodeLink tracks outgoing requests and incoming replies where applicable,
// and deals with the boilerplate of serializing message structures, encoding
// and decoding handles, and forwarding replies (and rejections) to callers.
class NodeLink : public mem::RefCounted {
 public:
  // Constructs a new NodeLink for Node to talk to a remote node via `transport`
  // as the basic transport. If the caller has a handle to the remote process,
  // it should be passed in `remote_process`. Generally this is only necessary
  // for links initiated by the broker node or by nodes who wish to introduce
  // other new nodes (e.g. their own child processes) to the broker node.
  NodeLink(Node& node,
           mem::Ref<Transport> transport,
           os::Process remote_process,
           Node::Type remote_node_type);

  // Constructor used for introductions, where remote process is not a concern
  // and link state memory is already provided.
  NodeLink(Node& node,
           const NodeName& local_name,
           const NodeName& remote_name,
           mem::Ref<Transport> transport,
           os::Memory::Mapping link_state_mapping);

  const mem::Ref<Node>& node() const { return node_; }

  absl::optional<NodeName> GetRemoteName() {
    absl::MutexLock lock(&mutex_);
    return remote_name_;
  }

  // Invokes the the driver to activate I/O on the underlying transport for this
  // NodeLink. By the time this returns, it's possible for the driver to
  // re-enter the NodeLink at any time via OnMessageFromTransport.
  IpczResult Activate();

  // Invokes the driver to deactivate I/O on the underlying transport. Once this
  // is called, the transport cannot be re-activated.
  IpczResult Deactivate();

  // Generic entry point for all messages received from another node. While the
  // memory addressed by `message` is guaranteed to be safely addressable, it
  // may live in untrusted shared memory. No other validation is assumed by this
  // method.
  bool OnMessageFromTransport(const Transport::Message& message);
  IpczResult OnTransportError();

  mem::Ref<Portal> Invite(const NodeName& local_name,
                          const NodeName& remote_name,
                          absl::Span<IpczHandle> initial_portals);
  void AwaitInvitation(absl::Span<IpczHandle> initial_portals);

  using RequestIntroductionCallback =
      std::function<void(const mem::Ref<NodeLink>&)>;
  void RequestIntroduction(const NodeName& name,
                           RequestIntroductionCallback callback);

  // Sends a routed parcel to the remote node. This is different from the other
  // Send() operations above because the hacky message macro infrastructure
  // doesn't support variable-length messages; and in practice, parcel transfer
  // is one of relatively few use cases for them.
  void AcceptParcel(RoutingId routing_id, Parcel& parcel);

  // Notifies the remote node that the `side` has been closed on whatever route
  // contains a PortalLink bound to `routing_id` on this NodeLink.
  // `sequence_length` is the total number of parcels transmitted from that side
  // of the route before it closed.
  void SideClosed(RoutingId routing_id,
                  Side side,
                  SequenceNumber sequence_length);

  // Informs the portal associated with `routing_id` on the remote node that its
  // predecessor is now a half-proxy and that the predecessor's peer could
  // instead send parcels to the portal directly, rather than proxying through
  // the predecessor. The portal receiving this information will send the named
  // peer node an authenticated BypassProxy message to redirect the route around
  // the proxy and ultimately initiate its demise.
  void InitiateProxyBypass(RoutingId routing_id,
                           const NodeName& proxy_peer_name,
                           RoutingId proxy_peer_routing_id,
                           absl::uint128 bypass_key);

  // Requests that the remote node bypass the existing PortalLink identified by
  // `proxy_routing_id` on its link to the node named `proxy_name`, with a new
  // PortalLink directly to our local `portal` (which we know to be the
  // destination of the remote proxy link) instead. `bypass_key` is used to
  // authenticate the request and must already have been provided to the remote
  // node by our predecessor (the proxy in question).
  //
  // Returns a new PortalLink corresponding to the newly allocated routing ID.
  mem::Ref<PortalLink> BypassProxyToPortal(const NodeName& proxy_name,
                                           RoutingId proxy_routing_id,
                                           absl::uint128 bypass_key,
                                           mem::Ref<Portal> portal);

  // Tells the remote node that it can stop proxying incoming messages on
  // `routing_id` toward the `side` of the route as soon as it has forwarded any
  // in-flight messages up to (but not including) sequence number
  // `sequence_length` in that direction.
  void StopProxyingTowardSide(RoutingId routing_id,
                              Side side,
                              SequenceNumber sequence_length);

  void SetRemoteProtocolVersion(uint32_t version);

  // Sends a message which does not expect a reply.
  template <typename T, typename = std::enable_if_t<!T::kExpectsReply>>
  void Send(T& message) {
    message.Serialize();
    Send(message.params_view(), message.handles_view());
  }

  // Sends a message which expects a specific type of reply. If the remote node
  // indicates that they won't reply (for example, if they are on an older
  // version and don't understand the request), then `reply_handler` receives a
  // null value.
  template <typename T, typename = std::enable_if_t<T::kExpectsReply>>
  void Send(T& message, std::function<bool(typename T::Reply*)> reply_handler) {
    message.Serialize();
    SendWithReplyHandler(
        message.params_view(), message.handles_view(),
        [reply_handler](void* message) {
          return reply_handler(static_cast<typename T::Reply*>(message));
        });
  }

  // Generic implementations to support the above template helpers.
  using GenericReplyHandler = std::function<bool(void*)>;
  void Send(absl::Span<uint8_t> data, absl::Span<os::Handle> handles = {});
  void SendWithReplyHandler(absl::Span<uint8_t> data,
                            absl::Span<os::Handle> handles,
                            GenericReplyHandler reply_handler);

  // Allocates `count` contiguous RoutingIds on the link and returns the
  // smallest of them. For example if this is called with a `count` of 5, a
  // return value of 17 indicates that routing IDs 17, 18, 19, 20, and 21 have
  // been allocated by the caller.
  RoutingId AllocateRoutingIds(size_t count);

  void DisconnectRoutingId(RoutingId routing_id);

  mem::Ref<Portal> GetPortalForRoutingId(RoutingId id);

 private:
  ~NodeLink() override;

  bool AssignRoutingId(RoutingId id, const mem::Ref<Portal>& portal);

  // Generic entry point for message replies. Always dispatched to a
  // corresponding callback after some validation.
  bool OnReplyFromTransport(const Transport::Message& message);

  // Strongly typed message handlers, dispatched by the generic OnMessage()
  // above. If these methods are invoked, the message is at least superficially
  // well-formed (plausible header, sufficient data payload, sufficient
  // handles.)
  //
  // These message objects live in private memory and are safe from TOCTOU.
  //
  // Apart from checking for sufficient and reasonable data payload size, number
  // of handle slots, and presence of any required OS handles, no other
  // validation is done. Methods here must assume that field values can take on
  // any legal value for their underlying POD type.
  bool OnInviteNode(msg::InviteNode& m);
  bool OnSideClosed(msg::SideClosed& m);
  bool OnRequestIntroduction(msg::RequestIntroduction& m);
  bool OnInitiateProxyBypass(msg::InitiateProxyBypass& m);
  bool OnBypassProxy(msg::BypassProxy& m);
  bool OnStopProxyingTowardSide(msg::StopProxyingTowardSide& m);

  bool OnAcceptParcel(const Transport::Message& m);

  void IntroduceNode(const NodeName& name,
                     Transport::Descriptor& transport_descriptor,
                     os::Memory link_memory);
  bool OnIntroduceNode(const Transport::Message& m);

  struct PendingReply {
    PendingReply();
    PendingReply(uint8_t message_id, GenericReplyHandler handler);
    PendingReply(const PendingReply&) = delete;
    PendingReply& operator=(const PendingReply&) = delete;
    PendingReply(PendingReply&&);
    PendingReply& operator=(PendingReply&&);
    ~PendingReply();
    uint8_t message_id;
    GenericReplyHandler handler;
  };

  const mem::Ref<Node> node_;
  const Node::Type remote_node_type_;
  const mem::Ref<Transport> transport_;
  std::atomic<uint16_t> next_request_id_{1};

  absl::Mutex mutex_;
  NodeName local_name_ ABSL_GUARDED_BY(mutex_);
  NodeName remote_name_ ABSL_GUARDED_BY(mutex_);
  absl::optional<uint32_t> remote_protocol_version_ ABSL_GUARDED_BY(mutex_);
  os::Process remote_process_ ABSL_GUARDED_BY(mutex_);
  absl::flat_hash_map<uint16_t, PendingReply> pending_replies_
      ABSL_GUARDED_BY(mutex_);
  absl::flat_hash_map<RoutingId, mem::Ref<Portal>> routes_
      ABSL_GUARDED_BY(mutex_);
  std::vector<mem::Ref<Portal>> portals_awaiting_invitation_
      ABSL_GUARDED_BY(mutex_);
  absl::flat_hash_map<NodeName, std::vector<RequestIntroductionCallback>>
      pending_introductions_ ABSL_GUARDED_BY(mutex_);
  os::Memory::Mapping link_state_mapping_ ABSL_GUARDED_BY(mutex_);
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NODE_LINK_H_
