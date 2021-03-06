// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_FRAGMENT_H_
#define IPCZ_SRC_IPCZ_FRAGMENT_H_

#include <cstdint>
#include <string>

#include "ipcz/buffer_id.h"
#include "ipcz/fragment_descriptor.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {

// Represents a span of memory located within the shared memory regions owned by
// a NodeLinkMemory. This is essentially a FragmentDescriptor plus the actual
// mapped address of the given buffer and offset.
struct Fragment {
  constexpr Fragment() = default;
  Fragment(const FragmentDescriptor& descriptor, void* address);
  Fragment(const Fragment&);
  Fragment& operator=(const Fragment&);

  bool is_null() const { return descriptor_.is_null(); }
  bool is_resolved() const { return address_ != nullptr; }
  bool is_addressable() const { return !is_null() && is_resolved(); }
  bool is_pending() const { return !is_null() && !is_resolved(); }

  bool operator==(const Fragment& other) const {
    return descriptor_ == other.descriptor_;
  }

  BufferId buffer_id() const { return descriptor_.buffer_id(); }
  uint32_t offset() const { return descriptor_.offset(); }
  uint32_t size() const { return descriptor_.size(); }
  const FragmentDescriptor& descriptor() const { return descriptor_; }

  void* address() const { return address_; }

  absl::Span<uint8_t> mutable_bytes() const {
    return {static_cast<uint8_t*>(address_), descriptor_.size()};
  }

  absl::Span<const uint8_t> bytes() const {
    return {static_cast<const uint8_t*>(address_), descriptor_.size()};
  }

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

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_FRAGMENT_H_
