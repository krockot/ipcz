// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/driver_memory.h"

#include <algorithm>

#include "ipcz/ipcz.h"
#include "third_party/abseil-cpp/absl/base/macros.h"

namespace ipcz {
namespace core {

DriverMemory::DriverMemory() = default;

DriverMemory::DriverMemory(const IpczDriver& driver, size_t num_bytes)
    : driver_(driver), size_(num_bytes) {
  IpczResult result =
      driver_.AllocateSharedMemory(num_bytes, 0, nullptr, &memory_);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
}

DriverMemory::DriverMemory(DriverMemory&& other)
    : size_(0), memory_(IPCZ_INVALID_DRIVER_HANDLE) {
  driver_ = other.driver_;
  std::swap(size_, other.size_);
  std::swap(memory_, other.memory_);
}

DriverMemory& DriverMemory::operator=(DriverMemory&& other) {
  Release();
  driver_ = other.driver_;
  size_ = other.size_;
  std::swap(memory_, other.memory_);
  return *this;
}

DriverMemory::~DriverMemory() {
  Release();
}

DriverMemory DriverMemory::Clone() {
  ABSL_ASSERT(is_valid());
  DriverMemory new_memory;
  new_memory.driver_ = driver_;
  new_memory.size_ = size_;
  IpczResult result =
      driver_.DuplicateSharedMemory(memory_, 0, nullptr, &new_memory.memory_);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
  return new_memory;
}

DriverMemoryMapping DriverMemory::Map() {
  ABSL_ASSERT(is_valid());
  void* address;
  IpczDriverHandle mapping_handle;
  IpczResult result =
      driver_.MapSharedMemory(memory_, 0, nullptr, &address, &mapping_handle);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
  return DriverMemoryMapping(driver_, mapping_handle, address, size_);
}

IpczResult DriverMemory::Serialize(std::vector<uint8_t>& data,
                                   std::vector<os::Handle>& handles) {
  uint32_t num_bytes = 0;
  uint32_t num_os_handles = 0;
  IpczResult result =
      driver_.SerializeSharedMemory(memory_, IPCZ_NO_FLAGS, nullptr, nullptr,
                                    &num_bytes, nullptr, &num_os_handles);
  ABSL_ASSERT(result == IPCZ_RESULT_RESOURCE_EXHAUSTED);
  data.resize(num_bytes);
  std::vector<IpczOSHandle> os_handles(num_os_handles);
  for (IpczOSHandle& handle : os_handles) {
    handle.size = sizeof(handle);
  }
  result = driver_.SerializeSharedMemory(memory_, IPCZ_NO_FLAGS, nullptr,
                                         data.data(), &num_bytes,
                                         os_handles.data(), &num_os_handles);
  ABSL_ASSERT(result != IPCZ_RESULT_RESOURCE_EXHAUSTED);
  if (result != IPCZ_RESULT_OK) {
    return result;
  }

  handles.resize(num_os_handles);
  for (size_t i = 0; i < num_os_handles; ++i) {
    handles[i] = os::Handle::FromIpczOSHandle(os_handles[i]);
  }

  memory_ = IPCZ_INVALID_DRIVER_HANDLE;
  return IPCZ_RESULT_OK;
}

// static
DriverMemory DriverMemory::Deserialize(const IpczDriver& driver,
                                       absl::Span<const uint8_t> data,
                                       absl::Span<os::Handle> handles) {
  std::vector<IpczOSHandle> os_handles(handles.size());
  bool fail = false;
  for (size_t i = 0; i < handles.size(); ++i) {
    os_handles[i].size = sizeof(os_handles[i]);
    bool ok = os::Handle::ToIpczOSHandle(std::move(handles[i]), &os_handles[i]);
    if (!ok) {
      fail = true;
    }
  }

  if (fail) {
    return DriverMemory();
  }

  uint32_t region_size;
  IpczDriverHandle memory;
  IpczResult result = driver.DeserializeSharedMemory(
      data.data(), static_cast<uint32_t>(data.size()), os_handles.data(),
      static_cast<uint32_t>(os_handles.size()), IPCZ_NO_FLAGS, nullptr,
      &region_size, &memory);
  if (result != IPCZ_RESULT_OK) {
    return DriverMemory();
  }

  DriverMemory new_memory;
  new_memory.driver_ = driver;
  new_memory.size_ = region_size;
  new_memory.memory_ = memory;
  return new_memory;
}

void DriverMemory::Release() {
  if (is_valid()) {
    driver_.ReleaseSharedMemory(memory_, 0, nullptr);
    size_ = 0;
    memory_ = IPCZ_INVALID_DRIVER_HANDLE;
  }
}

}  // namespace core
}  // namespace ipcz
