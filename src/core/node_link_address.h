// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_NODE_LINK_ADDRESS_H_
#define IPCZ_SRC_CORE_NODE_LINK_ADDRESS_H_

#include <cstdint>

#include "core/buffer_id.h"
#include "ipcz/ipcz.h"

namespace ipcz {
namespace core {

// Represents the location of a block of memory shared between the two nodes on
// either end of a NodeLink. A NodeLinkAddress can be resolved to a real memory
// address by passing it to NodeLinKMemory:;GetMappedAddress() or
// NodeLinkMemory::GetMapped<T>().
struct IPCZ_ALIGN(8) NodeLinkAddress {
  NodeLinkAddress();
  NodeLinkAddress(const NodeLinkAddress&);
  NodeLinkAddress& operator=(const NodeLinkAddress&);
  NodeLinkAddress(BufferId buffer_id, uint64_t offset);
  ~NodeLinkAddress();

  // An all-zero address is invalid by convention. Buffer 0 is the primary
  // buffer of any NodeLink, and the first bytes of the primary buffer have
  // fixed meaning, so there's never a need to reference them by
  // NodeLinkAddress.
  bool is_valid() const { return buffer_id_ != 0 || offset_ != 0; }
  BufferId buffer_id() const { return buffer_id_; }
  uint64_t offset() const { return offset_; }

 private:
  // Identifies the shared memory buffer in which the memory resides. This ID is
  // scoped to a specific NodeLink.
  BufferId buffer_id_ = 0;

  // An offset from the start of the identified shared memory buffer.
  uint64_t offset_ = 0;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NODE_LINK_ADDRESS_H_
