// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/node_link.h"

#include "core/buffering_portal_backend.h"
#include "core/message_internal.h"
#include "core/node.h"
#include "core/node_messages.h"
#include "core/parcel.h"
#include "core/portal_control_block.h"
#include "core/trap_event_dispatcher.h"
#include "debug/log.h"
#include "os/memory.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"

namespace ipcz {
namespace core {

namespace {

// Serialized representation of a Portal sent in a parcel. Implicitly the portal
// in question is moving from the sending node to the receiving node.
struct IPCZ_ALIGN(16) SerializedPortal {
  internal::StructHeader header;
  PortalName moved_from;
  PortalName moved_to;
  Side side;
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

void NodeLink::SetRemoteNodeName(const NodeName& name) {
  absl::MutexLock lock(&mutex_);
  remote_node_name_ = name;
}

void NodeLink::SetRemoteProtocolVersion(uint32_t version) {
  absl::MutexLock lock(&mutex_);
  remote_protocol_version_ = version;
}

void NodeLink::AddRoutedPortal(const PortalName& local_portal_name) {
  absl::MutexLock lock(&mutex_);
  routed_local_portals_.insert(local_portal_name);
}

void NodeLink::RemoveRoutedPortal(const PortalName& local_portal_name) {
  absl::MutexLock lock(&mutex_);
  routed_local_portals_.erase(local_portal_name);
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

void NodeLink::SendParcel(const PortalName& destination, Parcel& parcel) {
  // Build small messages on the stack.
  absl::InlinedVector<uint8_t, 256> serialized_data;

  // big ad hoc mess of custom serialization. balanced by deserialization in
  // OnAcceptParcel.

  size_t serialized_size =
      sizeof(internal::MessageHeader) + sizeof(PortalName) +
      sizeof(uint32_t) * 3 + parcel.data_view().size() +
      parcel.portals_view().size() * sizeof(SerializedPortal) +
      parcel.os_handles_view().size() * sizeof(internal::OSHandleData);
  serialized_data.resize(serialized_size);

  auto& header =
      *reinterpret_cast<internal::MessageHeader*>(serialized_data.data());
  header.size = sizeof(header);
  header.message_id = msg::kAcceptParcelId;
  auto& msg_destination = *reinterpret_cast<PortalName*>(&header + 1);
  msg_destination = destination;
  auto* sizes = reinterpret_cast<uint32_t*>(&msg_destination + 1);
  sizes[0] = parcel.data_view().size();
  sizes[1] = parcel.portals_view().size();
  sizes[2] = parcel.os_handles_view().size();
  auto* data = reinterpret_cast<uint8_t*>(sizes + 3);
  memcpy(data, parcel.data_view().data(), parcel.data_view().size());
  auto* portals = reinterpret_cast<SerializedPortal*>(data + sizes[0]);

  for (size_t i = 0; i < parcel.portals_view().size(); ++i) {
    PortalInTransit& portal = parcel.portals_view()[i];
    portals[i].header.size = sizeof(internal::StructHeader);
    portals[i].header.version = 0;
    portals[i].moved_from = portal.local_name;
    portals[i].moved_to = portal.new_name;
    portals[i].side = portal.side;
  }

  auto* handle_data = reinterpret_cast<internal::OSHandleData*>(
      portals + parcel.portals_view().size());
  for (size_t i = 0; i < parcel.os_handles_view().size(); ++i) {
    auto& data = handle_data[i];
    auto& handle = parcel.os_handles_view()[i];
    ABSL_ASSERT(handle.is_valid());
    data.header.size = sizeof(internal::StructHeader);
    data.header.version = 0;
#if defined(OS_POSIX)
    data.type = internal::OSHandleDataType::kFileDescriptor;
    data.value = i;
#else
    data.type = internal::OSHandleDataType::kNone;
#endif
  }

  Send(absl::MakeSpan(serialized_data), parcel.os_handles_view());
}

void NodeLink::SendPeerClosed(const PortalName& remote_portal) {
  msg::PeerClosed m;
  m.params.portal = remote_portal;
  Send(m);
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
  if (remote_node_type_ != Node::Type::kBroker) {
    return false;
  }

  bool accepted = node_->AcceptInvitationFromBroker(
      m.params.your_portal, m.params.broker_portal,
      os::Memory(std::move(m.handles.control_block_memory),
                 sizeof(PortalControlBlock)));

  msg::InviteNode_Reply reply;
  reply.header.request_id = m.header.request_id;
  reply.params.protocol_version = msg::kProtocolVersion;
  reply.params.accepted = accepted;
  Send(reply);
  return true;
}

bool NodeLink::OnPeerClosed(msg::PeerClosed& m) {
  TrapEventDispatcher dispatcher;
  return node_->OnPeerClosed(m.params.portal, dispatcher);
}

bool NodeLink::OnAcceptParcel(os::Channel::Message m) {
  if (m.data.size() < sizeof(internal::MessageHeader)) {
    return false;
  }

  const auto& header =
      *reinterpret_cast<const internal::MessageHeader*>(m.data.data());
  const auto& destination = *reinterpret_cast<const PortalName*>(&header + 1);
  auto* sizes = reinterpret_cast<const uint32_t*>(&destination + 1);
  const uint32_t num_bytes = sizes[0];
  const uint32_t num_portals = sizes[1];
  // const uint32_t num_os_handles = sizes[2];
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(sizes + 3);
  const auto* portals =
      reinterpret_cast<const SerializedPortal*>(bytes + num_bytes);
  // const auto* handle_data =
  //     reinterpret_cast<const internal::OSHandleData*>(portals + num_portals);
  std::vector<PortalInTransit> portals_in_transit(num_portals);
  for (size_t i = 0; i < num_portals; ++i) {
    // PortalName old_name = portals[i].local_name;
    PortalName new_name = portals[i].moved_to;
    auto backend = std::make_unique<BufferingPortalBackend>(portals[i].side);
    absl::MutexLock lock(&backend->mutex_);
    backend->routed_name_ = new_name;
    portals_in_transit[i].portal =
        mem::MakeRefCounted<Portal>(*node_, std::move(backend));
  }
  std::vector<os::Handle> os_handles;
  os_handles.reserve(m.handles.size());
  for (auto& handle : m.handles) {
    os_handles.push_back(std::move(handle));
  }

  TrapEventDispatcher dispatcher;
  Parcel parcel;
  parcel.SetData(std::vector<uint8_t>(bytes, bytes + num_bytes));
  parcel.SetPortals(std::move(portals_in_transit));
  parcel.SetOSHandles(std::move(os_handles));
  return node_->AcceptParcel(destination, parcel, dispatcher);
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
