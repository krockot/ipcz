// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_NODE_LINK_MEMORY_H_
#define IPCZ_SRC_CORE_NODE_LINK_MEMORY_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/buffer_id.h"
#include "core/driver_memory.h"
#include "core/driver_memory_mapping.h"
#include "core/fragment.h"
#include "core/fragment_allocator.h"
#include "core/fragment_descriptor.h"
#include "core/fragment_ref.h"
#include "core/router_link_state.h"
#include "core/sublink_id.h"
#include "mem/mpsc_queue.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_map.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/function.h"

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

  mem::MpscQueue<FragmentDescriptor>& incoming_message_fragments() {
    return incoming_message_fragments_;
  }

  mem::MpscQueue<FragmentDescriptor> outgoing_message_fragments() {
    return outgoing_message_fragments_;
  }

  static mem::Ref<NodeLinkMemory> Allocate(mem::Ref<Node> node,
                                           size_t num_initial_portals,
                                           DriverMemory& primary_buffer_memory);
  static mem::Ref<NodeLinkMemory> Adopt(mem::Ref<Node> node,
                                        DriverMemory primary_buffer_memory);

  // Sets a weak reference to a local NodeLink which shares ownership of this
  // NodeLinkMemory with some remote NodeLink. This must be reset to null when
  // `node_link` is deactivated.
  void SetNodeLink(mem::Ref<NodeLink> node_link);

  // Resolves a FragmentDescriptor (a buffer ID and offset) to a real memory
  // span mapped within the calling process. May return null if the given
  // FragmentDescriptor is not currently mapped in the calling process.
  Fragment GetFragment(const FragmentDescriptor& descriptor);

  // Simliar to GetFragment() but resolves to a specific subclass of
  // RefCountedFragment and returns a ref to it.
  //
  // This does not increment the ref count of the RefCountedFragment, but
  // instead adopts a ref implied by the descriptor.
  template <typename T>
  FragmentRef<T> AdoptFragmentRef(const FragmentDescriptor& descriptor) {
    return FragmentRef<T>(RefCountedFragment::kAdoptExistingRef,
                          mem::WrapRefCounted(this), GetFragment(descriptor));
  }

  // Returns the first of `count` newly allocated, contiguous sublink IDs for
  // use on the corresponding NodeLink.
  SublinkId AllocateSublinkIds(size_t count);

  // Returns the a ref to the RouterLinkState for the `i`th initial portal on
  // the NodeLink, as established by whatever Connect() call precipitated the
  // link's creation. Unlike other RouterLinkStates which are allocated
  // dynamically, these have a fixed location within the NodeLinkMemory's
  // primary buffer. The returned FragmentRef is unmanaged and will never free
  // its underlying fragment.
  FragmentRef<RouterLinkState> GetInitialRouterLinkState(size_t i);

  // Allocates a new ref-counted RouterLinkState in NodeLink memory and returns
  // a ref to its fragment. This is useful when constructing a new central
  // RemoteRouterLink.
  //
  // May return a null ref if there is no more capacity to allocate new
  // RouterLinkState instances.
  FragmentRef<RouterLinkState> AllocateRouterLinkState();

  // Allocates a fragment of shared memory of the given size or of the smallest
  // sufficient size available to this object. If no memory is available to
  // allocate the fragment, this returns a null fragment.
  Fragment AllocateFragment(size_t num_bytes);

  // Frees a fragment allocated by AllocateFragment() or other allocation
  // helpers on this object.
  void FreeFragment(const Fragment& fragment);

  // Requests allocation of additional fragment allocation capacity for this
  // NodeLinkMemory, in the form of a single new buffer of `size` bytes in which
  // fragments of `fragment_size` bytes will be allocated.
  //
  // `callback` is invoked once new buffer is available, which may require some
  // asynchronous work to accomplish.
  using RequestFragmentCapacityCallback = Function<void()>;
  void RequestFragmentCapacity(uint32_t buffer_size,
                               uint32_t fragment_size,
                               RequestFragmentCapacityCallback callback);

  // Introduces a new buffer associated with BufferId, for use as a fragment
  // allocator with fragments of size `fragment_size`. `id` must have been
  // allocated via AllocateBufferId() on this NodeLinkMemory or the
  // corresponding remote NodeLinkMemory on the same link.
  //
  // Returns true if successful, or false if the NodeLinkMemory already had a
  // buffer identified by `id`.
  bool AddFragmentAllocatorBuffer(BufferId id,
                                  uint32_t fragment_size,
                                  DriverMemory memory);

  // Flags the other endpoint with a pending notification and returns whether or
  // not there was already a notification pending.
  bool TestAndSetNotificationPending();
  void ClearPendingNotification();

  void OnBufferAvailable(BufferId id, Function<void()> callback);

 private:
  struct PrimaryBuffer;

  ~NodeLinkMemory() override;

  DriverMemoryMapping& primary_buffer_mapping() { return buffers_.front(); }

  PrimaryBuffer& primary_buffer() {
    return *static_cast<PrimaryBuffer*>(primary_buffer_mapping().address());
  }

  NodeLinkMemory(mem::Ref<Node> node, DriverMemoryMapping primary_buffer);

  BufferId AllocateBufferId();

  FragmentAllocator* GetFragmentAllocatorForSize(size_t num_bytes);

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

  // FragmentAllocators grouped by fragment size. Note that each allocator is
  // stored indirectly on the heap, and elements must never be removed from this
  // map. These constraints ensure a stable memory location for each allocator
  // throughout the lifetime of the NodeLinkMemory.
  absl::flat_hash_map<uint32_t, std::unique_ptr<FragmentAllocator>>
      fragment_allocators_ ABSL_GUARDED_BY(mutex_);

  // Message queues mapped from this NodeLinkMemory's primary buffer. These are
  // used as a lightweight medium to convey small data-only messages.
  mem::MpscQueue<FragmentDescriptor> incoming_message_fragments_;
  mem::MpscQueue<FragmentDescriptor> outgoing_message_fragments_;
  std::atomic_flag* incoming_notification_flag_;
  std::atomic_flag* outgoing_notification_flag_;

  // Callbacks to invoke when a pending capacity request is fulfilled for a
  // specific fragment size.
  using CapacityCallbackList = std::vector<RequestFragmentCapacityCallback>;
  absl::flat_hash_map<uint32_t, CapacityCallbackList> capacity_callbacks_
      ABSL_GUARDED_BY(mutex_);

  // Mapping from BufferId to some buffer in `buffers_` above.
  absl::flat_hash_map<BufferId, DriverMemoryMapping*> buffer_map_
      ABSL_GUARDED_BY(mutex_);

  // Callbacks to be invoked when an identified buffer becomes available.
  absl::flat_hash_map<BufferId, std::vector<Function<void()>>> buffer_callbacks_
      ABSL_GUARDED_BY(mutex_);
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NODE_LINK_MEMORY_H_
