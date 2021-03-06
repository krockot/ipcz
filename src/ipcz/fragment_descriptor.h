// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_FRAGMENT_DESCRIPTOR_H_
#define IPCZ_SRC_IPCZ_FRAGMENT_DESCRIPTOR_H_

#include <cstdint>
#include <string>
#include <tuple>

#include "ipcz/buffer_id.h"
#include "ipcz/ipcz.h"

namespace ipcz {

// Represents a span of memory within the shared memory regions owned by a
// NodeLinkMemory. A FragmentDescriptor can be resolved to a concrete Fragment
// by passing it to NodeLinkMemory's GetFragment() or AdoptFragmentRef().
struct IPCZ_ALIGN(8) FragmentDescriptor {
  constexpr FragmentDescriptor() = default;
  FragmentDescriptor(const FragmentDescriptor&);
  FragmentDescriptor& operator=(const FragmentDescriptor&);
  constexpr FragmentDescriptor(BufferId buffer_id,
                               uint32_t offset,
                               uint32_t size)
      : buffer_id_(buffer_id), offset_(offset), size_(size) {}

  bool is_null() const { return buffer_id_ == kInvalidBufferId; }

  bool operator==(const FragmentDescriptor& other) const {
    return std::tie(buffer_id_, offset_, size_) ==
           std::tie(other.buffer_id_, other.offset_, other.size_);
  }

  BufferId buffer_id() const { return buffer_id_; }
  uint32_t offset() const { return offset_; }
  uint32_t size() const { return size_; }
  uint32_t end() const { return offset_ + size_; }

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

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_FRAGMENT_DESCRIPTOR_H_
