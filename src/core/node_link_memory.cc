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

PrimaryBufferLayout& PrimaryBuffer(const os::Memory::Mapping& mapping) {
  return *static_cast<PrimaryBufferLayout*>(mapping.base());
}

uint64_t ToOffset(void* ptr, void* base) {
  return static_cast<uint8_t*>(ptr) - static_cast<uint8_t*>(base);
}

uint64_t GetRouterLinkStateOffset(const os::Memory::Mapping& mapping,
                                  size_t index) {
  return ToOffset(&PrimaryBuffer(mapping).router_link_states[index],
                  mapping.base());
}

}  // namespace

NodeLinkMemory::NodeLinkMemory(os::Memory::Mapping primary_buffer_mapping)
    : primary_buffer_mapping_(std::move(primary_buffer_mapping)) {}

NodeLinkMemory::~NodeLinkMemory() = default;

// static
mem::Ref<NodeLinkMemory> NodeLinkMemory::Allocate(
    size_t num_initial_portals,
    os::Memory& primary_buffer_memory) {
  primary_buffer_memory = os::Memory(sizeof(PrimaryBufferLayout));
  os::Memory::Mapping mapping(primary_buffer_memory.Map());
  PrimaryBufferLayout& buffer = PrimaryBuffer(mapping);
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
  ABSL_ASSERT(address.buffer_id() == 0);
  return static_cast<uint8_t*>(primary_buffer_mapping_.base()) +
      address.offset();
}

RoutingId NodeLinkMemory::AllocateRoutingIds(size_t count) {
  return PrimaryBuffer(primary_buffer_mapping_)
      .next_routing_id.fetch_add(count, std::memory_order_relaxed);
}

NodeLinkAddress NodeLinkMemory::GetInitialRouterLinkState(size_t i) {
  return NodeLinkAddress(0,
                         GetRouterLinkStateOffset(primary_buffer_mapping_, i));
}

NodeLinkAddress NodeLinkMemory::AllocateRouterLinkState() {
  NodeLinkAddress addr = AllocateUninitializedRouterLinkState();
  RouterLinkState::Initialize(GetMappedAddress(addr));
  return addr;
}

NodeLinkAddress NodeLinkMemory::AllocateUninitializedRouterLinkState() {
  PrimaryBufferLayout& buffer = PrimaryBuffer(primary_buffer_mapping_);
  const uint64_t index = buffer.next_router_link_state_index.fetch_add(
      1, std::memory_order_relaxed);
  return NodeLinkAddress(
      0, GetRouterLinkStateOffset(primary_buffer_mapping_, index));
}

}  // namespace core
}  // namespace ipcz
