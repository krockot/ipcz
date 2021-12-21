// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// no-include-guard-because-multiply-included

// This file defines the internal messages which can be sent on a NodeLink
// between two ipcz nodes.

// This needs to be incremented any time changes are made to these definitions.
IPCZ_PROTOCOL_VERSION(0)

// Initial greeting sent by a broker node when a ConnectNode() is issued without
// the IPCZ_CONNECT_NODE_TO_BROKER flag, implying that the remote node is a
// non-broker.
IPCZ_MSG_NO_REPLY(ConnectFromBrokerToNonBroker,
                  IPCZ_MSG_ID(0),
                  IPCZ_MSG_VERSION(0))
  // The name of the broker node.
  IPCZ_MSG_PARAM(NodeName, broker_name)

  // The name of the receiving non-broker node, assigned randomly by the broker.
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
  IPCZ_MSG_HANDLE_REQUIRED(initial_link_buffer_memory)
IPCZ_MSG_END()

// Initial greeting sent by a non-broker node when ConnectNode() is invoked with
// IPCZ_CONNECT_NODE_TO_BROKER. The sending non-broker node expects to receive a
// corresponding ConnectFromBrokerToNonBroker
IPCZ_MSG_NO_REPLY(ConnectFromNonBrokerToBroker,
                  IPCZ_MSG_ID(1),
                  IPCZ_MSG_VERSION(0))
  // The highest protocol version known and desired by the sender.
  IPCZ_MSG_PARAM(uint32_t, protocol_version)

  // The number of initial portals assumed on the sender's end of the
  // connection. If there is a mismatch between the number sent by each node on
  // an initial connection, the node which sent the larger number should behave
  // as if its excess portals have observed peer closure.
  IPCZ_MSG_PARAM(uint32_t, num_initial_portals)
IPCZ_MSG_END()

// Sent by a non-broker to a broker, indirectly via a call to ConnectNode() with
// IPCZ_CONNECT_NODE_INHERIT_BROKER. The other end of the transport in that case
// is expected to start on another non-broker node issuing a ConnectNode() call
// with IPCZ_CONNECT_NODE_SHARE_BROKER. In that case, the latter non-broker
// passes the transport to its broker, who will actually receive this message.
// The broker sends a corresponding ConnectFromBrokerIndirect.
IPCZ_MSG_NO_REPLY(ConnectToBrokerIndirect, IPCZ_MSG_ID(2), IPCZ_MSG_VERSION(0))
  // The highest protocol version known and desired by the sender.
  IPCZ_MSG_PARAM(uint32_t, protocol_version)

  // The number of initial portals assumed on the sender's end of the
  // connection. If there is a mismatch between the number sent by each node on
  // an initial connection, the node which sent the larger number should behave
  // as if its excess portals have observed peer closure.
  IPCZ_MSG_PARAM(uint32_t, num_initial_portals)
IPCZ_MSG_END()

// Sent by a broker to a non-broker who initiated contact by connecting to a
// intermediary non-broker with IPCZ_CONNECT_NODE_INHERIT_BROKER. This is
// similar to ConnectFromBrokerToNonBroker, but it also specifies the name of
// the non-broker node which facilitated the connection. In addition to
// establishing a broker link for the recipient, this informs the recipient that
// it should expect an introduction to the other node immediately after
// receiving this message.
IPCZ_MSG_NO_REPLY(ConnectFromBrokerIndirect,
                  IPCZ_MSG_ID(3),
                  IPCZ_MSG_VERSION(0))
  // The name of the broker node.
  IPCZ_MSG_PARAM(NodeName, broker_name)

  // The name of the receiving node, assigned randomly by the broker.
  IPCZ_MSG_PARAM(NodeName, receiver_name)

  // The name of the other non-broker node to which the receiver of this message
  // originally connected with IPCZ_CONNECT_NODE_INHERIT_BROKER. Both nodes will
  // receive an introduction to each other immediately after this message is
  // sent.
  IPCZ_MSG_PARAM(NodeName, connected_node_name)

  // The highest protocol version known and desired by the broker.
  IPCZ_MSG_PARAM(uint32_t, protocol_version)

  // The number of initial portals specified on the other end of this
  // connection, i.e. on the node named by `connected_node_name` and
  // specifically on the impending link between it and the recipient of this
  // message.
  IPCZ_MSG_PARAM(uint32_t, num_remote_portals)

  // A handle to an initial shared memory buffer which can be used to allocate
  // shared state between the broker and the recipient.
  IPCZ_MSG_HANDLE_REQUIRED(initial_link_buffer_memory)
IPCZ_MSG_END()

// Message sent from a broker to another broker, to establish a link between
// them and their respective node networks.
IPCZ_MSG_NO_REPLY(ConnectFromBrokerToBroker,
                  IPCZ_MSG_ID(4),
                  IPCZ_MSG_VERSION(0))
  // The name of the sending broker node.
  IPCZ_MSG_PARAM(NodeName, sender_name)

  // The highest protocol version known and desired by the sender.
  IPCZ_MSG_PARAM(uint32_t, protocol_version)

  // The number of initial portals assumed on the sender's end of the
  // connection. If there is a mismatch between the number sent by each node on
  // an initial connection, the node which sent the larger number should behave
  // as if its excess portals have observed peer closure.
  IPCZ_MSG_PARAM(uint32_t, num_initial_portals)

  // A handle to an initial shared memory buffer which can be used to allocate
  // shared state between the two connecting nodes. As both nodes are expected
  // to provide such a buffer, the one from the broker with the numericlly
  // lesser NodeName is adopted as the primary link buffer (buffer ID 0). The
  // other buffer is adopted by convention as the first secondary link buffer,
  // with a buffer ID of 1.
  IPCZ_MSG_HANDLE_REQUIRED(initial_link_buffer_memory)
IPCZ_MSG_END()

// Message ID 3 is used by RequestIndirectBrokerConnection in node_messages.h.

// A reply to RequestIndirectBrokerConnection. If `success` is true, this
// message will be followed immediately by an IntroduceNode message to introduce
// the recipient of this message to the newly connected node. See
// RequestIndirectBrokerConnection for a more detailed explanation.
IPCZ_MSG_NO_REPLY(AcceptIndirectBrokerConnection,
                  IPCZ_MSG_ID(11),
                  IPCZ_MSG_VERSION(0))
  // An ID corresponding to a RequestIndirectBrokerConnection message sent
  // previously by the target of this message.
  IPCZ_MSG_PARAM(uint64_t, request_id)

  // Indicates whether the broker agreed to complete the requested connection.
  // If true,
  IPCZ_MSG_PARAM(bool, success)

  // Padding to ensure the NodeName below is 16-byte aligned.
  IPCZ_MSG_PARAM(uint8_t, reserved0)
  IPCZ_MSG_PARAM(uint16_t, reserved1)

  // The number of initial portals requested by the new non-broker node who
  // requested this indirect connection.
  IPCZ_MSG_PARAM(uint32_t, num_remote_portals)

  // The name assigned to the node that was introduced to the broker by the
  // corresponding RequestIndirectBrokerConnection message.
  IPCZ_MSG_PARAM(NodeName, connected_node_name)
IPCZ_MSG_END()

// Sent by a non-broker node to a broker node. Requests that the broker provide
// a new NodeLink to both the sender and the node identified by `name`, linking
// the two nodes together and allowing them to communicate directly.
//
// If the broker does not know the node named `name`, it will send an
// IntroduceNode message back to the sender indicating failure.
//
// Otherwise it will send an IntroduceNode message to both the sender
// and the node identified by `name`, with complementary transport descriptors
// attached to each, and other details to facilitate direct communication
// between the two nodes.
IPCZ_MSG_NO_REPLY(RequestIntroduction, IPCZ_MSG_ID(12), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(NodeName, name)
IPCZ_MSG_END()

// Message ID 7 is used by IntroduceNode in node_messages.h.

// Message ID 10 is used by AcceptParcel in node_messages.h.

// Notifies a node that the route has been closed on one side. If this arrives
// at a router from an inward-facing or bridge link, it pertains to the router's
// own side of the route; otherwise it indicates that the other side of the
// route has been closed.
IPCZ_MSG_NO_REPLY(RouteClosed, IPCZ_MSG_ID(21), IPCZ_MSG_VERSION(0))
  // In the context of the receiving NodeLink, this identifies the specific
  // Router to receive this message.
  IPCZ_MSG_PARAM(RoutingId, routing_id)

  // The total number of parcels sent from the side of the route which closed,
  // before closing.
  IPCZ_MSG_PARAM(SequenceNumber, sequence_length)
IPCZ_MSG_END()

// Informs the recipient that its outward peer is a proxy which has locked its
// own outward (and implicitly central) link for impending bypass. This means
// it's safe for the recipient of this message to send a BypassProxy message
// to the node identified by `proxy_peer_node_name`.
IPCZ_MSG_NO_REPLY(InitiateProxyBypass, IPCZ_MSG_ID(30), IPCZ_MSG_VERSION(0))
  // In the context of the receiving NodeLink, this identifies the specific
  // Router to receive this message. This is the proxy's inward peer.
  IPCZ_MSG_PARAM(RoutingId, routing_id)

  // Padding for NodeName alignment. Reserved for future use and must be zero.
  IPCZ_MSG_PARAM(uint64_t, reserved0)

  // The name of the node on which the proxy's outward peer router lives.
  IPCZ_MSG_PARAM(NodeName, proxy_peer_name)

  // The routing ID of the RouterLink between the proxy router and its outward
  // peer on the above-named node.
  IPCZ_MSG_PARAM(RoutingId, proxy_peer_routing_id)
IPCZ_MSG_END()

// Informs the recipient that its outward link is connected to a proxying router
// and requests that this link be replaced immediately with a more direct link
// to the sender of this message, who must be the proxy's inward peer.
//
// By the time this message is sent, the proxy must have already shared the name
// of its inward peer's node (the node sending this message) with the recipient,
// as a way for the recipient to authenticate this request.
IPCZ_MSG_NO_REPLY(BypassProxy, IPCZ_MSG_ID(31), IPCZ_MSG_VERSION(0))
  // The name of the node on which the proxy resides.
  IPCZ_MSG_PARAM(NodeName, proxy_name)

  // In the context of the receiving NodeLink, this identifies the specific
  // Router to receive this message. This is the proxy's outward peer.
  IPCZ_MSG_PARAM(RoutingId, proxy_routing_id)

  // A new routing ID between the sender of this message and the receiving node,
  // establishing a direct RouterLink between the sender and receiver to use as
  // a repacement for the above identified RouterLink. This link effectively
  // bypasses the above link along the route.
  IPCZ_MSG_PARAM(RoutingId, new_routing_id)

  // The total number of parcels sent from the proxy's side of the route to the
  // recipient's side of the route before the proxy's side stopped sending
  // parcels through the proxy and started sending directly on the new link
  // introduced by this message. This informs the recipient of when it can stop
  // expecting new parcels to arrive via `proxy_routing_id`.
  IPCZ_MSG_PARAM(SequenceNumber, proxy_outbound_sequence_length)
IPCZ_MSG_END()

// Informs the recipient that the portal on `routing_id` for this NodeLink can
// cease to exist once it has received and forwarded parcels up to the
// specified sequence length in each direction.
IPCZ_MSG_NO_REPLY(StopProxying, IPCZ_MSG_ID(32), IPCZ_MSG_VERSION(0))
  // In the context of the receiving NodeLink, this identifies the specific
  // Router to receive this message. This is the proxy itself.
  IPCZ_MSG_PARAM(RoutingId, routing_id)

  // The total number of parcels sent from the other side of the route towards
  // the proxy's own side before the proxy's outward peer began bypassing the
  // proxy and sending parcels directly to its outward peer. This informs the
  // proxy of when it can stop expecting new parcels to arrive on RoutingId.
  IPCZ_MSG_PARAM(SequenceNumber, proxy_inbound_sequence_length)

  // The total number of parcels sent from the proxy's own side of the route
  // towards the other side before the proxy's inward peer began bypassing the
  // proxy and sending parcels directly to its outward peer. This informs the
  // proxy of when it can stop expecting new outbound parcels to forward to its
  // outward peer on `routing_id`.
  IPCZ_MSG_PARAM(SequenceNumber, proxy_outbound_sequence_length)
IPCZ_MSG_END()

// Informs the recipient of when its decaying outward peer will stop sending
// inbound parcels.
IPCZ_MSG_NO_REPLY(ProxyWillStop, IPCZ_MSG_ID(33), IPCZ_MSG_VERSION(0))
  // In the context of the receiving NodeLink, this identifies the specific
  // Router to receive this message. This is the proxy's inward peer.
  IPCZ_MSG_PARAM(RoutingId, routing_id)

  // The total number of parcels sent toward the recipients side of the route
  // through the proxying router. Beyond this length, all subsequent parcels
  // in that direction will have bypassed the proxy. Informs the recipient of
  // when it can stop expecting new parcels from the proxy.
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
IPCZ_MSG_NO_REPLY(BypassProxyToSameNode, IPCZ_MSG_ID(34), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(RoutingId, routing_id)
  IPCZ_MSG_PARAM(RoutingId, new_routing_id)
  IPCZ_MSG_PARAM(SequenceNumber, proxy_inbound_sequence_length)
IPCZ_MSG_END()

// Informs the recipient that it has been bypassed by the sender in favor of a
// direct route to the sender's local outward peer. This is essentially a reply
// to BypassProxyToSameNode.
IPCZ_MSG_NO_REPLY(StopProxyingToLocalPeer, IPCZ_MSG_ID(35), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(RoutingId, routing_id)
  IPCZ_MSG_PARAM(SequenceNumber, proxy_outbound_sequence_length)
IPCZ_MSG_END()

// Notifies the target router that bypass of its outward link may be possible.
// This may be sent to catalyze route reduction in some cases where the router
// in question could otherwise fail indefinitely to notice a bypass opportunity
// due to a lack of interesting state changes.
IPCZ_MSG_NO_REPLY(NotifyBypassPossible, IPCZ_MSG_ID(36), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(RoutingId, routing_id)
IPCZ_MSG_END()

// Requests that the receiving Router log a description of itself and forward
// this request along the same direction in which it was received.
IPCZ_MSG_NO_REPLY(LogRouteTrace, IPCZ_MSG_ID(240), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(RoutingId, routing_id)
IPCZ_MSG_END()
