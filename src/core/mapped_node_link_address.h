// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_MAPPED_NODE_LINK_ADDRESS_H_
#define IPCZ_SRC_CORE_MAPPED_NODE_LINK_ADDRESS_H_

#include <cstdint>
#include <string>

#include "core/buffer_id.h"
#include "core/node_link_address.h"

namespace ipcz {
namespace core {

// Represents a memory location within the shared memory regions owned by a
// NodeLinkMemory. This is essentially a NodeLinkAddress plus the cached base
// address of its corresponding NodeLinkMemory buffer.
struct MappedNodeLinkAddress {
  constexpr MappedNodeLinkAddress() = default;
  MappedNodeLinkAddress(const NodeLinkAddress& address,
                        void* buffer_base_address);
  MappedNodeLinkAddress(const MappedNodeLinkAddress&);
  MappedNodeLinkAddress& operator=(const MappedNodeLinkAddress&);

  bool is_null() const { return address_.is_null(); }

  BufferId buffer_id() const { return address_.buffer_id(); }
  uint64_t offset() const { return address_.offset(); }
  const NodeLinkAddress& address() const { return address_; }
  void* mapped_address() const { return mapped_address_; }

  std::string ToString() const;

  template <typename T>
  T* As() const {
    return static_cast<T*>(mapped_address_);
  }

 private:
  NodeLinkAddress address_;

  // The actual mapped address corresponding to `address_`.
  void* mapped_address_ = nullptr;
};

constexpr MappedNodeLinkAddress kNullMappedNodeLinkAddress;

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_MAPPED_NODE_LINK_ADDRESS_H_
