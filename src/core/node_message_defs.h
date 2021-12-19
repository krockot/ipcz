// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// no-include-guard-because-multiply-included

// This file defines the internal messages which can be sent on a NodeLink
// between two ipcz nodes.

// This needs to be incremented any time changes are made to these definitions.
IPCZ_PROTOCOL_VERSION(0)

// Initial greeting sent by a broker node to a non-broker node via the
// ConnectNode() API.
IPCZ_MSG_NO_REPLY(ConnectFromBroker, IPCZ_MSG_ID(0), IPCZ_MSG_VERSION(0))
  // The name of the broker node.
  IPCZ_MSG_PARAM(NodeName, broker_name)

  // The name of the receiving node, assigned randomly by the broker.
  IPCZ_MSG_PARAM(NodeName, receiver_name)

  // The highest protocol version known and desired by the broker.
  IPCZ_MSG_PARAM(uint32_t, protocol_version)

  // The number of initial portals assumed on the broker's end of the
  // connection. If there is a mismatch between the number sent by each node on
  // an initial connection, the node which sent the larger number should behave
  // as if its excess portals have observed peer closure.
  IPCZ_MSG_PARAM(uint32_t, num_initial_portals)

  // A handle to an initial shared memory buffer which can be used to allocate
  // shared state between the two connecting nodes.
  IPCZ_MSG_HANDLE_REQUIRED(link_state_memory)
IPCZ_MSG_END()

// Initial greeting sent by a non-broker node to a broker node via the
// ConnectNode() API.
IPCZ_MSG_NO_REPLY(ConnectToBroker, IPCZ_MSG_ID(1), IPCZ_MSG_VERSION(0))
  // The highest protocol version known and desired by the sender.
  IPCZ_MSG_PARAM(uint32_t, protocol_version)

  // The number of initial portals assumed on the sender's end of the
  // connection. If there is a mismatch between the number sent by each node on
  // an initial connection, the node which sent the larger number should behave
  // as if its excess portals have observed peer closure.
  IPCZ_MSG_PARAM(uint32_t, num_initial_portals)
IPCZ_MSG_END()

// Sent by a non-broker to another non-broker to request that the recipient
// introduce the sender to its own broker. It will do so by sending a
// RequestBrokerIntroduction message to its own broker, and once it receives a
// reply to that message it will send a ConnectAndIntroduceBroker message to the
// original sender of this message.
IPCZ_MSG_NO_REPLY(ConnectAndInheritBroker, IPCZ_MSG_ID(2), IPCZ_MSG_VERSION(0))
  // The highest protocol version known and desired by the sender.
  IPCZ_MSG_PARAM(uint32_t, protocol_version)

  // The number of initial portals assumed on the sender's end of the
  // connection. If there is a mismatch between the number sent by each node on
  // an initial connection, the node which sent the larger number should behave
  // as if its excess portals have observed peer closure.
  IPCZ_MSG_PARAM(uint32_t, num_initial_portals)
IPCZ_MSG_END()

// Sent by a non-broker to its already-established broker when attempting to
// establish a new broker connection on behalf of another non-broker. The broker
// will allocate a new transport and reply with its serialized representation,
// along with a new assigned name for the other non-broker. The requester should
// forwrad that information back to the original requesting node via a
// ConnectAndIntroduceBroker message.
IPCZ_MSG_NO_REPLY(RequestBrokerIntroduction, IPCZ_MSG_ID(3),
                  IPCZ_MSG_VERSION(0))
  // An ID for this introduction request, unique in the scope of the
  // transmitting NodeLink. A resulting IntroduceBrokerIndirect message is sent
  // with the same ID to correlate that message with an instance of this one.
  IPCZ_MSG_PARAM(uint64_t, request_id)

  // The highest protocol version known and desired by the node on whose behalf
  // this introduction is being requested.
  IPCZ_MSG_PARAM(uint32_t, new_node_protocol_version)

  // A handle to the the process hosting the new node to be introduced. This is
  // optional but may be necessary to specify on some platforms (namely Windows)
  // in order for OS handle transfer to work properly to and from the new node.
  IPCZ_MSG_HANDLE_OPTIONAL(new_node_process)
IPCZ_MSG_END()

// Sent by a non-broker node to a broker node. Requests that the broker provide
// a new NodeLink to both the sender and the node identified by `name`, linking
// the two nodes together and allowing them to communicate directly. This
// message has no reply.
//
// If the broker does not know the node named `name`, it will send an
// IntroduceNode message back to the sender with empty handles, indicating
// failure. Otherwise it will send an IntroduceNode message to both the sender
// and the node identified by `name`, with complementary transport descriptors
// attached to each, and an os::Memory handle in `link_state_memory` which each
// side can use to map a shared (zero-initialized) NodeLinkState.
IPCZ_MSG_NO_REPLY(RequestIntroduction, IPCZ_MSG_ID(5), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(NodeName, name)
IPCZ_MSG_END()

// Notifies a node that the route has been closed on one side. If this arrives
// at a router from an inward facing or bridge link, it implicitly pertains to
// the router's own side of the route. Otherwise it indicates that the other
// side of the route has been closed. In either case, `sequence_length` is the
// total number of parcels transmitted from the closed side before closing.
IPCZ_MSG_NO_REPLY(RouteClosed, IPCZ_MSG_ID(11), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(RoutingId, routing_id)
  IPCZ_MSG_PARAM(SequenceNumber, sequence_length)
IPCZ_MSG_END()

// Informs the recipient that its outward peer is a proxy which has negotiated
// its own turn to decay and be bypassed along the route. This provides the
// recipient with enough information to contact the proxy's own outward peer
// directly in order to request that its link to the proxy be bypassed with a
// direct link to the recipient.
IPCZ_MSG_NO_REPLY(InitiateProxyBypass, IPCZ_MSG_ID(12), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(RoutingId, routing_id)
  IPCZ_MSG_PARAM(uint64_t, reserved0)
  IPCZ_MSG_PARAM(NodeName, proxy_peer_name)
  IPCZ_MSG_PARAM(RoutingId, proxy_peer_routing_id)
IPCZ_MSG_END()

// Simultaneously informs the recipient that its outward link is connected to a
// proxying router, and requests that this link be replaced immediately
// with a more direct link to the proxy's inward peer, who is the sender of this
// message.
//
// By the time this is message is sent from a proxy's inward peer to the proxy's
// outward peer, the proxy must have already informed the outward peer of its
// inward peer's NodeName. The outward peer will then be able to reliably
// authenticate the source of this BypassProxy request.
IPCZ_MSG_NO_REPLY(BypassProxy, IPCZ_MSG_ID(13), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(NodeName, proxy_name)
  IPCZ_MSG_PARAM(RoutingId, proxy_routing_id)
  IPCZ_MSG_PARAM(RoutingId, new_routing_id)
  IPCZ_MSG_PARAM(SequenceNumber, proxy_outbound_sequence_length)
IPCZ_MSG_END()

// Informs the recipient that the portal on `routing_id` for this NodeLink can
// cease to exist once it has received and forwarded parcels up to the
// specified sequence length in each direction.
IPCZ_MSG_NO_REPLY(StopProxying, IPCZ_MSG_ID(14), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(RoutingId, routing_id)
  IPCZ_MSG_PARAM(SequenceNumber, proxy_inbound_sequence_length)
  IPCZ_MSG_PARAM(SequenceNumber, proxy_outbound_sequence_length)
IPCZ_MSG_END()

// Informs the recipient that its decaying outward link (implicitly going to a
// decaying proxy) will only receive inbound parcels up to but not including the
// given `sequence_length`.
IPCZ_MSG_NO_REPLY(ProxyWillStop, IPCZ_MSG_ID(15), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(RoutingId, routing_id)
  IPCZ_MSG_PARAM(SequenceNumber, proxy_inbound_sequence_length)
IPCZ_MSG_END()

// Equivalent to BypassProxy, but used only when the proxy and its outward peer
// live on the same node. In this case there's no need for the proxy's inward
// peer to authenticate any request to the outward peer, because the outward
// peer is made aware of made aware of the bypass operation as soon as the
// proxy initiates it.
//
// When the proxy's inward peer receives this message, it may immediately begin
// decaying its link on `routing_id` to the proxy, replacing it with a new link
// link (to the same node) on `new_routing_id` to the proxy's outward peer.
//
// `sequence_length` indicates the length of the sequence already sent or
// in-flight over `routing_id` from the proxy, and the recipient of this message
// must send a StopProxyingToLocalPeer back to the proxy with the length its own
// outbound sequence had at the time it began decaying the link on `routing_id`.
IPCZ_MSG_NO_REPLY(BypassProxyToSameNode, IPCZ_MSG_ID(16), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(RoutingId, routing_id)
  IPCZ_MSG_PARAM(RoutingId, new_routing_id)
  IPCZ_MSG_PARAM(SequenceNumber, proxy_inbound_sequence_length)
IPCZ_MSG_END()

// Informs the recipient that it has been bypassed by the sender in favor of a
// direct route to the sender's local outward peer. This is essentially a reply
// to BypassProxyToSameNode.
IPCZ_MSG_NO_REPLY(StopProxyingToLocalPeer, IPCZ_MSG_ID(17), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(RoutingId, routing_id)
  IPCZ_MSG_PARAM(SequenceNumber, proxy_outbound_sequence_length)
IPCZ_MSG_END()

// Notifies the target router that bypass of its outward link may be possible.
// This may be sent to catalyze route reduction in some cases where the router
// in question could otherwise fail indefinitely to notice a bypass opportunity.
IPCZ_MSG_NO_REPLY(NotifyBypassPossible, IPCZ_MSG_ID(18), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(RoutingId, routing_id)
IPCZ_MSG_END()

// Requests that the receiving Router log a description of itself and forward
// this request along the same direction in which it was received.
IPCZ_MSG_NO_REPLY(LogRouteTrace, IPCZ_MSG_ID(192), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(RoutingId, routing_id)
IPCZ_MSG_END()
