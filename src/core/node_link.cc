// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/node_link.h"

#include "core/message_internal.h"
#include "core/node.h"
#include "core/node_messages.h"
#include "core/node_name.h"
#include "core/parcel.h"
#include "core/portal.h"
#include "core/portal_link_state.h"
#include "core/sequence_number.h"
#include "core/trap_event_dispatcher.h"
#include "os/memory.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"

namespace ipcz {
namespace core {

namespace {

// Serialized representation of a Portal sent in a parcel. Implicitly the portal
// in question is moving from the sending node to the receiving node.
struct IPCZ_ALIGN(8) SerializedPortal {
  RouteId route;
  Side side;
  bool peer_closed : 1;
  SequenceNumber peer_sequence_length;
  SequenceNumber next_incoming_sequence_number;
  SequenceNumber next_outgoing_sequence_number;

  // TODO: provide an index into some NodeLink shared state where we can host a
  // corrpesonding PortalLinkState. for now we allocate a separate shared region
  // for every PortalLinkState, which is not a good idea.
};

struct IPCZ_ALIGN(8) AcceptParcelHeader {
  internal::MessageHeader message_header;
  RouteId route;
  SequenceNumber sequence_number;
  uint32_t num_bytes;
  uint32_t num_portals;
  uint32_t num_os_handles;
};

}  // namespace

NodeLink::NodeLink(Node& node,
                   os::Channel channel,
                   os::Process remote_process,
                   Node::Type remote_node_type)
    : node_(mem::WrapRefCounted(&node)),
      remote_node_type_(remote_node_type),
      channel_(std::move(channel)),
      remote_process_(std::move(remote_process)) {}

NodeLink::~NodeLink() = default;

void NodeLink::Listen() {
  absl::MutexLock lock(&mutex_);
  channel_.Listen([this](os::Channel::Message message) {
    return this->OnMessage(message);
  });
}

mem::Ref<Portal> NodeLink::Invite(const NodeName& local_name,
                                  const NodeName& remote_name) {
  os::Memory portal_link_state_memory(sizeof(PortalLinkState));
  os::Memory::Mapping portal_link_state_mapping =
      portal_link_state_memory.Map();
  PortalLinkState::Initialize(portal_link_state_mapping.base());

  os::Memory node_link_state_memory(sizeof(NodeLinkState));
  os::Memory::Mapping node_link_state_mapping = node_link_state_memory.Map();
  NodeLinkState& node_link_state =
      NodeLinkState::Initialize(node_link_state_mapping.base());

  RouteId route;
  mem::Ref<Portal> portal = mem::MakeRefCounted<Portal>(Side::kLeft);
  mem::Ref<PortalLink> portal_link;
  msg::InviteNode invitation;
  {
    absl::MutexLock lock(&mutex_);
    do {
      route = node_link_state.AllocateRoutes(1);
    } while (!AssignRoute(route, portal));
    local_name_ = local_name;
    remote_name_ = remote_name;
    link_state_mapping_ = std::move(node_link_state_mapping);
    invitation.params.protocol_version = msg::kProtocolVersion;
    invitation.params.source_name = local_name;
    invitation.params.target_name = remote_name;
    invitation.params.route = route;
    invitation.handles.node_link_state_memory =
        node_link_state_memory.TakeHandle();
    invitation.handles.portal_link_state_memory =
        portal_link_state_memory.TakeHandle();
  }

  Send(invitation,
       [link = mem::WrapRefCounted(this)](const msg::InviteNode_Reply* reply) {
         if (!reply || !reply->params.accepted) {
           // Newer versions may tolerate invitation rejection, but it's the
           // only handshake mechanism we have in v0. Treat this as a validation
           // failure.
           return false;
         }

         link->SetRemoteProtocolVersion(reply->params.protocol_version);
         return true;
       });

  portal->SetPeerLink(mem::MakeRefCounted<PortalLink>(
      mem::WrapRefCounted(this), route, std::move(portal_link_state_mapping)));
  return portal;
}

mem::Ref<Portal> NodeLink::AwaitInvitation() {
  mem::Ref<Portal> portal = mem::MakeRefCounted<Portal>(Side::kRight);

  absl::MutexLock lock(&mutex_);
  portal_awaiting_invitation_ = portal;
  return portal;
}

void NodeLink::SetRemoteProtocolVersion(uint32_t version) {
  absl::MutexLock lock(&mutex_);
  remote_protocol_version_ = version;
}

void NodeLink::Send(absl::Span<uint8_t> data, absl::Span<os::Handle> handles) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(channel_.is_valid());
  ABSL_ASSERT(data.size() >= sizeof(internal::MessageHeader));
  channel_.Send(os::Channel::Message(os::Channel::Data(data), handles));
}

void NodeLink::SendWithReplyHandler(absl::Span<uint8_t> data,
                                    absl::Span<os::Handle> handles,
                                    GenericReplyHandler reply_handler) {
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(channel_.is_valid());
  ABSL_ASSERT(data.size() >= sizeof(internal::MessageHeader));
  internal::MessageHeader& header =
      *reinterpret_cast<internal::MessageHeader*>(data.data());
  header.request_id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
  PendingReply pending_reply(header.message_id, std::move(reply_handler));
  while (
      !header.request_id ||
      !pending_replies_.try_emplace(header.request_id, std::move(pending_reply))
           .second) {
    header.request_id =
        next_request_id_.fetch_add(1, std::memory_order_relaxed);
  }
  channel_.Send(os::Channel::Message(os::Channel::Data(data), handles));
}

void NodeLink::SendParcel(RouteId route, Parcel& parcel) {
  // Build small messages on the stack.
  absl::InlinedVector<uint8_t, 256> serialized_data;

  // big ad hoc mess of custom serialization. balanced by deserialization in
  // OnAcceptParcel.

  const size_t num_portals = parcel.portals_view().size();
  const size_t num_os_handles = parcel.os_handles_view().size();
  const size_t serialized_size =
      sizeof(AcceptParcelHeader) + parcel.data_view().size() +
      num_portals * sizeof(SerializedPortal) +
      (num_os_handles + num_portals) * sizeof(internal::OSHandleData);
  serialized_data.resize(serialized_size);

  auto& header = *reinterpret_cast<AcceptParcelHeader*>(serialized_data.data());
  header.message_header.size = sizeof(header.message_header);
  header.message_header.message_id = msg::kAcceptParcelId;
  header.route = route;
  header.sequence_number = parcel.sequence_number();
  header.num_bytes = parcel.data_view().size();
  header.num_portals = num_portals;
  header.num_os_handles = num_portals + num_os_handles;
  auto* data = reinterpret_cast<uint8_t*>(&header + 1);
  memcpy(data, parcel.data_view().data(), parcel.data_view().size());
  auto* portals = reinterpret_cast<SerializedPortal*>(data + header.num_bytes);

  RouteId first_new_route;
  {
    absl::MutexLock lock(&mutex_);
    first_new_route = AllocateRoutes(num_portals);
  }

  std::vector<os::Handle> os_handles;
  os_handles.reserve(num_os_handles + num_portals);
  for (size_t i = 0; i < num_portals; ++i) {
    const RouteId route = first_new_route + i;
    PortalInTransit& portal = parcel.portals_view()[i];

    portals[i].route = route;
    portals[i].side = portal.side;
    portals[i].peer_closed = portal.peer_closed;
    portals[i].peer_sequence_length = portal.peer_sequence_length;
    portals[i].next_incoming_sequence_number =
        portal.next_incoming_sequence_number;
    portals[i].next_outgoing_sequence_number =
        portal.next_outgoing_sequence_number;

    os::Memory link_state_memory(sizeof(PortalLinkState));
    os::Memory::Mapping link_state = link_state_memory.Map();
    PortalLinkState::Initialize(link_state.base());
    os_handles.push_back(link_state_memory.TakeHandle());
    // TODO: populate OSHandleData too

    // The link will actually be given to the appropriate portal (either
    // `portal.portal` or its local peer where applicable) in
    // Portal::FinalizeAfterTransit().
    portal.link = mem::MakeRefCounted<PortalLink>(mem::WrapRefCounted(this),
                                                  route, std::move(link_state));

    // It's important to assign the route to a Portal before we transmit this
    // parcel, in case the receiver immediately sends back messages on the new
    // route.
    bool assigned;
    absl::MutexLock lock(&mutex_);
    if (portal.local_peer_before_transit) {
      // When the moved portal's peer is local to this node, the route gets
      // assigned to them and the moved portal essentially drops out of
      // existence on this node.
      assigned = AssignRoute(route, portal.local_peer_before_transit);
    } else {
      // When the moved portal's peer is somewhere else, the moved portal hangs
      // around on this node and gets assigned the route to its new location.
      // This will exist only temporarily until we can help the new location
      // establish a link to an appropriate peer-side portal.
      assigned = AssignRoute(route, portal.portal);
    }

    // TODO: A badassigned peer might steal the already-allocated routes from
    // above, in which case `assigned` will be false. Need to gracefully sever
    // the NodeLink in that case instead of asserting.
    ABSL_ASSERT(assigned);
  }

  auto* handle_data =
      reinterpret_cast<internal::OSHandleData*>(portals + num_portals);
  for (size_t i = 0; i < num_os_handles; ++i) {
    auto& data = handle_data[num_portals + i];
    auto& handle = parcel.os_handles_view()[i];
    ABSL_ASSERT(handle.is_valid());
    data.header.size = sizeof(internal::StructHeader);
    data.header.version = 0;
#if defined(OS_POSIX)
    data.type = internal::OSHandleDataType::kFileDescriptor;
    data.value = i + 1;
#else
    data.type = internal::OSHandleDataType::kNone;
#endif
    os_handles.push_back(std::move(handle));
  }

  Send(absl::MakeSpan(serialized_data), absl::MakeSpan(os_handles));
}

void NodeLink::SendPeerClosed(RouteId route, SequenceNumber sequence_length) {
  msg::PeerClosed m;
  m.params.route = route;
  m.params.sequence_length = sequence_length;
  Send(m);
}

RouteId NodeLink::AllocateRoutes(size_t count) {
  mutex_.AssertHeld();

  // Routes must only be allocated once we're a fully functioning link.
  ABSL_ASSERT(link_state_mapping_.is_valid());
  return link_state_mapping_.As<NodeLinkState>()->AllocateRoutes(count);
}

bool NodeLink::AssignRoute(RouteId id, const mem::Ref<Portal>& portal) {
  mutex_.AssertHeld();
  auto result = routes_.try_emplace(id, portal);
  return result.second;
}

mem::Ref<Portal> NodeLink::GetPortalForRoute(RouteId id) {
  absl::MutexLock lock(&mutex_);
  auto it = routes_.find(id);
  if (it == routes_.end()) {
    return nullptr;
  }
  return it->second;
}

bool NodeLink::OnMessage(os::Channel::Message message) {
  if (message.data.size() < sizeof(internal::MessageHeader)) {
    return false;
  }

  const auto& header =
      *reinterpret_cast<const internal::MessageHeader*>(message.data.data());
  if (header.size < sizeof(internal::MessageHeader)) {
    return false;
  }

  if (header.is_reply) {
    return OnReply(message);
  }

  switch (header.message_id) {
#include "core/message_macros/message_dispatch_macros.h"
#include "core/node_message_defs.h"

#include "core/message_macros/undef_message_macros.h"

    case msg::kAcceptParcelId:
      return OnAcceptParcel(message);

    default:
      // Unknown message types may come from clients using a newer ipcz version.
      // If they expect a reply, reply to indicate that we don't know what
      // they're talking about. Otherwise we silently ignore the message.
      if (header.expects_reply) {
        internal::MessageHeader nope{sizeof(nope)};
        nope.message_id = header.message_id;
        nope.wont_reply = true;
        nope.request_id = header.request_id;
        Send(absl::MakeSpan(reinterpret_cast<uint8_t*>(&nope), sizeof(nope)));
      }
      return true;
  }
}

bool NodeLink::OnReply(os::Channel::Message message) {
  // TODO: make sure we get a validated header from OnMessage(), and separately
  // copy message data
  const auto& header =
      *reinterpret_cast<const internal::MessageHeader*>(message.data.data());

  PendingReply pending_reply;
  {
    absl::MutexLock lock(&mutex_);
    auto it = pending_replies_.find(header.request_id);
    if (it == pending_replies_.end()) {
      // No reply expected with this ID. Oops! Validation failure.
      return false;
    }
    pending_reply = std::move(it->second);
    pending_replies_.erase(it);
  }

  // Not the message type we were expecting for this reply. Reject!
  if (header.message_id != pending_reply.message_id) {
    return false;
  }

  // The receiving Node did not understand our request.
  if (header.wont_reply) {
    pending_reply.handler(nullptr);
    return true;
  }

  switch (header.message_id) {
#include "core/message_macros/message_reply_dispatch_macros.h"
#include "core/node_message_defs.h"

#include "core/message_macros/undef_message_macros.h"

    default:
      // Replies for unrecognized message IDs don't make sense. Reject!
      return false;
  }
}

bool NodeLink::OnInviteNode(msg::InviteNode& m) {
  os::Memory node_link_state(std::move(m.handles.node_link_state_memory),
                             sizeof(NodeLinkState));
  os::Memory portal_link_state(std::move(m.handles.portal_link_state_memory),
                               sizeof(PortalLinkState));
  mem::Ref<PortalLink> portal_link = mem::MakeRefCounted<PortalLink>(
      mem::WrapRefCounted(this), m.params.route, portal_link_state.Map());

  mem::Ref<Portal> portal;
  {
    absl::MutexLock lock(&mutex_);
    if (remote_node_type_ == Node::Type::kBroker &&
        portal_awaiting_invitation_ && !remote_name_.is_valid() &&
        !local_name_.is_valid()) {
      remote_name_ = m.params.source_name;
      local_name_ = m.params.target_name;
      link_state_mapping_ = node_link_state.Map();
      if (AssignRoute(m.params.route, portal_awaiting_invitation_)) {
        if (node_->AcceptInvitationFromBroker(m.params.source_name,
                                              m.params.target_name)) {
          portal = std::move(portal_awaiting_invitation_);
        }
      }
    }
  }

  const bool accepted = portal != nullptr;
  msg::InviteNode_Reply reply;
  reply.header.request_id = m.header.request_id;
  reply.params.protocol_version = msg::kProtocolVersion;
  reply.params.accepted = accepted;
  Send(reply);

  if (accepted) {
    // Note that we don't set the portal's active link until after we've replied
    // to accept the invitation. This is because the portal may immediately
    // initiate communication over the route within SetPeerLink(), e.g. to flush
    // outgoing parcels, and we want the invitation reply to arrive first.
    portal->SetPeerLink(std::move(portal_link));
  } else {
    absl::MutexLock lock(&mutex_);
    routes_.erase(m.params.route);
  }
  return true;
}

bool NodeLink::OnPeerClosed(msg::PeerClosed& m) {
  TrapEventDispatcher dispatcher;
  mem::Ref<Portal> portal = GetPortalForRoute(m.params.route);
  // Note that the portal may have already been closed locally, so we can't
  // treat the route's absence here as an error.
  if (portal) {
    portal->NotifyPeerClosed(m.params.sequence_length, dispatcher);
  }
  return true;
}

bool NodeLink::OnAcceptParcel(os::Channel::Message m) {
  if (m.data.size() < sizeof(AcceptParcelHeader)) {
    return false;
  }

  const auto& header =
      *reinterpret_cast<const AcceptParcelHeader*>(m.data.data());
  const uint32_t num_bytes = header.num_bytes;
  const uint32_t num_portals = header.num_portals;
  const uint32_t num_os_handles = header.num_os_handles;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&header + 1);
  const auto* portals =
      reinterpret_cast<const SerializedPortal*>(bytes + num_bytes);

  // const auto* handle_data =
  //     reinterpret_cast<const internal::OSHandleData*>(portals + num_portals);
  if (num_os_handles < num_portals) {
    return false;
  }
  if (m.handles.size() != num_os_handles) {
    return false;
  }

  Parcel::PortalVector portals_in_transit(num_portals);
  {
    absl::MutexLock lock(&mutex_);
    for (size_t i = 0; i < num_portals; ++i) {
      auto portal = mem::MakeRefCounted<Portal>(portals[i].side);

      os::Memory link_state_memory(std::move(m.handles[i]),
                                   sizeof(PortalLinkState));
      RouteId route = portals[i].route;
      if (!AssignRoute(route, portal)) {
        return false;
      }

      portals_in_transit[i].portal = portal;
      portals_in_transit[i].side = portals[i].side;
      portals_in_transit[i].peer_closed = portals[i].peer_closed;
      portals_in_transit[i].peer_sequence_length =
          portals[i].peer_sequence_length;
      portals_in_transit[i].next_incoming_sequence_number =
          portals[i].next_incoming_sequence_number;
      portals_in_transit[i].next_outgoing_sequence_number =
          portals[i].next_outgoing_sequence_number;
      portals_in_transit[i].link = mem::MakeRefCounted<PortalLink>(
          mem::WrapRefCounted(this), route, link_state_memory.Map());
    }
  }

  for (PortalInTransit& portal : portals_in_transit) {
    portal.portal->DeserializeFromTransit(portal);
  }

  std::vector<os::Handle> os_handles;
  os_handles.reserve(m.handles.size() - num_portals);
  for (size_t i = num_portals; i < m.handles.size(); ++i) {
    os_handles.push_back(std::move(m.handles[i]));
  }

  TrapEventDispatcher dispatcher;
  Parcel parcel(header.sequence_number);
  parcel.SetData(std::vector<uint8_t>(bytes, bytes + num_bytes));
  parcel.SetPortals(std::move(portals_in_transit));
  parcel.SetOSHandles(std::move(os_handles));

  mem::Ref<Portal> receiver = GetPortalForRoute(header.route);
  if (!receiver) {
    return true;
  }

  return receiver->AcceptParcelFromLink(parcel, dispatcher);
}

NodeLink::PendingReply::PendingReply() = default;

NodeLink::PendingReply::PendingReply(uint8_t message_id,
                                     GenericReplyHandler handler)
    : message_id(message_id), handler(handler) {}

NodeLink::PendingReply::PendingReply(PendingReply&&) = default;

NodeLink::PendingReply& NodeLink::PendingReply::operator=(PendingReply&&) =
    default;

NodeLink::PendingReply::~PendingReply() = default;

}  // namespace core
}  // namespace ipcz
