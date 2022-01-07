// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/driver_memory_mapping.h"

#include <algorithm>

namespace ipcz {
namespace core {

DriverMemoryMapping::DriverMemoryMapping() = default;

DriverMemoryMapping::DriverMemoryMapping(const IpczDriver& driver,
                                         IpczDriverHandle mapping_handle,
                                         void* address)
    : driver_(driver), mapping_(mapping_handle), address_(address) {}

DriverMemoryMapping::DriverMemoryMapping(DriverMemoryMapping&& other)
    : driver_(other.driver_),
      mapping_(IPCZ_INVALID_DRIVER_HANDLE),
      address_(nullptr) {
  std::swap(mapping_, other.mapping_);
  std::swap(address_, other.address_);
}

DriverMemoryMapping& DriverMemoryMapping::operator=(
    DriverMemoryMapping&& other) {
  Unmap();
  driver_ = other.driver_;
  std::swap(mapping_, other.mapping_);
  std::swap(address_, other.address_);
  return *this;
}

DriverMemoryMapping::~DriverMemoryMapping() {
  Unmap();
}

void DriverMemoryMapping::Unmap() {
  if (is_valid()) {
    driver_.UnmapSharedMemory(mapping_, 0, nullptr);
    mapping_ = IPCZ_INVALID_DRIVER_HANDLE;
    address_ = nullptr;
  }
}

}  // namespace core
}  // namespace ipcz
