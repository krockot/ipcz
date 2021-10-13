// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_NODE_H_
#define IPCZ_SRC_CORE_NODE_H_

#include <memory>

#include "core/name.h"
#include "core/portal.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/channel.h"
#include "os/process.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_map.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"

namespace ipcz {
namespace core {

// Node encompasses the state of an isolated ipcz node.
class Node : public mem::RefCounted {
 public:
  Node();

  Portal::Pair OpenPortals();

  IpczResult OpenRemotePortal(os::Channel channel,
                              os::Process process,
                              std::unique_ptr<Portal>& out_portal);
  IpczResult AcceptRemotePortal(os::Channel channel,
                                std::unique_ptr<Portal>& out_portal);

 private:
  ~Node() override;

  absl::Mutex mutex_;
  absl::flat_hash_map<PortalName, Portal*, Name::Hasher> portals_
      ABSL_GUARDED_BY(mutex_);
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NODE_H_
