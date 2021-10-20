// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_NODE_LINK_H_
#define IPCZ_SRC_CORE_NODE_LINK_H_

#include <atomic>
#include <functional>

#include "core/message.h"
#include "core/node.h"
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
    Send(message.data(), message.handles());
  }

  // Sends a message which expects a specific type of reply. If the remote node
  // indicates that they won't reply (for example, if they are on an older
  // version and don't understand the request), then `reply_handler` receives a
  // null value.
  template <typename T, typename = std::enable_if_t<T::kExpectsReply>>
  void Send(T& message,
            std::function<bool(const typename T::Reply*)> reply_handler) {
    SendWithReplyHandler(
        message.data(), message.handles(),
        [reply_handler](const os::Channel::Message* message) {
          // NOTE: `message` is already partially validated (for correct type
          // and data size) by the time this handler is invoked, so this cast is
          // safe.
          if (!message) {
            return reply_handler(nullptr);
          } else {
            return reply_handler(reinterpret_cast<const typename T::Reply*>(
                message->data.data()));
          }
        });
  }

  // Generic implementations to support the above template helpers.
  using GenericReplyHandler = std::function<bool(const os::Channel::Message*)>;
  void Send(absl::Span<uint8_t> data, absl::Span<os::Handle> handles = {});
  void SendWithReplyHandler(absl::Span<uint8_t> data,
                            absl::Span<os::Handle> handles,
                            GenericReplyHandler reply_handler);

 private:
  ~NodeLink() override;

  bool OnMessage(os::Channel::Message message);

  const mem::Ref<Node> node_;
  std::atomic<uint16_t> next_request_id_{1};

  absl::Mutex mutex_;
  os::Channel channel_ ABSL_GUARDED_BY(mutex_);
  os::Process remote_process_ ABSL_GUARDED_BY(mutex_);
  absl::flat_hash_map<uint16_t, GenericReplyHandler> pending_requests_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NODE_LINK_H_
