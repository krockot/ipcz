// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_OS_MEMORY_H_
#define IPCZ_SRC_OS_MEMORY_H_

#include "os/handle.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace os {

// Generic shared memory object.
class Memory {
 public:
  class Mapping {
   public:
    Mapping();
    Mapping(void* base_address, size_t size);
    Mapping(Mapping&&);
    Mapping& operator=(Mapping&&);
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    ~Mapping();

    bool is_valid() const { return base_address_ != nullptr; }

    size_t size() const { return size_; }
    void* base() const { return base_address_; }

    absl::Span<uint8_t> bytes() const {
      return {static_cast<uint8_t*>(base()), size_};
    }

    template <typename T>
    T* As() const {
      return static_cast<T*>(base());
    }

    void Reset();

   private:
    void* base_address_ = nullptr;
    size_t size_ = 0;
  };

  Memory();
  Memory(Handle handle, size_t size);
  explicit Memory(size_t size);
  Memory(Memory&&);
  Memory& operator=(Memory&&);
  Memory(const Memory&) = delete;
  Memory& operator=(const Memory&) = delete;
  ~Memory();

  size_t size() const { return size_; }
  bool is_valid() const { return handle_.is_valid(); }
  const Handle& handle() const { return handle_; }

  Handle TakeHandle() { return std::move(handle_); }

  void reset() { handle_.reset(); }

  Memory Clone();

  Mapping Map();

 private:
  Handle handle_;
  size_t size_ = 0;
};

}  // namespace os
}  // namespace ipcz

#endif  // IPCZ_SRC_OS_MEMORY_H_
