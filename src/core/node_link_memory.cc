// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/node_link_memory.h"

#include <array>
#include <atomic>
#include <cstdint>

#include "core/node.h"
#include "core/node_link.h"
#include "core/router_link_state.h"
#include "ipcz/ipcz.h"
#include "mem/block_allocator.h"
#include "mem/mpsc_queue.h"
#include "third_party/abseil-cpp/absl/numeric/bits.h"

namespace ipcz {
namespace core {

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
  std::atomic<uint64_t> next_router_link_state_index{0};
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
  std::array<uint8_t, 2048> mem_for_a_to_b_message_queue;
  std::array<uint8_t, 2048> mem_for_b_to_a_message_queue;
  std::array<uint8_t, 27648> mem_for_64_byte_fragments;
  std::array<uint8_t, 32768> mem_for_256_byte_fragments;

  mem::MpscQueue<FragmentDescriptor> a_to_b_message_queue() {
    return mem::MpscQueue<FragmentDescriptor>(
        absl::MakeSpan(mem_for_a_to_b_message_queue));
  }

  mem::MpscQueue<FragmentDescriptor> b_to_a_message_queue() {
    return mem::MpscQueue<FragmentDescriptor>(
        absl::MakeSpan(mem_for_b_to_a_message_queue));
  }

  mem::BlockAllocator block_allocator_64() {
    return mem::BlockAllocator(absl::MakeSpan(mem_for_64_byte_fragments), 64);
  }

  mem::BlockAllocator block_allocator_256() {
    return mem::BlockAllocator(absl::MakeSpan(mem_for_256_byte_fragments), 256);
  }
};

NodeLinkMemory::NodeLinkMemory(mem::Ref<Node> node,
                               DriverMemoryMapping primary_buffer_memory)
    : node_(std::move(node)) {
  buffers_.push_back(std::move(primary_buffer_memory));

  auto allocator_64 = std::make_unique<FragmentAllocator>(64);
  allocator_64->AddBlockAllocator(kPrimaryBufferId,
                                  primary_buffer_mapping().bytes(),
                                  primary_buffer().block_allocator_64());
  fragment_allocators_[64] = std::move(allocator_64);

  auto allocator_256 = std::make_unique<FragmentAllocator>(256);
  allocator_256->AddBlockAllocator(kPrimaryBufferId,
                                   primary_buffer_mapping().bytes(),
                                   primary_buffer().block_allocator_256());
  fragment_allocators_[256] = std::move(allocator_256);
}

NodeLinkMemory::~NodeLinkMemory() = default;

// static
mem::Ref<NodeLinkMemory> NodeLinkMemory::Allocate(
    mem::Ref<Node> node,
    size_t num_initial_portals,
    DriverMemory& primary_buffer_memory) {
  primary_buffer_memory = DriverMemory(node->driver(), sizeof(PrimaryBuffer));
  DriverMemoryMapping mapping(primary_buffer_memory.Map());

  auto memory = mem::WrapRefCounted(
      new NodeLinkMemory(std::move(node), std::move(mapping)));

  PrimaryBuffer& primary_buffer = memory->primary_buffer();
  primary_buffer.header.next_sublink = num_initial_portals;
  primary_buffer.header.next_buffer_id = 1;
  primary_buffer.header.next_router_link_state_index = num_initial_portals;
  primary_buffer.header.is_side_a_notification_pending.clear();
  primary_buffer.header.is_side_b_notification_pending.clear();

  primary_buffer.a_to_b_message_queue().InitializeRegion();
  primary_buffer.b_to_a_message_queue().InitializeRegion();
  primary_buffer.block_allocator_64().InitializeRegion();
  primary_buffer.block_allocator_256().InitializeRegion();
  return memory;
}

// static
mem::Ref<NodeLinkMemory> NodeLinkMemory::Adopt(
    mem::Ref<Node> node,
    DriverMemory primary_buffer_memory) {
  return mem::WrapRefCounted(
      new NodeLinkMemory(std::move(node), primary_buffer_memory.Map()));
}

void NodeLinkMemory::SetNodeLink(mem::Ref<NodeLink> node_link) {
  absl::MutexLock lock(&mutex_);
  node_link_ = std::move(node_link);
  if (!node_link_) {
    return;
  }

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
    ABSL_ASSERT(!buffers_.empty());
    if (descriptor.end() > buffers_.front().bytes().size()) {
      return {};
    }

    return Fragment(descriptor,
                    buffers_.front().address_at(descriptor.offset()));
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

  FragmentDescriptor descriptor(
      kPrimaryBufferId, ToOffset(state, primary_buffer_mapping().address()),
      sizeof(RouterLinkState));
  return FragmentRef<RouterLinkState>(RefCountedFragment::kUnmanagedRef,
                                      Fragment(descriptor, state));
}

FragmentRef<RouterLinkState> NodeLinkMemory::AllocateRouterLinkState() {
  // Ensure RouterLinkStates can be allocated as 64-byte fragments.
  static_assert(sizeof(RouterLinkState) == 64, "Invalid RouterLinkState size");

  Fragment fragment = AllocateFragment(sizeof(RouterLinkState));
  if (!fragment.is_null()) {
    RouterLinkState::Initialize(fragment.address());
    return FragmentRef<RouterLinkState>(mem::WrapRefCounted(this), fragment);
  }

  return {};
}

Fragment NodeLinkMemory::AllocateFragment(size_t num_bytes) {
  FragmentAllocator* allocator = GetFragmentAllocatorForSize(num_bytes);
  if (!allocator) {
    return {};
  }

  return allocator->Allocate();
}

void NodeLinkMemory::FreeFragment(const Fragment& fragment) {
  if (fragment.is_null()) {
    return;
  }

  FragmentAllocator* allocator = GetFragmentAllocatorForSize(fragment.size());
  if (allocator) {
    allocator->Free(fragment);
  }
}

void NodeLinkMemory::RequestFragmentCapacity(
    uint32_t buffer_size,
    uint32_t fragment_size,
    RequestFragmentCapacityCallback callback) {
  fragment_size = absl::bit_ceil(fragment_size);
  {
    absl::MutexLock lock(&mutex_);
    auto [it, need_new_request] =
        capacity_callbacks_.emplace(fragment_size, CapacityCallbackList());
    it->second.push_back(std::move(callback));
    if (!need_new_request) {
      return;
    }
  }

  node_->AllocateSharedMemory(
      buffer_size,
      [self = mem::WrapRefCounted(this), fragment_size](DriverMemory memory) {
        mem::Ref<NodeLink> link;
        const BufferId new_buffer_id = self->AllocateBufferId();
        CapacityCallbackList callbacks;
        {
          absl::MutexLock lock(&self->mutex_);
          self->buffers_.push_back(memory.Map());
          DriverMemoryMapping* mapping = &self->buffers_.back();
          self->buffer_map_[new_buffer_id] = mapping;
          auto it = self->capacity_callbacks_.find(fragment_size);
          if (it != self->capacity_callbacks_.end()) {
            callbacks = std::move(it->second);
            self->capacity_callbacks_.erase(it);
          }
          link = self->node_link_;

          mem::BlockAllocator block_allocator(mapping->bytes(), fragment_size);
          block_allocator.InitializeRegion();

          std::unique_ptr<FragmentAllocator>& allocator =
              self->fragment_allocators_[fragment_size];
          if (!allocator) {
            allocator = std::make_unique<FragmentAllocator>(fragment_size);
          }
          allocator->AddBlockAllocator(new_buffer_id, mapping->bytes(),
                                       block_allocator);
        }

        if (link) {
          link->AddFragmentAllocatorBuffer(new_buffer_id, fragment_size,
                                           std::move(memory));
        }

        for (auto& callback : callbacks) {
          callback();
        }
      });
}

bool NodeLinkMemory::AddFragmentAllocatorBuffer(BufferId id,
                                                uint32_t fragment_size,
                                                DriverMemory memory) {
  fragment_size = absl::bit_ceil(fragment_size);
  std::vector<std::function<void()>> buffer_callbacks;
  {
    absl::MutexLock lock(&mutex_);
    auto result = buffer_map_.emplace(id, nullptr);
    if (!result.second) {
      return false;
    }

    buffers_.push_back(memory.Map());
    DriverMemoryMapping& mapping = buffers_.back();
    result.first->second = &mapping;

    mem::BlockAllocator block_allocator(mapping.bytes(), fragment_size);
    std::unique_ptr<FragmentAllocator>& allocator =
        fragment_allocators_[fragment_size];
    if (!allocator) {
      allocator = std::make_unique<FragmentAllocator>(fragment_size);
    }
    allocator->AddBlockAllocator(id, mapping.bytes(), block_allocator);

    auto it = buffer_callbacks_.find(id);
    if (it != buffer_callbacks_.end()) {
      std::swap(it->second, buffer_callbacks);
      buffer_callbacks_.erase(it);
    }
  }

  for (std::function<void()>& callback : buffer_callbacks) {
    callback();
  }

  return true;
}

bool NodeLinkMemory::TestAndSetNotificationPending() {
  if (!outgoing_notification_flag_) {
    return false;
  }

  return outgoing_notification_flag_->test_and_set(
      std::memory_order_relaxed);
}

void NodeLinkMemory::ClearPendingNotification() {
  if (!incoming_notification_flag_) {
    return;
  }

  incoming_notification_flag_->clear();
}

void NodeLinkMemory::OnBufferAvailable(BufferId id,
                                       std::function<void()> callback) {
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

FragmentAllocator* NodeLinkMemory::GetFragmentAllocatorForSize(
    size_t num_bytes) {
  absl::MutexLock lock(&mutex_);
  auto it = fragment_allocators_.find(absl::bit_ceil(num_bytes));
  if (it == fragment_allocators_.end()) {
    return nullptr;
  }
  return it->second.get();
}

}  // namespace core
}  // namespace ipcz
