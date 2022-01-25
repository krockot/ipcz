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

namespace ipcz {
namespace core {

namespace {

constexpr size_t kAuxBufferSize = 16384;

struct IPCZ_ALIGN(8) PrimaryBufferLayout {
  std::atomic<uint64_t> next_sublink{0};
  std::atomic<uint64_t> next_buffer_id{1};
  std::atomic<uint64_t> next_router_link_state_index{0};
  uint64_t padding = 0;
  std::array<RouterLinkState, 64> router_link_states;
};

PrimaryBufferLayout& PrimaryBufferView(const DriverMemoryMapping& mapping) {
  return *static_cast<PrimaryBufferLayout*>(mapping.address());
}

uint64_t ToOffset(void* ptr, void* base) {
  return static_cast<uint8_t*>(ptr) - static_cast<uint8_t*>(base);
}

uint64_t GetRouterLinkStateOffset(const DriverMemoryMapping& mapping,
                                  size_t index) {
  return ToOffset(&PrimaryBufferView(mapping).router_link_states[index],
                  mapping.address());
}

}  // namespace

NodeLinkMemory::NodeLinkMemory(mem::Ref<Node> node,
                               DriverMemoryMapping primary_buffer_mapping)
    : node_(std::move(node)) {
  buffers_.push_back(std::move(primary_buffer_mapping));
}

NodeLinkMemory::~NodeLinkMemory() = default;

// static
mem::Ref<NodeLinkMemory> NodeLinkMemory::Allocate(
    mem::Ref<Node> node,
    size_t num_initial_portals,
    DriverMemory& primary_buffer_memory) {
  primary_buffer_memory =
      DriverMemory(node->driver(), sizeof(PrimaryBufferLayout));
  DriverMemoryMapping mapping(primary_buffer_memory.Map());
  PrimaryBufferLayout& buffer = PrimaryBufferView(mapping);
  buffer.next_sublink = num_initial_portals;
  buffer.next_buffer_id = 1;
  buffer.next_router_link_state_index = num_initial_portals;
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

void* NodeLinkMemory::GetMappedAddress(const NodeLinkAddress& address) {
  if (address.buffer_id() == 0) {
    // Fast path for primary buffer access.
    ABSL_ASSERT(!buffers_.empty());
    return static_cast<uint8_t*>(buffers_.front().address()) + address.offset();
  }

  absl::MutexLock lock(&mutex_);
  auto it = buffer_map_.find(address.buffer_id());
  if (it == buffer_map_.end()) {
    return nullptr;
  }

  return static_cast<uint8_t*>(it->second->address()) + address.offset();
}

SublinkId NodeLinkMemory::AllocateSublinkIds(size_t count) {
  return PrimaryBufferView(primary_buffer())
      .next_sublink.fetch_add(count, std::memory_order_relaxed);
}

NodeLinkAddress NodeLinkMemory::GetInitialRouterLinkState(size_t i) {
  return NodeLinkAddress(0, GetRouterLinkStateOffset(primary_buffer(), i));
}

NodeLinkAddress NodeLinkMemory::AllocateRouterLinkState() {
  NodeLinkAddress addr = AllocateUninitializedRouterLinkState();
  if (!addr.is_null()) {
    RouterLinkState::Initialize(GetMappedAddress(addr));
  }
  return addr;
}

void NodeLinkMemory::RequestCapacity(CapacityCallback callback) {
  {
    absl::MutexLock lock(&mutex_);
    capacity_callbacks_.push_back(std::move(callback));
    if (awaiting_capacity_) {
      return;
    }
    awaiting_capacity_ = true;
  }

  node_->AllocateSharedMemory(
      kAuxBufferSize, [self = mem::WrapRefCounted(this)](DriverMemory memory) {
        mem::Ref<NodeLink> link;
        const BufferId new_buffer_id = self->AllocateBufferId();
        std::vector<CapacityCallback> callbacks;
        {
          absl::MutexLock lock(&self->mutex_);
          self->buffers_.push_back(memory.Map());
          self->buffer_map_[new_buffer_id] = &self->buffers_.back();
          self->awaiting_capacity_ = false;
          std::swap(callbacks, self->capacity_callbacks_);
          link = self->node_link_;
        }

        for (CapacityCallback& callback : callbacks) {
          callback();
        }

        if (link) {
          link->AddLinkBuffer(new_buffer_id, std::move(memory));
        }
      });
}

void NodeLinkMemory::AddBuffer(BufferId id, DriverMemory memory) {
  std::vector<std::function<void()>> buffer_callbacks;
  {
    absl::MutexLock lock(&mutex_);
    buffers_.push_back(memory.Map());
    buffer_map_[id] = &buffers_.back();

    auto it = buffer_callbacks_.find(id);
    if (it != buffer_callbacks_.end()) {
      std::swap(it->second, buffer_callbacks);
      buffer_callbacks_.erase(it);
    }
  }

  for (std::function<void()>& callback : buffer_callbacks) {
    callback();
  }
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
  return PrimaryBufferView(primary_buffer())
      .next_buffer_id.fetch_add(1, std::memory_order_relaxed);
}

NodeLinkAddress NodeLinkMemory::AllocateUninitializedRouterLinkState() {
  PrimaryBufferLayout& buffer = PrimaryBufferView(primary_buffer());
  const uint64_t index = buffer.next_router_link_state_index.fetch_add(
      1, std::memory_order_relaxed);
  if (index < buffer.router_link_states.size()) {
    return NodeLinkAddress(0,
                           GetRouterLinkStateOffset(primary_buffer(), index));
  }
  uint64_t adjusted_index = index - buffer.router_link_states.size();
  constexpr size_t kStatesPerBuffer = kAuxBufferSize / sizeof(RouterLinkState);
  BufferId buffer_id = 1 + adjusted_index / kStatesPerBuffer;
  uint64_t offset =
      (adjusted_index % kStatesPerBuffer) * sizeof(RouterLinkState);
  absl::MutexLock lock(&mutex_);
  auto it = buffer_map_.find(buffer_id);
  if (it == buffer_map_.end()) {
    return NodeLinkAddress();
  }

  return NodeLinkAddress(buffer_id, offset);
}

}  // namespace core
}  // namespace ipcz
