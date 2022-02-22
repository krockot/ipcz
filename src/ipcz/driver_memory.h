// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_DRIVER_MEMORY_H_
#define IPCZ_SRC_IPCZ_DRIVER_MEMORY_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ipcz/driver_memory_mapping.h"
#include "ipcz/driver_object.h"
#include "ipcz/ipcz.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/os_handle.h"
#include "util/ref_counted.h"

namespace ipcz {

class Node;

// Scoped wrapper around a shared memory region allocated and manipulated
// through an ipcz driver.
class DriverMemory {
 public:
  DriverMemory();

  // Takes ownership of an existing driver memory object.
  DriverMemory(DriverObject memory, size_t num_bytes);

  // Asks the node to allocate a new driver shared memory region of at least
  // `num_bytes` in size.
  DriverMemory(Ref<Node> node, size_t num_bytes);

  DriverMemory(DriverMemory&& other);
  DriverMemory& operator=(DriverMemory&& other);

  ~DriverMemory();

  bool is_valid() const { return memory_.is_valid(); }
  size_t size() const { return size_; }

  DriverObject& driver_object() { return memory_; }

  // Asks the driver to clone this memory object and return a new one which
  // references the same underlying memory region.
  DriverMemory Clone();

  // Asks the driver to map this memory object into the process's address space
  // and returns a scoper to control the mapping's lifetime.
  DriverMemoryMapping Map();

  // Asks the driver to serialize this memory object into a series of bytes
  // and/or OS handles suitable to send over a driver transport, to be
  // deserialized intact on another node.
  IpczResult Serialize(std::vector<uint8_t>& data,
                       std::vector<OSHandle>& handles);

  // Asks `driver` to deserialize a memory object from a series of bytes and/or
  // OS handles received over a driver transport.
  static DriverMemory Deserialize(Ref<Node> node,
                                  absl::Span<const uint8_t> data,
                                  absl::Span<OSHandle> handles);

 private:
  DriverObject memory_;
  size_t size_ = 0;
};

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_DRIVER_MEMORY_H_
