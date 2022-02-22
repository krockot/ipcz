// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/driver_memory.h"

#include <algorithm>

#include "ipcz/ipcz.h"
#include "ipcz/node.h"
#include "third_party/abseil-cpp/absl/base/macros.h"

namespace ipcz {

DriverMemory::DriverMemory() = default;

DriverMemory::DriverMemory(DriverObject memory, size_t num_bytes)
    : memory_(std::move(memory)), size_(num_bytes) {}

DriverMemory::DriverMemory(Ref<Node> node, size_t num_bytes)
    : size_(num_bytes) {
  IpczDriverHandle handle;
  IpczResult result =
      node->driver().AllocateSharedMemory(num_bytes, 0, nullptr, &handle);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
  memory_ = DriverObject(std::move(node), handle);
}

DriverMemory::DriverMemory(DriverMemory&& other) = default;

DriverMemory& DriverMemory::operator=(DriverMemory&& other) = default;

DriverMemory::~DriverMemory() = default;

DriverMemory DriverMemory::Clone() {
  ABSL_ASSERT(is_valid());

  IpczDriverHandle handle;
  IpczResult result = memory_.node()->driver().DuplicateSharedMemory(
      memory_.handle(), 0, nullptr, &handle);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);

  return DriverMemory(DriverObject(memory_.node(), handle), size_);
}

DriverMemoryMapping DriverMemory::Map() {
  ABSL_ASSERT(is_valid());
  void* address;
  IpczDriverHandle mapping_handle;
  IpczResult result = memory_.node()->driver().MapSharedMemory(
      memory_.handle(), 0, nullptr, &address, &mapping_handle);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
  return DriverMemoryMapping(memory_.node()->driver(), mapping_handle, address,
                             size_);
}

IpczResult DriverMemory::Serialize(std::vector<uint8_t>& data,
                                   std::vector<OSHandle>& handles) {
  if (!memory_.is_valid()) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  DriverObject::SerializedDimensions dimensions =
      memory_.GetSerializedDimensions();
  data.resize(dimensions.num_bytes);
  handles.resize(dimensions.num_os_handles);
  return memory_.Serialize(absl::MakeSpan(data), absl::MakeSpan(handles));
}

// static
DriverMemory DriverMemory::Deserialize(Ref<Node> node,
                                       absl::Span<const uint8_t> data,
                                       absl::Span<OSHandle> handles) {
  DriverObject memory =
      DriverObject::Deserialize(std::move(node), data, handles);
  if (!memory.is_valid()) {
    return DriverMemory();
  }

  uint32_t region_size;
  IpczResult result = memory.node()->driver().GetSharedMemoryInfo(
      memory.handle(), IPCZ_NO_FLAGS, nullptr, &region_size);
  if (result != IPCZ_RESULT_OK) {
    return DriverMemory();
  }

  return DriverMemory(std::move(memory), region_size);
}

}  // namespace ipcz
