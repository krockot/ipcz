// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_NODE_LINK_MEMORY_H_
#define IPCZ_SRC_CORE_NODE_LINK_MEMORY_H_

#include <cstddef>

#include "core/node_link_address.h"
#include "core/routing_id.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "os/memory.h"

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
  NodeLinkAddress AllocateRouterLinkState();

 private:
  ~NodeLinkMemory() override;

  explicit NodeLinkMemory(os::Memory::Mapping primary_buffer);

  NodeLinkAddress AllocateUninitializedRouterLinkState();

  os::Memory::Mapping primary_buffer_mapping_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NODE_LINK_MEMORY_H_
