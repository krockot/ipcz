// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/node_link_memory.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "ipcz/block_allocator_pool.h"
#include "ipcz/ipcz.h"
#include "ipcz/node.h"
#include "ipcz/node_link.h"
#include "ipcz/router_link_state.h"
#include "third_party/abseil-cpp/absl/numeric/bits.h"
#include "util/block_allocator.h"
#include "util/function.h"
#include "util/mpsc_queue.h"

namespace ipcz {

namespace {

constexpr BufferId kPrimaryBufferId = 0;

// The front of the primary buffer is reserved for special uses which require
// synchronous availability throughout the link's lifetime.
constexpr size_t kPrimaryBufferReservedHeaderSize = 256;

// Number of fixed RouterLinkState locations in the primary buffer. This limits
// the maximum number of initial portals supported by the ConnectNode() API.
// Note that these states reside in a fixed location at the end of the reserved
// block.
using InitialRouterLinkStateArray = std::array<RouterLinkState, 12>;
static_assert(sizeof(InitialRouterLinkStateArray) == 768,
              "Invalid InitialRouterLinkStateArray size");

// This structure always sits at offset 0 in the primary buffer.
struct IPCZ_ALIGN(8) PrimaryBufferHeader {
  std::atomic<uint64_t> next_sublink{0};
  std::atomic<uint64_t> next_buffer_id{1};
  std::atomic_flag is_side_a_notification_pending;
  std::atomic_flag is_side_b_notification_pending;
};

constexpr size_t kPrimaryBufferHeaderPaddingSize =
    kPrimaryBufferReservedHeaderSize - sizeof(PrimaryBufferHeader);

uint32_t ToOffset(void* ptr, void* base) {
  return static_cast<uint32_t>(static_cast<uint8_t*>(ptr) -
                               static_cast<uint8_t*>(base));
}

}  // namespace

struct IPCZ_ALIGN(8) NodeLinkMemory::PrimaryBuffer {
  PrimaryBufferHeader header;
  uint8_t reserved_header_padding[kPrimaryBufferHeaderPaddingSize];
  InitialRouterLinkStateArray initial_link_states;
  std::array<uint8_t, 512> mem_for_a_to_b_message_queue;
  std::array<uint8_t, 512> mem_for_b_to_a_message_queue;
  std::array<uint8_t, 16384> mem_for_256_byte_fragments;
  std::array<uint8_t, 16384> mem_for_512_byte_fragments;
  std::array<uint8_t, 11264> mem_for_1024_byte_fragments;
  std::array<uint8_t, 16384> mem_for_2048_byte_fragments;

  MpscQueue<FragmentDescriptor> a_to_b_message_queue() {
    return MpscQueue<FragmentDescriptor>(
        absl::MakeSpan(mem_for_a_to_b_message_queue));
  }

  MpscQueue<FragmentDescriptor> b_to_a_message_queue() {
    return MpscQueue<FragmentDescriptor>(
        absl::MakeSpan(mem_for_b_to_a_message_queue));
  }

  BlockAllocator block_allocator_256() {
    return BlockAllocator(absl::MakeSpan(mem_for_256_byte_fragments), 256);
  }

  BlockAllocator block_allocator_512() {
    return BlockAllocator(absl::MakeSpan(mem_for_512_byte_fragments), 512);
  }

  BlockAllocator block_allocator_1024() {
    return BlockAllocator(absl::MakeSpan(mem_for_1024_byte_fragments), 1024);
  }

  BlockAllocator block_allocator_2048() {
    return BlockAllocator(absl::MakeSpan(mem_for_2048_byte_fragments), 2048);
  }
};

NodeLinkMemory::NodeLinkMemory(Ref<Node> node,
                               DriverMemoryMapping primary_buffer_memory)
    : node_(std::move(node)),
      primary_buffer_(std::move(primary_buffer_memory)) {
  auto bytes = primary_buffer_.bytes();
  fragment_allocator_.AddBlockAllocator(256, kPrimaryBufferId, bytes,
                                        primary_buffer().block_allocator_256());
  fragment_allocator_.AddBlockAllocator(512, kPrimaryBufferId, bytes,
                                        primary_buffer().block_allocator_512());
  fragment_allocator_.AddBlockAllocator(
      1024, kPrimaryBufferId, bytes, primary_buffer().block_allocator_1024());
  fragment_allocator_.AddBlockAllocator(
      2048, kPrimaryBufferId, bytes, primary_buffer().block_allocator_2048());
}

NodeLinkMemory::~NodeLinkMemory() = default;

// static
Ref<NodeLinkMemory> NodeLinkMemory::Allocate(
    Ref<Node> node,
    size_t num_initial_portals,
    DriverMemory& primary_buffer_memory) {
  primary_buffer_memory = DriverMemory(node, sizeof(PrimaryBuffer));
  DriverMemoryMapping mapping(primary_buffer_memory.Map());

  auto memory =
      WrapRefCounted(new NodeLinkMemory(std::move(node), std::move(mapping)));

  PrimaryBuffer& primary_buffer = memory->primary_buffer();
  primary_buffer.header.next_sublink = num_initial_portals;
  primary_buffer.header.next_buffer_id = 1;
  primary_buffer.header.is_side_a_notification_pending.clear();
  primary_buffer.header.is_side_b_notification_pending.clear();

  primary_buffer.a_to_b_message_queue().InitializeRegion();
  primary_buffer.b_to_a_message_queue().InitializeRegion();
  primary_buffer.block_allocator_256().InitializeRegion();
  primary_buffer.block_allocator_512().InitializeRegion();
  primary_buffer.block_allocator_1024().InitializeRegion();
  primary_buffer.block_allocator_2048().InitializeRegion();
  return memory;
}

// static
Ref<NodeLinkMemory> NodeLinkMemory::Adopt(Ref<Node> node,
                                          DriverMemory primary_buffer_memory) {
  return WrapRefCounted(
      new NodeLinkMemory(std::move(node), primary_buffer_memory.Map()));
}

void NodeLinkMemory::SetNodeLink(Ref<NodeLink> node_link) {
  absl::MutexLock lock(&mutex_);
  node_link_ = std::move(node_link);
  if (!node_link_) {
    return;
  }

  fragment_allocator_.SetNodeLink(node_link_);
  if (node_link_->link_side().is_side_a()) {
    incoming_message_fragments_ = primary_buffer().b_to_a_message_queue();
    outgoing_message_fragments_ = primary_buffer().a_to_b_message_queue();
    incoming_notification_flag_ =
        &primary_buffer().header.is_side_a_notification_pending;
    outgoing_notification_flag_ =
        &primary_buffer().header.is_side_b_notification_pending;
  } else {
    incoming_message_fragments_ = primary_buffer().a_to_b_message_queue();
    outgoing_message_fragments_ = primary_buffer().b_to_a_message_queue();
    incoming_notification_flag_ =
        &primary_buffer().header.is_side_b_notification_pending;
    outgoing_notification_flag_ =
        &primary_buffer().header.is_side_a_notification_pending;
  }
}

Fragment NodeLinkMemory::GetFragment(const FragmentDescriptor& descriptor) {
  if (descriptor.is_null()) {
    return {};
  }

  if (descriptor.buffer_id() == kPrimaryBufferId) {
    // Fast path for primary buffer access.
    if (descriptor.end() > primary_buffer_.bytes().size()) {
      return {};
    }

    return Fragment(descriptor,
                    primary_buffer_.address_at(descriptor.offset()));
  }

  absl::MutexLock lock(&mutex_);
  auto it = buffer_map_.find(descriptor.buffer_id());
  if (it == buffer_map_.end()) {
    return Fragment(descriptor, nullptr);
  }

  auto& [id, mapping] = *it;
  if (descriptor.end() > mapping->bytes().size()) {
    return {};
  }

  return Fragment(descriptor, mapping->address_at(descriptor.offset()));
}

SublinkId NodeLinkMemory::AllocateSublinkIds(size_t count) {
  return primary_buffer().header.next_sublink.fetch_add(
      count, std::memory_order_relaxed);
}

FragmentRef<RouterLinkState> NodeLinkMemory::GetInitialRouterLinkState(
    size_t i) {
  auto& states = primary_buffer().initial_link_states;
  ABSL_ASSERT(i < states.size());
  RouterLinkState* state = &states[i];

  FragmentDescriptor descriptor(kPrimaryBufferId,
                                ToOffset(state, primary_buffer_.address()),
                                sizeof(RouterLinkState));
  return FragmentRef<RouterLinkState>(RefCountedFragment::kUnmanagedRef,
                                      Fragment(descriptor, state));
}

FragmentRef<RouterLinkState> NodeLinkMemory::AllocateRouterLinkState() {
  // Ensure RouterLinkStates can be allocated as 64-byte fragments.
  static_assert(sizeof(RouterLinkState) == 64, "Invalid RouterLinkState size");

  Fragment fragment = fragment_allocator_.Allocate(sizeof(RouterLinkState));
  if (!fragment.is_null()) {
    RouterLinkState::Initialize(fragment.address());
    return FragmentRef<RouterLinkState>(WrapRefCounted(this), fragment);
  }

  return {};
}

void NodeLinkMemory::AllocateRouterLinkStateAsync(
    RouterLinkStateCallback callback) {
  fragment_allocator_.AllocateAsync(
      sizeof(RouterLinkState),
      [self = WrapRefCounted(this),
       callback = std::move(callback)](Fragment fragment) {
        if (fragment.is_null()) {
          callback({});
          return;
        }

        RouterLinkState::Initialize(fragment.address());
        callback(FragmentRef<RouterLinkState>(std::move(self), fragment));
        return;
      });
}

bool NodeLinkMemory::AddFragmentAllocatorBuffer(BufferId id,
                                                uint32_t fragment_size,
                                                DriverMemory memory) {
  fragment_size = absl::bit_ceil(fragment_size);
  std::vector<Function<void()>> buffer_callbacks;
  {
    absl::MutexLock lock(&mutex_);
    auto result = buffer_map_.emplace(id, nullptr);
    if (!result.second) {
      return false;
    }

    buffers_.push_back(memory.Map());
    DriverMemoryMapping& mapping = buffers_.back();
    result.first->second = &mapping;

    BlockAllocator block_allocator(mapping.bytes(), fragment_size);
    fragment_allocator_.AddBlockAllocator(fragment_size, id, mapping.bytes(),
                                          block_allocator);

    auto it = buffer_callbacks_.find(id);
    if (it != buffer_callbacks_.end()) {
      std::swap(it->second, buffer_callbacks);
      buffer_callbacks_.erase(it);
    }
  }

  for (Function<void()>& callback : buffer_callbacks) {
    callback();
  }

  return true;
}

bool NodeLinkMemory::TestAndSetNotificationPending() {
  if (!outgoing_notification_flag_) {
    return false;
  }

  return outgoing_notification_flag_->test_and_set(std::memory_order_relaxed);
}

void NodeLinkMemory::ClearPendingNotification() {
  if (!incoming_notification_flag_) {
    return;
  }

  incoming_notification_flag_->clear();
}

void NodeLinkMemory::AllocateBuffer(size_t num_bytes,
                                    AllocateBufferCallback callback) {
  node_->AllocateSharedMemory(
      num_bytes, [self = WrapRefCounted(this),
                  callback = std::move(callback)](DriverMemory memory) {
        if (!memory.is_valid()) {
          DriverMemoryMapping invalid_mapping;
          callback(0, DriverMemory(), invalid_mapping);
          return;
        }

        const BufferId new_buffer_id = self->AllocateBufferId();
        DriverMemoryMapping* mapping;
        {
          absl::MutexLock lock(&self->mutex_);
          self->buffers_.push_back(memory.Map());
          mapping = &self->buffers_.back();
          self->buffer_map_[new_buffer_id] = mapping;
        }
        callback(new_buffer_id, std::move(memory), *mapping);
      });
}

void NodeLinkMemory::OnBufferAvailable(BufferId id, Function<void()> callback) {
  {
    absl::MutexLock lock(&mutex_);
    auto it = buffer_map_.find(id);
    if (it == buffer_map_.end()) {
      buffer_callbacks_[id].push_back(std::move(callback));
      return;
    }
  }

  callback();
}

BufferId NodeLinkMemory::AllocateBufferId() {
  return primary_buffer().header.next_buffer_id.fetch_add(
      1, std::memory_order_relaxed);
}

}  // namespace ipcz
