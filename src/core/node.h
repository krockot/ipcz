// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_NODE_H_
#define IPCZ_SRC_CORE_NODE_H_

#include "core/node_name.h"
#include "core/transport.h"
#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/memory.h"
#include "os/process.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_map.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

class NodeLink;
class Portal;

// Node encompasses the state of an isolated ipcz node.
class Node : public mem::RefCounted {
 public:
  enum class Type {
    kNormal,
    kBroker,
  };

  Node(Type type, const IpczDriver& driver, IpczDriverHandle driver_node);

  NodeName GetName() {
    absl::MutexLock lock(&mutex_);
    return name_;
  }

  const IpczDriver& driver() const { return driver_; }
  bool is_broker() const { return type_ == Type::kBroker; }

  mem::Ref<NodeLink> GetBrokerLink();
  mem::Ref<NodeLink> GetLink(const NodeName& name);
  bool AddLink(const NodeName& name, const mem::Ref<NodeLink>& link);

  void EstablishLink(
      const NodeName& name,
      std::function<void(const mem::Ref<NodeLink>& link)> callback);

  IpczResult CreateTransports(Transport::Descriptor& first,
                              Transport::Descriptor& second);
  IpczResult DeserializeTransport(const os::Process& remote_process,
                                  Transport::Descriptor& descriptor,
                                  IpczDriverHandle* driver_transport);

  IpczResult ConnectNode(IpczDriverHandle driver_handle,
                         Type remote_node_type,
                         os::Process process,
                         absl::Span<IpczHandle> initial_portals);

  std::pair<mem::Ref<Portal>, mem::Ref<Portal>> OpenPortals();

  bool AcceptInvitationFromBroker(const NodeName& broker_name,
                                  const NodeName& our_name);

  void ShutDown();

 private:
  ~Node() override;

  const Type type_;
  const IpczDriver driver_;
  const IpczDriverHandle driver_node_;

  absl::Mutex mutex_;
  NodeName name_ ABSL_GUARDED_BY(mutex_);
  mem::Ref<NodeLink> broker_link_ ABSL_GUARDED_BY(mutex_);
  absl::flat_hash_map<NodeName, mem::Ref<NodeLink>> node_links_
      ABSL_GUARDED_BY(mutex_);
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NODE_H_
