// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_FRAGMENT_DESCRIPTOR_H_
#define IPCZ_SRC_CORE_FRAGMENT_DESCRIPTOR_H_

#include <cstdint>
#include <string>

#include "core/buffer_id.h"
#include "ipcz/ipcz.h"

namespace ipcz {
namespace core {

// Represents a span of memory within the shared memory regions owned by a
// NodeLinkMemory. A FragmentDescriptor can be resolved to a concrete Fragment
// by passing it to NodeLinkMemory::GetFragment().
struct IPCZ_ALIGN(8) FragmentDescriptor {
  constexpr FragmentDescriptor() = default;
  FragmentDescriptor(const FragmentDescriptor&);
  FragmentDescriptor& operator=(const FragmentDescriptor&);
  constexpr FragmentDescriptor(BufferId buffer_id,
                               uint32_t offset,
                               uint32_t size)
      : buffer_id_(buffer_id), offset_(offset), size_(size) {}

  bool is_null() const { return buffer_id_ == kInvalidBufferId; }

  BufferId buffer_id() const { return buffer_id_; }
  uint32_t offset() const { return offset_; }
  uint32_t size() const { return size_; }

  std::string ToString() const;

 private:
  // Identifies the shared memory buffer in which the memory resides. This ID is
  // scoped to a specific NodeLink.
  BufferId buffer_id_ = kInvalidBufferId;

  // The byte offset from the start of the identified shared memory buffer where
  // this fragment begins.
  uint32_t offset_ = 0;

  // The size of this fragment in bytes.
  uint32_t size_ = 0;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_FRAGMENT_DESCRIPTOR_H_
