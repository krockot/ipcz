// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_NODE_LINK_MEMORY_H_
#define IPCZ_SRC_CORE_NODE_LINK_MEMORY_H_

#include <atomic>
#include <cstddef>
#include <functional>
#include <vector>

#include "core/buffer_id.h"
#include "core/node_link_address.h"
#include "core/routing_id.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "os/memory.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_map.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace ipcz {
namespace core {

// NodeLinkMemory owns and manages all shared memory resource allocation on a
// single NodeLink. Each end of a NodeLink has its own NodeLinkMemory instance
// cooperatively managing the same dynamic pool of memory, shared exclusively
// between the two endpoint nodes/
class NodeLinkMemory : public mem::RefCounted {
 public:
  NodeLinkMemory(NodeLinkMemory&&);

  static mem::Ref<NodeLinkMemory> Allocate(size_t num_initial_portals,
                                           os::Memory& primary_buffer_memory);
  static mem::Ref<NodeLinkMemory> Adopt(
      os::Memory::Mapping primary_buffer_mapping);
  static mem::Ref<NodeLinkMemory> Adopt(os::Handle primary_buffer_handle);

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
  absl::optional<NodeLinkAddress> AllocateRouterLinkState();

  // Requests allocation of additional capacity for this NodeLink memory.
  // `callback` is invoked once new capacity is available, which may require
  // some asynchronous work to accomplish.
  using CapacityCallback = std::function<void()>;
  void RequestCapacity(CapacityCallback callback);

 private:
  ~NodeLinkMemory() override;

  os::Memory::Mapping& primary_buffer() { return buffers_.front(); }

  explicit NodeLinkMemory(os::Memory::Mapping primary_buffer);

  absl::optional<NodeLinkAddress> AllocateUninitializedRouterLinkState();

  absl::Mutex mutex_;

  // List of all allocated buffers for this object. Once elements are appended
  // to this list, they remain indefinitely. The head of the list is initialized
  // at construction time and is therefore always stable, so its read access is
  // not guarded by `mutex_` (hence no annotation). All other accesses must be
  // guarded.
  std::list<os::Memory::Mapping> buffers_;

  // Indicates whether a request is already in flight for new memory capacity.
  bool awaiting_capacity_ ABSL_GUARDED_BY(mutex_) = false;

  // Callbacks to invoke when the current pending capacity request is fulfilled.
  std::vector<CapacityCallback> capacity_callbacks_ ABSL_GUARDED_BY(mutex_);

  // Mapping from BufferId to some buffer in `buffers_` above.
  absl::flat_hash_map<BufferId, os::Memory::Mapping*> buffer_map_
      ABSL_GUARDED_BY(mutex_);
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NODE_LINK_MEMORY_H_
