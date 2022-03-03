// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/driver_object.h"

#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

#include "ipcz/ipcz.h"
#include "ipcz/node.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"

namespace ipcz {

DriverObject::DriverObject() = default;

DriverObject::DriverObject(Ref<Node> node, IpczDriverHandle handle)
    : node_(std::move(node)), handle_(handle) {}

DriverObject::DriverObject(DriverObject&& other)
    : node_(std::move(other.node_)), handle_(other.handle_) {
  other.handle_ = IPCZ_INVALID_DRIVER_HANDLE;
}

DriverObject& DriverObject::operator=(DriverObject&& other) {
  reset();
  node_ = std::move(other.node_);
  handle_ = other.handle_;
  other.handle_ = IPCZ_INVALID_DRIVER_HANDLE;
  return *this;
}

DriverObject::~DriverObject() {
  reset();
}

void DriverObject::reset() {
  if (is_valid()) {
    node_->driver().Close(handle_, IPCZ_NO_FLAGS, nullptr);
    node_.reset();
    handle_ = IPCZ_INVALID_DRIVER_HANDLE;
  }
}

IpczDriverHandle DriverObject::release() {
  IpczDriverHandle handle = handle_;
  handle_ = IPCZ_INVALID_DRIVER_HANDLE;
  node_.reset();
  return handle;
}

bool DriverObject::IsTransmissible() const {
  if (!is_valid()) {
    return false;
  }

  uint32_t num_bytes = 0;
  uint32_t num_driver_handles = 0;
  IpczResult result =
      node_->driver().Serialize(handle_, IPCZ_NO_FLAGS, nullptr, nullptr,
                                &num_bytes, 0, &num_driver_handles);
  return result == IPCZ_RESULT_ALREADY_EXISTS;
}

bool DriverObject::IsSerializable() const {
  if (!is_valid()) {
    return false;
  }

  uint32_t num_bytes = 0;
  uint32_t num_driver_handles = 0;
  IpczResult result =
      node_->driver().Serialize(handle_, IPCZ_NO_FLAGS, nullptr, nullptr,
                                &num_bytes, 0, &num_driver_handles);
  return result == IPCZ_RESULT_RESOURCE_EXHAUSTED ||
         result == IPCZ_RESULT_ALREADY_EXISTS;
}

DriverObject::SerializedDimensions DriverObject::GetSerializedDimensions()
    const {
  ABSL_ASSERT(IsSerializable());
  DriverObject::SerializedDimensions dimensions = {};
  IpczResult result = node_->driver().Serialize(
      handle_, IPCZ_NO_FLAGS, nullptr, nullptr, &dimensions.num_bytes, nullptr,
      &dimensions.num_driver_handles);
  ABSL_ASSERT(result == IPCZ_RESULT_RESOURCE_EXHAUSTED);
  ABSL_ASSERT(dimensions.num_bytes > 0 || dimensions.num_driver_handles > 0);
  return dimensions;
}

IpczResult DriverObject::Serialize(absl::Span<uint8_t> data,
                                   absl::Span<IpczDriverHandle> handles) {
  if (!is_valid()) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  // ipcz should never ask to serialize already-transmissible objects.
  ABSL_ASSERT(!IsTransmissible());

  uint32_t num_bytes = static_cast<uint32_t>(data.size());
  uint32_t num_handles = static_cast<uint32_t>(handles.size());
  IpczResult result =
      node_->driver().Serialize(handle_, IPCZ_NO_FLAGS, nullptr, data.data(),
                                &num_bytes, handles.data(), &num_handles);
  if (result != IPCZ_RESULT_OK) {
    return result;
  }

  release();
  return IPCZ_RESULT_OK;
}

// static
DriverObject DriverObject::Deserialize(
    Ref<Node> node,
    absl::Span<const uint8_t> data,
    absl::Span<const IpczDriverHandle> handles) {
  IpczDriverHandle handle;
  IpczResult result = node->driver().Deserialize(
      node->driver_node(), data.data(), static_cast<uint32_t>(data.size()),
      handles.data(), static_cast<uint32_t>(handles.size()), IPCZ_NO_FLAGS,
      nullptr, &handle);
  if (result != IPCZ_RESULT_OK) {
    return DriverObject();
  }

  return DriverObject(std::move(node), handle);
}

}  // namespace ipcz
