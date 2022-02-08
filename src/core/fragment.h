// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_FRAGMENT_H_
#define IPCZ_SRC_CORE_FRAGMENT_H_

#include <cstdint>
#include <string>

#include "core/buffer_id.h"
#include "core/fragment_descriptor.h"

namespace ipcz {
namespace core {

// Represents a span of memory located within the shared memory regions owned by
// a NodeLinkMemory. This is essentially a FragmentDescriptor plus the actual
// mapped address of the given buffer and offset.
struct Fragment {
  constexpr Fragment() = default;
  Fragment(const FragmentDescriptor& descriptor, void* address);
  Fragment(const Fragment&);
  Fragment& operator=(const Fragment&);

  bool is_null() const { return descriptor_.is_null(); }

  BufferId buffer_id() const { return descriptor_.buffer_id(); }
  uint64_t offset() const { return descriptor_.offset(); }
  const FragmentDescriptor& descriptor() const { return descriptor_; }

  void* address() const { return address_; }

  std::string ToString() const;

  template <typename T>
  T* As() const {
    return static_cast<T*>(address_);
  }

 private:
  FragmentDescriptor descriptor_;

  // The actual mapped address corresponding to `descriptor_`.
  void* address_ = nullptr;
};

constexpr Fragment kNullFragment;

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_FRAGMENT_H_
