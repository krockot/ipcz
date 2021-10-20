// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_NODE_H_
#define IPCZ_SRC_CORE_NODE_H_

#include "core/name.h"
#include "core/portal.h"
#include "core/router.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/channel.h"
#include "os/process.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_set.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"

namespace ipcz {
namespace core {

class NodeLink;

// Node encompasses the state of an isolated ipcz node.
class Node : public mem::RefCounted {
 public:
  // Scoped accessor to the Node's internal Router, ensuring the Router's mutex
  // is held during access.
  class LockedRouter {
   public:
    explicit LockedRouter(Node& node);
    ~LockedRouter();

    Router* operator->() const { return &router_; }
    Router& operator*() const { return router_; }

   private:
    Router& router_;
    absl::MutexLock lock_;
  };

  Node();

  void ShutDown();

  Portal::Pair OpenPortals();
  IpczResult OpenRemotePortal(os::Channel channel,
                              os::Process process,
                              mem::Ref<Portal>& out_portal);
  IpczResult AcceptRemotePortal(os::Channel channel,
                                mem::Ref<Portal>& out_portal);

 private:
  friend class LockedRouter;

  ~Node() override;

  // Adds a connection to another node, using `channel` as the medium. `process`
  // is a handle to the process in which the other node lives, if available.
  mem::Ref<NodeLink> AddNodeLink(os::Channel channel, os::Process process);

  absl::Mutex router_mutex_;
  Router router_;
  absl::flat_hash_set<mem::Ref<NodeLink>> anonymous_node_links_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NODE_H_
