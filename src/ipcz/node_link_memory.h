// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_NODE_LINK_MEMORY_H_
#define IPCZ_SRC_IPCZ_NODE_LINK_MEMORY_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "ipcz/buffer_id.h"
#include "ipcz/driver_memory.h"
#include "ipcz/driver_memory_mapping.h"
#include "ipcz/fragment.h"
#include "ipcz/fragment_allocator.h"
#include "ipcz/fragment_descriptor.h"
#include "ipcz/fragment_ref.h"
#include "ipcz/ipcz.h"
#include "ipcz/router_link_state.h"
#include "ipcz/sequence_number.h"
#include "ipcz/sublink_id.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_map.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/mpsc_queue.h"
#include "util/ref_counted.h"

namespace ipcz {

class Node;
class NodeLink;

// NodeLinkMemory owns and manages all shared memory resource allocation on a
// single NodeLink. Each end of a NodeLink has its own NodeLinkMemory instance
// cooperatively managing the same dynamic pool of memory, shared exclusively
// between the two endpoint nodes.
class NodeLinkMemory : public RefCounted {
 public:
  struct MessageFragment {
    MessageFragment();
    MessageFragment(SequenceNumber sequence_number,
                    const FragmentDescriptor& descriptor);
    ~MessageFragment();
    SequenceNumber sequence_number;
    FragmentDescriptor descriptor;
  };

  NodeLinkMemory(NodeLinkMemory&&);

  MpscQueue<MessageFragment>& incoming_message_fragments() {
    return incoming_message_fragments_;
  }

  MpscQueue<MessageFragment> outgoing_message_fragments() {
    return outgoing_message_fragments_;
  }

  FragmentAllocator& fragment_allocator() { return fragment_allocator_; }

  static Ref<NodeLinkMemory> Allocate(Ref<Node> node,
                                      size_t num_initial_portals,
                                      DriverMemory& primary_buffer_memory);
  static Ref<NodeLinkMemory> Adopt(Ref<Node> node,
                                   DriverMemory primary_buffer_memory);

  // Sets a weak reference to a local NodeLink which shares ownership of this
  // NodeLinkMemory with some remote NodeLink. This must be reset to null when
  // `node_link` is deactivated.
  void SetNodeLink(Ref<NodeLink> node_link);

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
                          WrapRefCounted(this), GetFragment(descriptor));
  }

  // Returns the first of `count` newly allocated, contiguous sublink IDs for
  // use on the corresponding NodeLink.
  SublinkId AllocateSublinkIds(size_t count);

  // Returns a ref to the RouterLinkState for the `i`th initial portal on the
  // NodeLink, established by the Connect() call which created this link. Unlike
  // Unlike other RouterLinkStates which are allocated dynamically, these have a
  // fixed location within the NodeLinkMemory's primary buffer. The returned
  // FragmentRef is unmanaged and will never free its underlying fragment.
  FragmentRef<RouterLinkState> GetInitialRouterLinkState(size_t i);

  // Allocates a new ref-counted RouterLinkState in NodeLink memory and returns
  // a ref to its fragment. This is useful when constructing a new central
  // RemoteRouterLink.
  //
  // May return a null ref if there is no more capacity to allocate new
  // RouterLinkState instances.
  FragmentRef<RouterLinkState> AllocateRouterLinkState();

  // Same as above, but may complete asynchronously.
  using RouterLinkStateCallback =
      std::function<void(FragmentRef<RouterLinkState>)>;
  void AllocateRouterLinkStateAsync(RouterLinkStateCallback callback);

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
  // not there was already a notification pending. This is used as a signal to
  // avoid redundant flushes of the link when multiple messages are sent close
  // together.
  bool TestAndSetNotificationPending();

  // Clears this side's pending notification flag. Once cleared, any subsequent
  // message sent by the other side will elicit at least one additional flush
  // message to this side via the driver transport.
  void ClearPendingNotification();

  // Requests allocation of a new buffer of the given size. When completed,
  // `allocate_callback` is invoked with the memory object, a mapping (owned by
  // this NodeLinkMemory), and the ID of the new buffer. The buffer is NOT
  // automatically shared across the link. Instead, the caller should do that
  // along with whatever context is necessary for the other side to use the
  // buffer as expected.
  //
  // On success only, `share_callback` is run BEFORE the buffer is registered
  // with this node so that the caller may initialize and share the buffer with
  // the remote node as needed before it can be used for any work by the local
  // node. The buffer is registered locally to the given BufferId as soon as
  // `share_callback` returns.
  //
  // Once the buffer is registered -- or if allocation failed --
  // `finished_callback` is run to indicate success or failure. At this point
  // (assuming success) it's safe to perform operations which rely on the
  // buffer's registration being complete, such as sending other messages which
  // reference the buffer's contents.
  using AllocateBufferShareCallback =
      std::function<void(BufferId, DriverMemory, DriverMemoryMapping&)>;
  using AllocateBufferFinishedCallback = std::function<void(bool)>;
  void AllocateBuffer(size_t num_bytes,
                      AllocateBufferShareCallback share_callback,
                      AllocateBufferFinishedCallback finished_callback);

  // Registers a callback to be invoked as soon as the identified buffer becomes
  // available to this NodeLinkMemory.
  void OnBufferAvailable(BufferId id, std::function<void()> callback);

 private:
  struct PrimaryBuffer;

  ~NodeLinkMemory() override;

  PrimaryBuffer& primary_buffer() {
    return *static_cast<PrimaryBuffer*>(primary_buffer_.address());
  }

  NodeLinkMemory(Ref<Node> node, DriverMemoryMapping primary_buffer);

  BufferId AllocateBufferId();

  const Ref<Node> node_;

  absl::Mutex mutex_;

  // The local NodeLink which shares ownership of this object. May be null if
  // the link has been deactivated and is set for destruction.
  Ref<NodeLink> node_link_ ABSL_GUARDED_BY(mutex_);

  // Mapping for this link's fixed primary buffer.
  const DriverMemoryMapping primary_buffer_;

  // List of all allocated buffers for this object, except the primary buffer.
  // Once elements are appended to this list, they remain indefinitely.
  std::list<DriverMemoryMapping> buffers_ ABSL_GUARDED_BY(mutex_);

  // Handles dynamic allocation of most shared memory chunks used by this
  // NodeLinkMemory.
  FragmentAllocator fragment_allocator_;

  // Message queues mapped from this NodeLinkMemory's primary buffer. These are
  // used as a lightweight medium to convey small data-only messages.
  MpscQueue<MessageFragment> incoming_message_fragments_;
  MpscQueue<MessageFragment> outgoing_message_fragments_;
  std::atomic_flag* incoming_notification_flag_;
  std::atomic_flag* outgoing_notification_flag_;

  // Mapping from BufferId to some buffer in `buffers_` above.
  absl::flat_hash_map<BufferId, DriverMemoryMapping*> buffer_map_
      ABSL_GUARDED_BY(mutex_);

  // Callbacks to be invoked when an identified buffer becomes available.
  absl::flat_hash_map<BufferId, std::vector<std::function<void()>>>
      buffer_callbacks_ ABSL_GUARDED_BY(mutex_);
};

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_NODE_LINK_MEMORY_H_
