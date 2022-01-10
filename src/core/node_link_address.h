// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_NODE_LINK_ADDRESS_H_
#define IPCZ_SRC_CORE_NODE_LINK_ADDRESS_H_

#include <cstdint>
#include <string>

#include "core/buffer_id.h"
#include "ipcz/ipcz.h"

namespace ipcz {
namespace core {

// Represents a memory location within the shared memory regions owned by a
// NodeLinkMemory. A NodeLinkAddress can be resolved to a real memory address by
// passing it to NodeLinKMemory::GetMappedAddress() or
// NodeLinkMemory::GetMapped<T>().
struct IPCZ_ALIGN(8) NodeLinkAddress {
  constexpr NodeLinkAddress() = default;
  NodeLinkAddress(const NodeLinkAddress&);
  NodeLinkAddress& operator=(const NodeLinkAddress&);
  constexpr NodeLinkAddress(BufferId buffer_id, uint64_t offset)
      : buffer_id_(buffer_id), offset_(offset) {}

  bool is_null() const { return buffer_id_ == 0xffffffffffffffffull; }

  BufferId buffer_id() const { return buffer_id_; }
  uint64_t offset() const { return offset_; }

  std::string ToString() const;

 private:
  // Identifies the shared memory buffer in which the memory resides. This ID is
  // scoped to a specific NodeLink.
  BufferId buffer_id_ = 0xffffffffffffffffull;

  // An offset from the start of the identified shared memory buffer.
  uint64_t offset_ = 0xffffffffffffffffull;
};

constexpr NodeLinkAddress kNullNodeLinkAddress;

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NODE_LINK_ADDRESS_H_
