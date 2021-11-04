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
#include "core/portal_control_block.h"
#include "core/trap_event_dispatcher.h"
#include "os/memory.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"

namespace ipcz {
namespace core {

namespace {

// Serialized representation of a Portal sent in a parcel. Implicitly the portal
// in question is moving from the sending node to the receiving node.
struct IPCZ_ALIGN(16) SerializedPortal {
  Side side;
  RouteId route;

  // TODO: provide an index into some NodeLink shared state where we can host a
  // corrpesonding PortalControlBlock.
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
  os::Memory control_block(sizeof(PortalControlBlock));
  os::Memory::Mapping control = control_block.Map();
  PortalControlBlock::Initialize(control.base());

  os::Memory link_state_memory(sizeof(NodeLinkState));
  os::Memory::Mapping link_state_mapping = link_state_memory.Map();
  NodeLinkState& link_state =
      NodeLinkState::Initialize(link_state_mapping.base());

  mem::Ref<Portal> portal = mem::MakeRefCounted<Portal>(Side::kLeft);
  RouteId route;
  msg::InviteNode invitation;
  {
    absl::MutexLock lock(&mutex_);
    do {
      route = link_state.AllocateRoutes(1);
    } while (!AssignRoute(route, portal));
    local_name_ = local_name;
    remote_name_ = remote_name;
    link_state_mapping_ = std::move(link_state_mapping);
    invitation.params.protocol_version = msg::kProtocolVersion;
    invitation.params.source_name = local_name;
    invitation.params.target_name = remote_name;
    invitation.params.route = route;
    invitation.handles.control_block_memory = control_block.TakeHandle();
    invitation.handles.link_state_memory = link_state_memory.TakeHandle();
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
      mem::WrapRefCounted(this), route, std::move(control)));
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
      sizeof(internal::MessageHeader) + sizeof(RouteId) + sizeof(uint32_t) * 3 +
      parcel.data_view().size() + num_portals * sizeof(SerializedPortal) +
      (num_os_handles + num_portals) * sizeof(internal::OSHandleData);
  serialized_data.resize(serialized_size);

  auto& header =
      *reinterpret_cast<internal::MessageHeader*>(serialized_data.data());
  header.size = sizeof(header);
  header.message_id = msg::kAcceptParcelId;
  auto& msg_route = *reinterpret_cast<RouteId*>(&header + 1);
  msg_route = route;
  auto* sizes = reinterpret_cast<uint32_t*>(&msg_route + 1);
  sizes[0] = parcel.data_view().size();
  sizes[1] = num_portals;
  sizes[2] = num_os_handles + num_portals;
  auto* data = reinterpret_cast<uint8_t*>(sizes + 3);
  memcpy(data, parcel.data_view().data(), parcel.data_view().size());
  auto* portals = reinterpret_cast<SerializedPortal*>(data + sizes[0]);

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
    portals[i].side = portal.side;
    portals[i].route = route;

    os::Memory control_block_memory(sizeof(PortalControlBlock));
    os::Memory::Mapping control_block = control_block_memory.Map();
    PortalControlBlock::Initialize(control_block.base());
    os_handles.push_back(control_block_memory.TakeHandle());
    // TODO: populate OSHandleData too

    // If we had a local peer before this transmission, that local peer will
    // adopt this link as its peer link. Otherwise we will adopt it as our own
    // forwarding link.
    mem::Ref<PortalLink> link = mem::MakeRefCounted<PortalLink>(
        mem::WrapRefCounted(this), route, std::move(control_block));
    if (portal.local_peer_before_transit) {
      {
        absl::MutexLock lock(&mutex_);
        AssignRoute(route, portal.local_peer_before_transit);
      }
      portal.local_peer_before_transit->SetPeerLink(std::move(link));
    } else {
      // TODO- any reason for forwarding target to reply?
      portal.portal->SetForwardingLink(std::move(link));
    }
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

void NodeLink::SendPeerClosed(RouteId route) {
  msg::PeerClosed m;
  m.params.route = route;
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
  os::Memory link_state_memory(std::move(m.handles.link_state_memory),
                               sizeof(NodeLinkState));
  mem::Ref<Portal> portal;
  {
    absl::MutexLock lock(&mutex_);
    if (remote_node_type_ == Node::Type::kBroker &&
        portal_awaiting_invitation_ && !remote_name_.is_valid() &&
        !local_name_.is_valid()) {
      remote_name_ = m.params.source_name;
      local_name_ = m.params.target_name;
      link_state_mapping_ = link_state_memory.Map();
      if (AssignRoute(m.params.route, portal_awaiting_invitation_)) {
        if (node_->AcceptInvitationFromBroker(m.params.source_name,
                                              m.params.target_name)) {
          portal = std::move(portal_awaiting_invitation_);
        } else {
          routes_.erase(m.params.route);
        }
      }
    }
  }

  msg::InviteNode_Reply reply;
  reply.header.request_id = m.header.request_id;
  reply.params.protocol_version = msg::kProtocolVersion;
  reply.params.accepted = portal != nullptr;
  Send(reply);

  if (portal) {
    // Note that we don't set the portal's active link until after we've replied
    // to accept the invitation. This is because the portal may immediately
    // initiate communication over the route within SetPeerLink(), e.g. to flush
    // outgoing parcels.
    os::Memory control_block(std::move(m.handles.control_block_memory),
                             sizeof(PortalControlBlock));
    portal->SetPeerLink(mem::MakeRefCounted<PortalLink>(
        mem::WrapRefCounted(this), m.params.route, control_block.Map()));
  }
  return true;
}

bool NodeLink::OnPeerClosed(msg::PeerClosed& m) {
  TrapEventDispatcher dispatcher;
  mem::Ref<Portal> portal = GetPortalForRoute(m.params.route);
  // Note that the portal may have already been closed locally, so we can't
  // treat the route's absence here as an error.
  if (portal) {
    portal->NotifyPeerClosed(dispatcher);
  }
  return true;
}

bool NodeLink::OnAcceptParcel(os::Channel::Message m) {
  if (m.data.size() < sizeof(internal::MessageHeader)) {
    return false;
  }

  const auto& header =
      *reinterpret_cast<const internal::MessageHeader*>(m.data.data());
  const auto& route = *reinterpret_cast<const RouteId*>(&header + 1);
  auto* sizes = reinterpret_cast<const uint32_t*>(&route + 1);
  const uint32_t num_bytes = sizes[0];
  const uint32_t num_portals = sizes[1];
  const uint32_t num_os_handles = sizes[2];
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(sizes + 3);
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
      RouteId route = portals[i].route;
      auto portal = mem::MakeRefCounted<Portal>(portals[i].side);
      if (!AssignRoute(route, portal)) {
        return false;
      }

      os::Memory control_block_memory(std::move(m.handles[i]),
                                      sizeof(PortalControlBlock));
      portals_in_transit[i].portal = portal;
      portals_in_transit[i].side = portals[i].side;

      // Set up a new peer link to be adopted by this portal below.
      portals_in_transit[i].link = mem::MakeRefCounted<PortalLink>(
          mem::WrapRefCounted(this), route, control_block_memory.Map());
    }
  }

  for (PortalInTransit& portal : portals_in_transit) {
    portal.portal->SetPeerLink(std::move(portal.link));
  }

  std::vector<os::Handle> os_handles;
  os_handles.reserve(m.handles.size() - num_portals);
  for (size_t i = num_portals; i < m.handles.size(); ++i) {
    os_handles.push_back(std::move(m.handles[i]));
  }

  TrapEventDispatcher dispatcher;
  Parcel parcel;
  parcel.SetData(std::vector<uint8_t>(bytes, bytes + num_bytes));
  parcel.SetPortals(std::move(portals_in_transit));
  parcel.SetOSHandles(std::move(os_handles));

  mem::Ref<Portal> receiver = GetPortalForRoute(route);
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
