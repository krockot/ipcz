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
IPCZ_MSG_BEGIN(ConnectFromBrokerToNonBroker,
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

  // A serialized shared memory region to serve as the new NodeLink's primary
  // buffer.
  IPCZ_MSG_PARAM_SHARED_MEMORY(buffer)
IPCZ_MSG_END()

// Initial greeting sent by a non-broker node when ConnectNode() is invoked with
// IPCZ_CONNECT_NODE_TO_BROKER. The sending non-broker node expects to receive a
// corresponding ConnectFromBrokerToNonBroker
IPCZ_MSG_BEGIN(ConnectFromNonBrokerToBroker,
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
IPCZ_MSG_BEGIN(ConnectToBrokerIndirect, IPCZ_MSG_ID(2), IPCZ_MSG_VERSION(0))
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
IPCZ_MSG_BEGIN(ConnectFromBrokerIndirect, IPCZ_MSG_ID(3), IPCZ_MSG_VERSION(0))
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

  // A serialized shared memory region to serve as the new NodeLink's primary
  // buffer.
  IPCZ_MSG_PARAM_SHARED_MEMORY(buffer)
IPCZ_MSG_END()

// Message sent from a broker to another broker, to establish a link between
// them and their respective node networks.
IPCZ_MSG_BEGIN(ConnectFromBrokerToBroker, IPCZ_MSG_ID(4), IPCZ_MSG_VERSION(0))
  // The name of the sending broker node.
  IPCZ_MSG_PARAM(NodeName, sender_name)

  // The highest protocol version known and desired by the sender.
  IPCZ_MSG_PARAM(uint32_t, protocol_version)

  // The number of initial portals assumed on the sender's end of the
  // connection. If there is a mismatch between the number sent by each node on
  // an initial connection, the node which sent the larger number should behave
  // as if its excess portals have observed peer closure.
  IPCZ_MSG_PARAM(uint32_t, num_initial_portals)

  // A serialized shared memory region to serve as the new NodeLink's primary
  // buffer. As both nodes are expected to provide such a buffer, the one from
  // the broker with the numericlly lesser NodeName is adopted as the primary
  // link buffer (buffer ID 0). The other buffer is adopted by convention as
  // the first auxilliary NodeLinkMemory buffer, with a BufferId of 1.
  IPCZ_MSG_PARAM_SHARED_MEMORY(buffer)
IPCZ_MSG_END()

// Requests that a broker node accept a new non-broker client, introduced
// indirectly by some established non-broker client on the new client's behalf.
// This message supports ConnectNode() calls which specify
// IPCZ_CONNECT_NODE_SHARE_BROKER. The calling node in that case sends this
// message -- which also contains a serialized representation of the transport
// given to the call -- to its broker.
//
// The broker then uses the transport to complete a special handshake with the
// new client node (via ConnectFromBrokerIndirect and ConnectToBrokerIndirect),
// and it responds to the sender of this message with an
// AcceptIndirectBrokerConnection.
//
// Finally the broker then introduces the sender of this message to the new
// client using the usual IntroduceNode messages. Each non-broker node by that
// point has enough information (by receiving either ConnectFromBrokerIndirect
// or AcceptIndirectBrokerConnection) to expect that introduction and use it to
// establish initial portals between the two non-broker nodes as their original
// ConnectNode() calls intended.
IPCZ_MSG_BEGIN(RequestIndirectBrokerConnection,
               IPCZ_MSG_ID(10),
               IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(uint64_t, request_id)
  IPCZ_MSG_PARAM(uint32_t, num_initial_portals)
  IPCZ_MSG_PARAM_ARRAY(uint8_t, transport_data)
  IPCZ_MSG_PARAM_HANDLE_ARRAY(transport_os_handles)
  IPCZ_MSG_PARAM_HANDLE_ARRAY(process_handle)
IPCZ_MSG_END()

// A reply to RequestIndirectBrokerConnection. If `success` is true, this
// message will be followed immediately by an IntroduceNode message to introduce
// the recipient of this message to the newly connected node. See
// RequestIndirectBrokerConnection for a more detailed explanation.
IPCZ_MSG_BEGIN(AcceptIndirectBrokerConnection,
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
IPCZ_MSG_BEGIN(RequestIntroduction, IPCZ_MSG_ID(12), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(NodeName, name)
IPCZ_MSG_END()

// Introduces one node to another. Sent only by broker nodes and must only be
// accepted from broker nodes. Includes a serialized driver transport descriptor
// which the recipient can use to communicate with the new named node.
IPCZ_MSG_BEGIN(IntroduceNode, IPCZ_MSG_ID(13), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(NodeName, name)
  IPCZ_MSG_PARAM(bool, known)
  IPCZ_MSG_PARAM_ARRAY(uint8_t, transport_data)
  IPCZ_MSG_PARAM_HANDLE_ARRAY(transport_os_handles)
  IPCZ_MSG_PARAM_SHARED_MEMORY(buffer)
IPCZ_MSG_END()

// Shares a new link buffer with the receiver. The buffer may be referenced by
// the given `buffer_id` in the scope of the NodeLink which transmits this
// message. Buffers shared with this message are read-writable to both sides
// of a NodeLink and shared exclusively between the two nodes on either side of
// the transmitting link.
IPCZ_MSG_BEGIN(AddLinkBuffer, IPCZ_MSG_ID(14), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(BufferId, buffer_id)
  IPCZ_MSG_PARAM(uint32_t, buffer_size)
  IPCZ_MSG_PARAM_SHARED_MEMORY(buffer)
IPCZ_MSG_END()

// Conveys the contents of a parcel from one router to another across a node
// boundary.
IPCZ_MSG_BEGIN(AcceptParcel, IPCZ_MSG_ID(20), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(SublinkId, sublink)
  IPCZ_MSG_PARAM(SequenceNumber, sequence_number)
  IPCZ_MSG_PARAM_ARRAY(uint8_t, parcel_data)
  IPCZ_MSG_PARAM_ARRAY(RouterDescriptor, new_routers)
  IPCZ_MSG_PARAM_HANDLE_ARRAY(os_handles)
IPCZ_MSG_END()

// Notifies a node that the route has been closed on one side. This message
// always pertains to the side of the route opposite of the router receiving it,
// guaranteed by the fact that the closed side of the route only transmits this
// message outward once its terminal router is adjacent to the central link.
IPCZ_MSG_BEGIN(RouteClosed, IPCZ_MSG_ID(21), IPCZ_MSG_VERSION(0))
  // In the context of the receiving NodeLink, this identifies the specific
  // Router to receive this message.
  IPCZ_MSG_PARAM(SublinkId, sublink)

  // The total number of parcels sent from the side of the route which closed,
  // before closing.
  IPCZ_MSG_PARAM(SequenceNumber, sequence_length)
IPCZ_MSG_END()

// Notifies a node that one of its routes now has an allocated RouterLinkState
// in the shared NodeLinkMemory buffer identified by `buffer_id`. The receiving
// node may not yet have a handle to the identified buffer, but if not, it can
// expect to receive one imminently via an AddLinkBuffer message.
IPCZ_MSG_BEGIN(SetRouterLinkStateAddress, IPCZ_MSG_ID(22), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(SublinkId, sublink)
  IPCZ_MSG_PARAM(NodeLinkAddress, address)
IPCZ_MSG_END()

// Informs the recipient that its outward peer is a proxy which has locked its
// own outward (and implicitly central) link for impending bypass. This means
// it's safe for the recipient of this message to send a BypassProxy message
// to the node identified by `proxy_peer_node_name`.
IPCZ_MSG_BEGIN(InitiateProxyBypass, IPCZ_MSG_ID(30), IPCZ_MSG_VERSION(0))
  // In the context of the receiving NodeLink, this identifies the specific
  // Router to receive this message. This is the proxy's inward peer.
  IPCZ_MSG_PARAM(SublinkId, sublink)

  // Padding for NodeName alignment. Reserved for future use and must be zero.
  IPCZ_MSG_PARAM(uint64_t, reserved0)

  // The name of the node on which the proxy's outward peer router lives.
  IPCZ_MSG_PARAM(NodeName, proxy_peer_name)

  // The sublink of the RouterLink between the proxy router and its outward
  // peer on the above-named node.
  IPCZ_MSG_PARAM(SublinkId, proxy_peer_sublink)
IPCZ_MSG_END()

// Informs the recipient that its outward link is connected to a proxying router
// and requests that this link be replaced immediately with a more direct link
// to the sender of this message, who must be the proxy's inward peer.
//
// By the time this message is sent, the proxy must have already shared the name
// of its inward peer's node (the node sending this message) with the recipient,
// as a way for the recipient to authenticate this request.
IPCZ_MSG_BEGIN(BypassProxy, IPCZ_MSG_ID(31), IPCZ_MSG_VERSION(0))
  // The name of the node on which the proxy resides.
  IPCZ_MSG_PARAM(NodeName, proxy_name)

  // In the context of the receiving NodeLink, this identifies the specific
  // Router to receive this message. This is the proxy's outward peer.
  IPCZ_MSG_PARAM(SublinkId, proxy_sublink)

  // A new sublink between the sender of this message and the receiving node,
  // establishing a direct RouterLink between the sender and receiver to use as
  // a repacement for the above identified RouterLink. This link effectively
  // bypasses the above link along the route.
  IPCZ_MSG_PARAM(SublinkId, new_sublink)

  // Location of the new route's RouterLinkState.
  IPCZ_MSG_PARAM(NodeLinkAddress, new_link_state_address)

  // The total number of parcels sent from the proxy's side of the route to the
  // recipient's side of the route before the proxy's side stopped sending
  // parcels through the proxy and started sending directly on the new link
  // introduced by this message. This informs the recipient of when it can stop
  // expecting new parcels to arrive via `proxy_sublink`.
  IPCZ_MSG_PARAM(SequenceNumber, proxy_outbound_sequence_length)
IPCZ_MSG_END()

// Informs the recipient that the portal on `sublink` for this NodeLink can
// cease to exist once it has received and forwarded parcels up to the
// specified sequence length in each direction.
IPCZ_MSG_BEGIN(StopProxying, IPCZ_MSG_ID(32), IPCZ_MSG_VERSION(0))
  // In the context of the receiving NodeLink, this identifies the specific
  // Router to receive this message. This is the proxy itself.
  IPCZ_MSG_PARAM(SublinkId, sublink)

  // The total number of parcels sent from the other side of the route towards
  // the proxy's own side before the proxy's outward peer began bypassing the
  // proxy and sending parcels directly to its outward peer. This informs the
  // proxy of when it can stop expecting new parcels to arrive on SublinkId.
  IPCZ_MSG_PARAM(SequenceNumber, proxy_inbound_sequence_length)

  // The total number of parcels sent from the proxy's own side of the route
  // towards the other side before the proxy's inward peer began bypassing the
  // proxy and sending parcels directly to its outward peer. This informs the
  // proxy of when it can stop expecting new outbound parcels to forward to its
  // outward peer on `sublink`.
  IPCZ_MSG_PARAM(SequenceNumber, proxy_outbound_sequence_length)
IPCZ_MSG_END()

// Informs the recipient of when its decaying outward peer will stop sending
// inbound parcels.
IPCZ_MSG_BEGIN(ProxyWillStop, IPCZ_MSG_ID(33), IPCZ_MSG_VERSION(0))
  // In the context of the receiving NodeLink, this identifies the specific
  // Router to receive this message. This is the proxy's inward peer.
  IPCZ_MSG_PARAM(SublinkId, sublink)

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
// decaying its link on `sublink` to the proxy, replacing it with a new link
// link (to the same node) on `new_sublink` to the proxy's outward peer.
//
// `sequence_length` indicates the length of the sequence already sent or
// in-flight over `sublink` from the proxy, and the recipient of this message
// must send a StopProxyingToLocalPeer back to the proxy with the length its own
// outbound sequence had at the time it began decaying the link on `sublink`.
IPCZ_MSG_BEGIN(BypassProxyToSameNode, IPCZ_MSG_ID(34), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(SublinkId, sublink)
  IPCZ_MSG_PARAM(SublinkId, new_sublink)
  IPCZ_MSG_PARAM(NodeLinkAddress, new_link_state_address)
  IPCZ_MSG_PARAM(SequenceNumber, proxy_inbound_sequence_length)
IPCZ_MSG_END()

// Informs the recipient that it has been bypassed by the sender in favor of a
// direct link to the sender's local outward peer. This is essentially a reply
// to BypassProxyToSameNode.
IPCZ_MSG_BEGIN(StopProxyingToLocalPeer, IPCZ_MSG_ID(35), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(SublinkId, sublink)
  IPCZ_MSG_PARAM(SequenceNumber, proxy_outbound_sequence_length)
IPCZ_MSG_END()

// Hints to the target router that it should flush its state. Generally sent to
// catalyze route reduction or elicit some other state change which was blocked
// on work being done by the sender of this message.
IPCZ_MSG_BEGIN(Flush, IPCZ_MSG_ID(36), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(SublinkId, sublink)
IPCZ_MSG_END()

// Requests allocation of a shared memory region of a given size. If the
// recipient can comply, they will send back a corresponding ProvideMemory
// message with a serialized memory region. This message is only sent to a
// node's allocation delegate (usually the broker) as established providing the
// IPCZ_CONNECT_NODE_TO_ALLOCATION_DELEGATE flag to ConnectNode().
IPCZ_MSG_BEGIN(RequestMemory, IPCZ_MSG_ID(64), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(uint32_t, size)
IPCZ_MSG_END()

// Provides a new shared buffer to the receiver, owned exclusively by the
// receiver. The receiver is free to duplicate this buffer and share it with
// other nodes.
IPCZ_MSG_BEGIN(ProvideMemory, IPCZ_MSG_ID(65), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(uint32_t, size)
  IPCZ_MSG_PARAM_SHARED_MEMORY(buffer)
IPCZ_MSG_END()

// Requests that the receiving Router log a description of itself and forward
// this request along the same direction in which it was received.
IPCZ_MSG_BEGIN(LogRouteTrace, IPCZ_MSG_ID(240), IPCZ_MSG_VERSION(0))
  IPCZ_MSG_PARAM(SublinkId, sublink)
IPCZ_MSG_END()
