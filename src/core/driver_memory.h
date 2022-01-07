// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_DRIVER_MEMORY_H_
#define IPCZ_SRC_CORE_DRIVER_MEMORY_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/driver_memory_mapping.h"
#include "ipcz/ipcz.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

// Scoped wrapper around a driver-controlled shared memory region.
class DriverMemory {
 public:
  DriverMemory();
  DriverMemory(const IpczDriver& driver, size_t num_bytes);
  DriverMemory(DriverMemory&& other);
  DriverMemory(const DriverMemory&) = delete;
  DriverMemory& operator=(DriverMemory&& other);
  DriverMemory& operator=(const DriverMemory&) = delete;
  ~DriverMemory();

  bool is_valid() const { return memory_ != IPCZ_INVALID_DRIVER_HANDLE; }
  size_t size() const { return size_; }

  DriverMemory Clone();
  DriverMemoryMapping Map();

  IpczResult Serialize(std::vector<uint8_t>& data,
                       std::vector<os::Handle>& handles);
  static DriverMemory Deserialize(const IpczDriver& driver,
                                  absl::Span<const uint8_t> data,
                                  absl::Span<os::Handle> handles);

 private:
  void Release();

  IpczDriver driver_;
  size_t size_ = 0;
  IpczDriverHandle memory_ = IPCZ_INVALID_DRIVER_HANDLE;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_DRIVER_MEMORY_H_
