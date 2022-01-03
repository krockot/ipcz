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
// address by passing it to NodeLink::GetMemory().
struct IPCZ_ALIGN(8) NodeLinkAddress {
  NodeLinkAddress();
  NodeLinkAddress(const NodeLinkAddress&);
  NodeLinkAddress& operator=(const NodeLinkAddress&);
  NodeLinkAddress(BufferId buffer_id, uint64_t offset);
  ~NodeLinkAddress();

  // Identifies the shared memory buffer in which the memory resides. This ID is
  // scoped to a specific NodeLink.
  BufferId buffer_id = 0;

  // An offset from the start of the identified shared memory buffer.
  uint64_t offset = 0;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NODE_LINK_ADDRESS_H_
