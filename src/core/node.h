// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_NODE_H_
#define IPCZ_SRC_CORE_NODE_H_

#include <utility>

#include "core/node_name.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/process.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_map.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

class NodeLink;
class Portal;

class Node : public mem::RefCounted {
 public:
  enum class Type {
    kBroker,
    kNormal,
  };

  Node(Type type, const IpczDriver& driver, IpczDriverHandle driver_node);

  const NodeName& name() const { return name_; }
  Type type() const { return type_; }
  const IpczDriver& driver() const { return driver_; }

  void ShutDown();
  IpczResult ConnectNode(IpczDriverHandle driver_transport,
                         Type remote_node_type,
                         os::Process remote_process,
                         absl::Span<IpczHandle> initial_portals);
  std::pair<mem::Ref<Portal>, mem::Ref<Portal>> OpenPortals();

 private:
  ~Node() override;

  bool AddLink(const NodeName& remote_node_name, mem::Ref<NodeLink> link);

  const NodeName name_{NodeName::kRandom};
  const Type type_;
  const IpczDriver driver_;
  const IpczDriverHandle driver_node_;

  absl::Mutex mutex_;
  absl::flat_hash_map<NodeName, mem::Ref<NodeLink>> node_links_
      ABSL_GUARDED_BY(mutex_);
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NODE_H_
