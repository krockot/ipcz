// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_DRIVER_MEMORY_MAPPING_H_
#define IPCZ_SRC_CORE_DRIVER_MEMORY_MAPPING_H_

#include "ipcz/ipcz.h"

namespace ipcz {
namespace core {

// Scoped wrapper around a driver-controlled shared memory region mapping.
class DriverMemoryMapping {
 public:
  DriverMemoryMapping();

  // Tracks the driver-produced handle and base address of an active memory
  // mapping.
  DriverMemoryMapping(const IpczDriver& driver,
                      IpczDriverHandle mapping_handle,
                      void* address);

  DriverMemoryMapping(DriverMemoryMapping&& other);
  DriverMemoryMapping(const DriverMemoryMapping&) = delete;
  DriverMemoryMapping& operator=(DriverMemoryMapping&& other);
  DriverMemoryMapping& operator=(const DriverMemoryMapping&) = delete;
  ~DriverMemoryMapping();

  bool is_valid() const { return mapping_ != IPCZ_INVALID_DRIVER_HANDLE; }

  void* address() const { return address_; }

 private:
  void Unmap();

  IpczDriver driver_;
  IpczDriverHandle mapping_;
  void* address_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_DRIVER_MEMORY_MAPPING_H_
