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
#include "third_party/abseil-cpp/absl/numeric/bits.h"

namespace ipcz {
namespace core {

namespace {

constexpr BufferId kPrimaryBufferId = 0;
constexpr size_t kPrimaryBufferSize = 4096;

// The front of the primary buffer is reserved for special uses which require
// synchronous availability throughout the link's lifetime.
constexpr size_t kPrimaryBufferReservedBlockSize = 512;

// Number of fixed RouterLinkState locations in the primary buffer. This limits
// the maximum number of initial portals supported by the ConnectNode() API.
using InitialRouterLinkStateArray = std::array<RouterLinkState, 16>;

// This structure always sits at offset 0 in the primary buffer.
struct IPCZ_ALIGN(8) PrimaryBufferHeader {
  std::atomic<uint64_t> next_sublink{0};
  std::atomic<uint64_t> next_buffer_id{1};
  std::atomic<uint64_t> next_router_link_state_index{0};
};

constexpr size_t kPrimaryBufferReservedBlockPaddingSize =
    kPrimaryBufferReservedBlockSize - sizeof(InitialRouterLinkStateArray) -
    sizeof(PrimaryBufferHeader);

struct IPCZ_ALIGN(8) PrimaryBufferReservedBlock {
  PrimaryBufferHeader header;
  uint8_t reserved[kPrimaryBufferReservedBlockPaddingSize];
  InitialRouterLinkStateArray initial_link_states;
};
static_assert(sizeof(PrimaryBufferReservedBlock) ==
                  kPrimaryBufferReservedBlockSize,
              "Invalid PrimaryBufferReservedBlock size");

constexpr size_t kPrimaryBufferLinkAllocatorSize =
    kPrimaryBufferSize - sizeof(PrimaryBufferReservedBlock);

constexpr size_t kRouterLinkStateBlockSize = 32;
static_assert(kRouterLinkStateBlockSize >= sizeof(RouterLinkState),
              "Invalid RouterLinkState block size");

PrimaryBufferReservedBlock& ReservedBlock(const DriverMemoryMapping& mapping) {
  return *static_cast<PrimaryBufferReservedBlock*>(mapping.address());
}

PrimaryBufferHeader& Header(const DriverMemoryMapping& mapping) {
  return ReservedBlock(mapping).header;
}

uint64_t ToOffset(void* ptr, void* base) {
  return static_cast<uint8_t*>(ptr) - static_cast<uint8_t*>(base);
}

absl::Span<uint8_t> GetPrimaryLinkStateAllocatorMemory(
    const DriverMemoryMapping& mapping) {
  return {reinterpret_cast<uint8_t*>(&ReservedBlock(mapping) + 1),
          kPrimaryBufferLinkAllocatorSize};
}

}  // namespace

NodeLinkMemory::NodeLinkMemory(mem::Ref<Node> node,
                               DriverMemoryMapping primary_buffer_mapping)
    : node_(std::move(node)) {
  buffers_.push_back(std::move(primary_buffer_mapping));

  mem::BlockAllocator allocator(
      GetPrimaryLinkStateAllocatorMemory(primary_buffer()),
      kRouterLinkStateBlockSize, mem::BlockAllocator::kAlreadyInitialized);

  auto link_state_allocators =
      std::make_unique<BlockAllocatorPool>(kRouterLinkStateBlockSize);
  link_state_allocators->AddAllocator(kPrimaryBufferId,
                                      primary_buffer().bytes(), allocator);
  block_allocator_pools_[kRouterLinkStateBlockSize] =
      std::move(link_state_allocators);
}

NodeLinkMemory::~NodeLinkMemory() = default;

// static
mem::Ref<NodeLinkMemory> NodeLinkMemory::Allocate(
    mem::Ref<Node> node,
    size_t num_initial_portals,
    DriverMemory& primary_buffer_memory) {
  primary_buffer_memory = DriverMemory(node->driver(), kPrimaryBufferSize);
  DriverMemoryMapping mapping(primary_buffer_memory.Map());
  PrimaryBufferHeader& header = Header(mapping);
  header.next_sublink = num_initial_portals;
  header.next_buffer_id = 1;
  header.next_router_link_state_index = num_initial_portals;

  mem::BlockAllocator allocator(GetPrimaryLinkStateAllocatorMemory(mapping),
                                kRouterLinkStateBlockSize,
                                mem::BlockAllocator::kInitialize);

  return mem::WrapRefCounted(
      new NodeLinkMemory(std::move(node), std::move(mapping)));
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
}

MappedNodeLinkAddress NodeLinkMemory::GetMappedAddress(
    const NodeLinkAddress& address) {
  if (address.is_null()) {
    return {};
  }

  if (address.buffer_id() == kPrimaryBufferId) {
    // Fast path for primary buffer access.
    ABSL_ASSERT(!buffers_.empty());
    return MappedNodeLinkAddress(address,
                                 buffers_.front().address_at(address.offset()));
  }

  absl::MutexLock lock(&mutex_);
  auto it = buffer_map_.find(address.buffer_id());
  if (it == buffer_map_.end()) {
    return {};
  }

  return MappedNodeLinkAddress(address,
                               it->second->address_at(address.offset()));
}

SublinkId NodeLinkMemory::AllocateSublinkIds(size_t count) {
  return Header(primary_buffer())
      .next_sublink.fetch_add(count, std::memory_order_relaxed);
}

MappedNodeLinkAddress NodeLinkMemory::GetInitialRouterLinkState(size_t i) {
  auto& states = ReservedBlock(primary_buffer()).initial_link_states;
  ABSL_ASSERT(i < states.size());
  RouterLinkState* state = &states[i];
  return MappedNodeLinkAddress(
      NodeLinkAddress(kPrimaryBufferId,
                      ToOffset(state, primary_buffer().address())),
      state);
}

MappedNodeLinkAddress NodeLinkMemory::AllocateRouterLinkState() {
  MappedNodeLinkAddress addr = AllocateBlock(kRouterLinkStateBlockSize);
  if (!addr.is_null()) {
    RouterLinkState::Initialize(addr.mapped_address());
  }
  return addr;
}

MappedNodeLinkAddress NodeLinkMemory::AllocateBlock(size_t num_bytes) {
  BlockAllocatorPool* pool = GetPoolForAllocation(absl::bit_ceil(num_bytes));
  if (!pool) {
    return {};
  }

  return pool->Allocate();
}

void NodeLinkMemory::FreeBlock(const MappedNodeLinkAddress& address,
                               size_t num_bytes) {
  if (address.is_null()) {
    return;
  }

  BlockAllocatorPool* pool = GetPoolForAllocation(absl::bit_ceil(num_bytes));
  pool->Free(address);
}

void NodeLinkMemory::RequestBlockAllocatorCapacity(
    uint32_t buffer_size,
    uint32_t block_size,
    RequestBlockAllocatorCapacityCallback callback) {
  block_size = absl::bit_ceil(block_size);
  {
    absl::MutexLock lock(&mutex_);
    auto [it, need_new_request] =
        capacity_callbacks_.emplace(block_size, CapacityCallbackList());
    it->second.push_back(std::move(callback));
    if (!need_new_request) {
      return;
    }
  }

  node_->AllocateSharedMemory(buffer_size, [self = mem::WrapRefCounted(this),
                                            block_size](DriverMemory memory) {
    mem::Ref<NodeLink> link;
    const BufferId new_buffer_id = self->AllocateBufferId();
    CapacityCallbackList callbacks;
    {
      absl::MutexLock lock(&self->mutex_);
      self->buffers_.push_back(memory.Map());
      DriverMemoryMapping* mapping = &self->buffers_.back();
      self->buffer_map_[new_buffer_id] = mapping;
      auto it = self->capacity_callbacks_.find(block_size);
      if (it != self->capacity_callbacks_.end()) {
        callbacks = std::move(it->second);
        self->capacity_callbacks_.erase(it);
      }
      link = self->node_link_;

      mem::BlockAllocator allocator(mapping->bytes(), block_size,
                                    mem::BlockAllocator::kInitialize);

      std::unique_ptr<BlockAllocatorPool>& pool =
          self->block_allocator_pools_[block_size];
      if (!pool) {
        pool = std::make_unique<BlockAllocatorPool>(block_size);
      }
      pool->AddAllocator(new_buffer_id, mapping->bytes(), allocator);
    }

    if (link) {
      link->AddBlockAllocatorBuffer(new_buffer_id, block_size,
                                    std::move(memory));
    }

    for (auto& callback : callbacks) {
      callback();
    }
  });
}

bool NodeLinkMemory::AddBlockAllocatorBuffer(BufferId id,
                                             uint32_t block_size,
                                             DriverMemory memory) {
  block_size = absl::bit_ceil(block_size);
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

    mem::BlockAllocator allocator(mapping.bytes(), block_size,
                                  mem::BlockAllocator::kAlreadyInitialized);
    std::unique_ptr<BlockAllocatorPool>& pool =
        block_allocator_pools_[block_size];
    if (!pool) {
      pool = std::make_unique<BlockAllocatorPool>(block_size);
    }
    pool->AddAllocator(id, mapping.bytes(), allocator);

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
  return Header(primary_buffer())
      .next_buffer_id.fetch_add(1, std::memory_order_relaxed);
}

BlockAllocatorPool* NodeLinkMemory::GetPoolForAllocation(size_t num_bytes) {
  absl::MutexLock lock(&mutex_);
  auto it = block_allocator_pools_.find(absl::bit_ceil(num_bytes));
  if (it == block_allocator_pools_.end()) {
    return nullptr;
  }
  return it->second.get();
}

}  // namespace core
}  // namespace ipcz
