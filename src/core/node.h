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
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"

namespace ipcz {
namespace core {

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

  Portal::Pair OpenPortals();
  IpczResult OpenRemotePortal(os::Channel channel,
                              os::Process process,
                              mem::Ref<Portal>& out_portal);
  IpczResult AcceptRemotePortal(os::Channel channel,
                                mem::Ref<Portal>& out_portal);

 private:
  friend class LockedRouter;

  ~Node() override;

  absl::Mutex router_mutex_;
  Router router_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NODE_H_
