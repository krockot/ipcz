// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_NODE_LINK_H_
#define IPCZ_SRC_CORE_NODE_LINK_H_

#include <cstddef>
#include <cstdint>

#include "core/driver_transport.h"
#include "core/node_messages.h"
#include "core/node_name.h"
#include "core/routing_id.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "os/memory.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_map.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

class Node;
struct NodeLinkBuffer;
class Router;
class RouterLink;

class NodeLink : public mem::RefCounted, private DriverTransport::Listener {
 public:
  NodeLink(mem::Ref<Node> node,
           const NodeName& remote_node_name,
           uint32_t remote_protocol_version,
           mem::Ref<DriverTransport> transport,
           os::Memory::Mapping link_memory);

  const NodeName& remote_node_name() const { return remote_node_name_; }
  uint32_t remote_protocol_version() const { return remote_protocol_version_; }
  const mem::Ref<DriverTransport>& transport() const { return transport_; }
  NodeLinkBuffer& buffer() const { return *link_memory_.As<NodeLinkBuffer>(); }

  RoutingId AllocateRoutingIds(size_t count);

  mem::Ref<RouterLink> AddRoute(RoutingId routing_id,
                                size_t link_state_index,
                                mem::Ref<Router> router);

  void Deactivate();

  void Transmit(absl::Span<const uint8_t> data, absl::Span<os::Handle> handles);

  template <typename T>
  void Transmit(T& message) {
    transport_->Transmit(message);
  }

 private:
  ~NodeLink() override;

  mem::Ref<Router> GetRouter(RoutingId routing_id);

  // DriverTransport::Listener:
  IpczResult OnTransportMessage(
      const DriverTransport::Message& message) override;
  void OnTransportError() override;

  bool OnAcceptParcel(const DriverTransport::Message& message);
  bool OnSideClosed(const msg::SideClosed& side_closed);

  const mem::Ref<Node> node_;
  const NodeName remote_node_name_;
  const uint32_t remote_protocol_version_;
  const mem::Ref<DriverTransport> transport_;
  const os::Memory::Mapping link_memory_;

  absl::Mutex mutex_;
  bool active_ = true;
  absl::flat_hash_map<RoutingId, mem::Ref<Router>> routes_
      ABSL_GUARDED_BY(mutex_);
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NODE_LINK_H_
