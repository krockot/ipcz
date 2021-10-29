// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/node_link.h"

#include "core/message_internal.h"
#include "core/node.h"
#include "core/node_messages.h"
#include "core/portal_control_block.h"
#include "debug/log.h"
#include "os/memory.h"
#include "third_party/abseil-cpp/absl/base/macros.h"

namespace ipcz {
namespace core {

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
