// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_NODE_LINK_H_
#define IPCZ_SRC_CORE_NODE_LINK_H_

#include <atomic>

#include "core/message.h"
#include "core/node.h"
#include "mem/ref_counted.h"
#include "os/channel.h"
#include "os/process.h"

namespace ipcz {
namespace core {

class Node;

// NodeLink provides both a client and service interface from one Node to
// another via an os::Channel and any number of optional shared memory queues.
//
// All messages defined by node_messages.h are implemented here and proxied to
// the local Node with any relevant context derived from the client's identity.
class NodeLink : public mem::RefCounted {
 public:
  // Constructs a new NodeLink for Node to talk to a remote node via `channel`
  // as the primordial transport. If the caller has a handle to the remote
  // process, it should be passed in `remote_process`. Generally this is only
  // necessary for links initiated by the broker node or by nodes who wish to
  // introduce other new nodes (e.g. their own child processes) to the broker
  // node.
  NodeLink(Node& node, os::Channel channel, os::Process remote_process);

  template <typename T,
            typename = std::enable_if_t<std::is_base_of<Message, T>::value>>
  void Send(T& message) {
    Send(message.data(), message.handles());
  }

  void Send(absl::Span<uint8_t> data, absl::Span<os::Handle> handles = {});

 private:
  ~NodeLink() override;

  bool OnMessage(os::Channel::Message message);

  const mem::Ref<Node> node_;
  std::atomic<uint16_t> next_request_id_{0};
  os::Channel channel_;
  os::Process remote_process_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NODE_LINK_H_
