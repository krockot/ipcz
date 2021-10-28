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
#include "third_party/abseil-cpp/absl/container/flat_hash_map.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"

namespace ipcz {
namespace core {

class NodeLink;

// Node encompasses the state of an isolated ipcz node.
class Node : public mem::RefCounted, private Router {
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

  enum class Type {
    kNormal,
    kBroker,
  };

  explicit Node(Type type);

  void ShutDown();

  bool is_broker() const { return type_ == Type::kBroker; }

  mem::Ref<NodeLink> GetBrokerLink();

  Portal::Pair OpenPortals();
  IpczResult OpenRemotePortal(os::Channel channel,
                              os::Process process,
                              mem::Ref<Portal>& out_portal);
  IpczResult AcceptRemotePortal(os::Channel channel,
                                mem::Ref<Portal>& out_portal);

  bool AcceptInvitation(const PortalAddress& my_address,
                        const PortalAddress& broker_portal);

 private:
  friend class LockedRouter;

  ~Node() override;

  // Router:
  void RouteParcel(const PortalAddress& destination, Parcel& parcel) override;

  const Type type_;

  absl::Mutex mutex_;
  NodeName name_;
  mem::Ref<NodeLink> broker_link_;
  mem::Ref<Portal> portal_waiting_for_invitation_;
  absl::flat_hash_map<NodeName, mem::Ref<NodeLink>> peer_links_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NODE_H_
