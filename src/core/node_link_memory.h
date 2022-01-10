// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_NODE_LINK_MEMORY_H_
#define IPCZ_SRC_CORE_NODE_LINK_MEMORY_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "core/buffer_id.h"
#include "core/driver_memory.h"
#include "core/driver_memory_mapping.h"
#include "core/node_link_address.h"
#include "core/routing_id.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_map.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

class Node;
class NodeLink;

// NodeLinkMemory owns and manages all shared memory resource allocation on a
// single NodeLink. Each end of a NodeLink has its own NodeLinkMemory instance
// cooperatively managing the same dynamic pool of memory, shared exclusively
// between the two endpoint nodes.
class NodeLinkMemory : public mem::RefCounted {
 public:
  NodeLinkMemory(NodeLinkMemory&&);

  static mem::Ref<NodeLinkMemory> Allocate(mem::Ref<Node> node,
                                           size_t num_initial_portals,
                                           DriverMemory& primary_buffer_memory);
  static mem::Ref<NodeLinkMemory> Adopt(mem::Ref<Node> node,
                                        DriverMemory primary_buffer_memory);

  // Sets a weak reference to a local NodeLink which shares ownership of this
  // NodeLinkMemory with some remote NodeLink. This must be reset to null when
  // `node_link` is deactivated.
  void SetNodeLink(mem::Ref<NodeLink> node_link);

  // Resolves a NodeLinkAddress (a buffer ID and offset) to a real memory
  // address mapped within the calling process. May return null if the given
  // NodeLinkAddress is not currently mapped in the calling process.
  void* GetMappedAddress(const NodeLinkAddress& address);

  // Helper for typed address mapping.
  template <typename T>
  T* GetMapped(const NodeLinkAddress& address) {
    return static_cast<T*>(GetMappedAddress(address));
  }

  // Returns the first of `count` newly allocated routing IDs for use on the
  // corresponding NodeLink.
  RoutingId AllocateRoutingIds(size_t count);

  // Returns the location of the RouterLinkState for the `i`th initial portal
  // on the NodeLink, as established by whatever Connect() call precipitated
  // the link's creation. Unlike other RouterLinkStates which are allocated
  // dynamically, these have a fixed location within the NodeLinkMemory's
  // primary buffer.
  NodeLinkAddress GetInitialRouterLinkState(size_t i);

  // Allocates a new RouterLinkState in NodeLink memory and returns its future
  // address. This is useful when constructing a new central RemoteRouterLink.
  // May return null if there is no more capacity to allocate new
  /// RouterLinkState instances.
  NodeLinkAddress AllocateRouterLinkState();

  // Requests allocation of additional capacity for this NodeLink memory.
  // `callback` is invoked once new capacity is available, which may require
  // some asynchronous work to accomplish.
  using CapacityCallback = std::function<void()>;
  void RequestCapacity(CapacityCallback callback);

  // Introduces a new buffer associated with BufferId. This BufferId must have
  // been allocated via AllocateBufferId() on this NodeLinkMemory or the
  // corresponding remote NodeLinkMemory associated with the same conceptual
  // link.
  void AddBuffer(BufferId id, DriverMemory memory);

  void OnBufferAvailable(BufferId id, std::function<void()> callback);

 private:
  ~NodeLinkMemory() override;

  DriverMemoryMapping& primary_buffer() { return buffers_.front(); }

  NodeLinkMemory(mem::Ref<Node> node, DriverMemoryMapping primary_buffer);

  BufferId AllocateBufferId();
  NodeLinkAddress AllocateUninitializedRouterLinkState();

  const mem::Ref<Node> node_;

  absl::Mutex mutex_;

  // The local NodeLink which shares ownership of this object. May be null if
  // the link has been deactivated and is set for destruction.
  mem::Ref<NodeLink> node_link_ ABSL_GUARDED_BY(mutex_);

  // List of all allocated buffers for this object. Once elements are appended
  // to this list, they remain indefinitely. The head of the list is initialized
  // at construction time and is therefore always stable, so its read access is
  // not guarded by `mutex_` (hence no annotation). All other accesses must be
  // guarded.
  std::list<DriverMemoryMapping> buffers_;

  // Indicates whether a request is already in flight for new memory capacity.
  bool awaiting_capacity_ ABSL_GUARDED_BY(mutex_) = false;

  // Callbacks to invoke when the current pending capacity request is fulfilled.
  std::vector<CapacityCallback> capacity_callbacks_ ABSL_GUARDED_BY(mutex_);

  // Mapping from BufferId to some buffer in `buffers_` above.
  absl::flat_hash_map<BufferId, DriverMemoryMapping*> buffer_map_
      ABSL_GUARDED_BY(mutex_);

  // Callbacks to be invoked when an identified buffer becomes available.
  absl::flat_hash_map<BufferId, std::vector<std::function<void()>>>
      buffer_callbacks_ ABSL_GUARDED_BY(mutex_);
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NODE_LINK_MEMORY_H_
