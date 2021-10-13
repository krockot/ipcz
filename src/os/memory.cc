// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "os/memory.h"

#include "build/build_config.h"
#include "third_party/abseil-cpp/absl/base/macros.h"

#if defined(OS_POSIX)
#include <fcntl.h>
#include <sys/mman.h>
#endif

namespace ipcz {
namespace os {

Memory::Mapping::Mapping() = default;

Memory::Mapping::Mapping(void* base_address, size_t size)
    : base_address_(base_address), size_(size) {}

Memory::Mapping::Mapping(Mapping&& other) {
  base_address_ = other.base_address_;
  size_ = other.size_;
  other.base_address_ = nullptr;
  other.size_ = 0;
}

Memory::Mapping& Memory::Mapping::operator=(Mapping&& other) {
  Reset();
  base_address_ = other.base_address_;
  size_ = other.size_;
  other.base_address_ = nullptr;
  other.size_ = 0;
  return *this;
}

Memory::Mapping::~Mapping() {
  Reset();
}

void Memory::Mapping::Reset() {
  if (base_address_) {
#if defined(OS_POSIX)
    munmap(base_address_, size_);
    base_address_ = nullptr;
    size_ = 0;
#endif
  }
}

Memory::Memory() = default;

Memory::Memory(Handle handle, size_t size)
    : handle_(std::move(handle)), size_(size) {}

Memory::Memory(size_t size) {
#if defined(OS_POSIX)
  int fd = memfd_create("/ipcz/mem", MFD_ALLOW_SEALING);
  ABSL_ASSERT(fd >= 0);

  int result = ftruncate(fd, size);
  ABSL_ASSERT(result == 0);

  result = fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK);
  ABSL_ASSERT(result == 0);

  handle_ = Handle(fd);
  size_ = size;
#endif
}

Memory::Memory(Memory&&) = default;

Memory& Memory::operator=(Memory&&) = default;

Memory::~Memory() = default;

Memory Memory::Clone() {
  return Memory(handle_.Clone(), size_);
}

Memory::Mapping Memory::Map() {
  ABSL_ASSERT(is_valid());
#if defined(OS_POSIX)
  void* addr =
      mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, handle_.fd(), 0);
  ABSL_ASSERT(addr && addr != MAP_FAILED);
  return Mapping(addr, size_);
#endif
}

}  // namespace os
}  // namespace ipcz
