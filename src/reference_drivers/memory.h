// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_REFERENCE_DRIVERS_MEMORY_H_
#define IPCZ_SRC_REFERENCE_DRIVERS_MEMORY_H_

#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/os_handle.h"

namespace ipcz {
namespace reference_drivers {

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
  Memory(OSHandle handle, size_t size);
  explicit Memory(size_t size);
  Memory(Memory&&);
  Memory& operator=(Memory&&);
  Memory(const Memory&) = delete;
  Memory& operator=(const Memory&) = delete;
  ~Memory();

  size_t size() const { return size_; }
  bool is_valid() const { return handle_.is_valid(); }
  const OSHandle& handle() const { return handle_; }

  OSHandle TakeHandle() { return std::move(handle_); }

  void reset() { handle_.reset(); }

  Memory Clone();

  Mapping Map();

 private:
  OSHandle handle_;
  size_t size_ = 0;
};

}  // namespace reference_drivers
}  // namespace ipcz

#endif  // IPCZ_SRC_REFERENCE_DRIVERS_MEMORY_H_
