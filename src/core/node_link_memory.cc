// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/node_link_memory.h"

#include <array>
#include <atomic>
#include <cstdint>

#include "core/router_link_state.h"
#include "ipcz/ipcz.h"

namespace ipcz {
namespace core {

namespace {

struct IPCZ_ALIGN(16) PrimaryBufferLayout {
  std::atomic<uint64_t> next_routing_id{0};
  std::atomic<uint64_t> next_router_link_state_index{0};
  std::array<RouterLinkState, 2047> router_link_states;
};

PrimaryBufferLayout& PrimaryBufferView(const os::Memory::Mapping& mapping) {
  return *static_cast<PrimaryBufferLayout*>(mapping.base());
}

uint64_t ToOffset(void* ptr, void* base) {
  return static_cast<uint8_t*>(ptr) - static_cast<uint8_t*>(base);
}

uint64_t GetRouterLinkStateOffset(const os::Memory::Mapping& mapping,
                                  size_t index) {
  return ToOffset(&PrimaryBufferView(mapping).router_link_states[index],
                  mapping.base());
}

}  // namespace

NodeLinkMemory::NodeLinkMemory(os::Memory::Mapping primary_buffer_mapping) {
  buffers_.push_back(std::move(primary_buffer_mapping));
}

NodeLinkMemory::~NodeLinkMemory() = default;

// static
mem::Ref<NodeLinkMemory> NodeLinkMemory::Allocate(
    size_t num_initial_portals,
    os::Memory& primary_buffer_memory) {
  primary_buffer_memory = os::Memory(sizeof(PrimaryBufferLayout));
  os::Memory::Mapping mapping(primary_buffer_memory.Map());
  PrimaryBufferLayout& buffer = PrimaryBufferView(mapping);
  buffer.next_routing_id = num_initial_portals;
  buffer.next_router_link_state_index = num_initial_portals;
  return mem::WrapRefCounted(new NodeLinkMemory(std::move(mapping)));
}

// static
mem::Ref<NodeLinkMemory> NodeLinkMemory::Adopt(
    os::Memory::Mapping primary_buffer_mapping) {
  return mem::WrapRefCounted(
      new NodeLinkMemory(std::move(primary_buffer_mapping)));
}

// static
mem::Ref<NodeLinkMemory> NodeLinkMemory::Adopt(
    os::Handle primary_buffer_handle) {
  return mem::WrapRefCounted(new NodeLinkMemory(
      os::Memory(std::move(primary_buffer_handle), sizeof(PrimaryBufferLayout))
          .Map()));
}

void* NodeLinkMemory::GetMappedAddress(const NodeLinkAddress& address) {
  if (address.buffer_id() == 0) {
    // Fast path for primary buffer access.
    ABSL_ASSERT(!buffers_.empty());
    return static_cast<uint8_t*>(buffers_.front().base()) + address.offset();
  }

  absl::MutexLock lock(&mutex_);
  auto it = buffer_map_.find(address.buffer_id());
  if (it == buffer_map_.end()) {
    return nullptr;
  }

  return static_cast<uint8_t*>(it->second->base()) + address.offset();
}

RoutingId NodeLinkMemory::AllocateRoutingIds(size_t count) {
  return PrimaryBufferView(primary_buffer())
      .next_routing_id.fetch_add(count, std::memory_order_relaxed);
}

NodeLinkAddress NodeLinkMemory::GetInitialRouterLinkState(size_t i) {
  return NodeLinkAddress(0, GetRouterLinkStateOffset(primary_buffer(), i));
}

absl::optional<NodeLinkAddress> NodeLinkMemory::AllocateRouterLinkState() {
  absl::optional<NodeLinkAddress> addr = AllocateUninitializedRouterLinkState();
  if (addr) {
    RouterLinkState::Initialize(GetMappedAddress(*addr));
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

  // todo:
  ABSL_ASSERT(false);
}

absl::optional<NodeLinkAddress>
NodeLinkMemory::AllocateUninitializedRouterLinkState() {
  PrimaryBufferLayout& buffer = PrimaryBufferView(primary_buffer());
  const uint64_t index = buffer.next_router_link_state_index.fetch_add(
      1, std::memory_order_relaxed);
  return NodeLinkAddress(0, GetRouterLinkStateOffset(primary_buffer(), index));
}

}  // namespace core
}  // namespace ipcz
