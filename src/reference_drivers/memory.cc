// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "reference_drivers/memory.h"

#include "build/build_config.h"
#include "third_party/abseil-cpp/absl/base/macros.h"

#if defined(OS_POSIX)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#if defined(OS_WIN)
#include <windows.h>
#endif

namespace ipcz {
namespace reference_drivers {

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
#elif defined(OS_WIN)
    ::UnmapViewOfFile(base_address_);
#endif
  }
}

Memory::Memory() = default;

Memory::Memory(OSHandle handle, size_t size)
    : handle_(std::move(handle)), size_(size) {}

Memory::Memory(size_t size) {
#if defined(OS_POSIX)
  int fd = memfd_create("/ipcz/mem", MFD_ALLOW_SEALING);
  ABSL_ASSERT(fd >= 0);

  int result = ftruncate(fd, size);
  ABSL_ASSERT(result == 0);

  result = fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK);
  ABSL_ASSERT(result == 0);

  handle_ = OSHandle(fd);
  size_ = size;
#elif defined(OS_WIN)
  HANDLE h = ::CreateFileMapping(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                 0, static_cast<DWORD>(size), nullptr);
  const HANDLE process = ::GetCurrentProcess();
  HANDLE h2;
  BOOL ok = ::DuplicateHandle(process, h, process, &h2,
                              FILE_MAP_READ | FILE_MAP_WRITE, FALSE, 0);
  ::CloseHandle(h);
  ABSL_ASSERT(ok);

  handle_ = OSHandle(h2);
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
#elif defined(OS_WIN)
  void* addr = ::MapViewOfFile(handle_.handle(), FILE_MAP_READ | FILE_MAP_WRITE,
                               0, 0, size_);
  ABSL_ASSERT(addr);
  return Mapping(addr, size_);
#endif
}

}  // namespace reference_drivers
}  // namespace ipcz
