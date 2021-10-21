// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_NODE_LINK_H_
#define IPCZ_SRC_CORE_NODE_LINK_H_

#include <atomic>
#include <functional>

#include "core/node.h"
#include "core/node_messages.h"
#include "mem/ref_counted.h"
#include "os/channel.h"
#include "os/process.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_map.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"

namespace ipcz {
namespace core {

class Node;

// NodeLink provides both a client and service interface from one Node to
// another via an os::Channel and potentially other negotiated media like shared
// memory queues.
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
  // Constructs a new NodeLink for Node to talk to a remote node via `channel`
  // as the basic transport. If the caller has a handle to the remote process,
  // it should be passed in `remote_process`. Generally this is only necessary
  // for links initiated by the broker node or by nodes who wish to introduce
  // other new nodes (e.g. their own child processes) to the broker node.
  NodeLink(Node& node, os::Channel channel, os::Process remote_process);

  // Sends a message which does not expect a reply.
  template <typename T, typename = std::enable_if_t<!T::kExpectsReply>>
  void Send(T& message) {
    message.Serialize();
    Send(message.data_view(), message.handles_view());
  }

  // Sends a message which expects a specific type of reply. If the remote node
  // indicates that they won't reply (for example, if they are on an older
  // version and don't understand the request), then `reply_handler` receives a
  // null value.
  template <typename T, typename = std::enable_if_t<T::kExpectsReply>>
  void Send(T& message, std::function<bool(typename T::Reply*)> reply_handler) {
    message.Serialize();
    SendWithReplyHandler(
        message.data_view(), message.handles_view(),
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

 private:
  ~NodeLink() override;

  // Generic entry point for all messages. While the memory addressed by
  // `message` is guaranteed to be safely addressable, it may be untrusted
  // shared memory. No other validation is assumed by this method.
  bool OnMessage(os::Channel::Message message);

  // Generic entry point for message replies. Always dispatched to a
  // corresponding callback after some validation.
  bool OnReply(os::Channel::Message message);

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
  bool OnMessage(msg::RequestBrokerLink& m);

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
  std::atomic<uint16_t> next_request_id_{1};

  absl::Mutex mutex_;
  os::Channel channel_ ABSL_GUARDED_BY(mutex_);
  os::Process remote_process_ ABSL_GUARDED_BY(mutex_);
  absl::flat_hash_map<uint16_t, PendingReply> pending_replies_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NODE_LINK_H_
