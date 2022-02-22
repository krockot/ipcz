// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/driver_object.h"

#include <cstdint>
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

bool DriverObject::IsSerializable() const {
  if (!is_valid()) {
    return false;
  }

  uint32_t num_bytes = 0;
  uint32_t num_os_handles = 0;
  IpczResult result = node_->driver().Serialize(
      handle_, IPCZ_NO_FLAGS, nullptr, nullptr, &num_bytes, 0, &num_os_handles);
  return result == IPCZ_RESULT_RESOURCE_EXHAUSTED;
}

DriverObject::SerializedDimensions DriverObject::GetSerializedDimensions()
    const {
  ABSL_ASSERT(IsSerializable());
  DriverObject::SerializedDimensions dimensions = {};
  IpczResult result = node_->driver().Serialize(
      handle_, IPCZ_NO_FLAGS, nullptr, nullptr, &dimensions.num_bytes, nullptr,
      &dimensions.num_os_handles);
  ABSL_ASSERT(result == IPCZ_RESULT_RESOURCE_EXHAUSTED);
  ABSL_ASSERT(dimensions.num_bytes > 0 || dimensions.num_os_handles > 0);
  return dimensions;
}

IpczResult DriverObject::Serialize(absl::Span<uint8_t> data,
                                   absl::Span<OSHandle> handles) {
  if (!is_valid()) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  absl::InlinedVector<IpczOSHandle, 4> os_handles(handles.size());
  for (auto& handle : os_handles) {
    handle.size = sizeof(handle);
  }

  uint32_t num_bytes = static_cast<uint32_t>(data.size());
  uint32_t num_os_handles = static_cast<uint32_t>(handles.size());
  IpczResult result =
      node_->driver().Serialize(handle_, IPCZ_NO_FLAGS, nullptr, data.data(),
                                &num_bytes, os_handles.data(), &num_os_handles);
  if (result != IPCZ_RESULT_OK) {
    return result;
  }

  release();
  for (size_t i = 0; i < num_os_handles; ++i) {
    handles[i] = OSHandle::FromIpczOSHandle(os_handles[i]);
  }
  return IPCZ_RESULT_OK;
}

// static
DriverObject DriverObject::Deserialize(Ref<Node> node,
                                       absl::Span<const uint8_t> data,
                                       absl::Span<OSHandle> handles) {
  std::vector<IpczOSHandle> os_handles(handles.size());
  bool fail = false;
  for (size_t i = 0; i < handles.size(); ++i) {
    os_handles[i].size = sizeof(os_handles[i]);
    bool ok = OSHandle::ToIpczOSHandle(std::move(handles[i]), &os_handles[i]);
    if (!ok) {
      fail = true;
    }
  }

  if (fail) {
    return DriverObject();
  }

  IpczDriverHandle handle;
  IpczResult result = node->driver().Deserialize(
      node->driver_node(), data.data(), static_cast<uint32_t>(data.size()),
      os_handles.data(), static_cast<uint32_t>(os_handles.size()),
      IPCZ_NO_FLAGS, nullptr, &handle);
  if (result != IPCZ_RESULT_OK) {
    return DriverObject();
  }

  return DriverObject(std::move(node), handle);
}

}  // namespace ipcz
