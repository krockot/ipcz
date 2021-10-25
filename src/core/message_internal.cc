// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/message_internal.h"

#include <algorithm>

#include "ipcz/ipcz.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {
namespace internal {

void SerializeHandles(absl::Span<os::Handle> handles,
                      absl::Span<OSHandleData> out_handle_data) {
#if defined(OS_POSIX)
  // Only valid handles are attached, but all handle fields in a message have
  // a descriptor. Ensure each descriptor is properly encoded: descriptors for
  // valid handle fields encode the index of the attached handle, and other
  // descriptors encode a their type as kNone.
  uint64_t next_valid_handle_index = 0;
  for (size_t i = 0; i < handles.size(); ++i) {
    OSHandleData& this_data = out_handle_data[i];
    if (handles[i].is_valid()) {
      this_data.type = OSHandleDataType::kFileDescriptor;
      this_data.value = next_valid_handle_index++;
    } else {
      this_data.type = OSHandleDataType::kNone;
      this_data.value = 0;
    }
  }
#endif
}

bool DeserializeData(absl::Span<const uint8_t> incoming_bytes,
                     uint32_t current_params_version,
                     absl::Span<uint8_t> out_header_storage,
                     absl::Span<uint8_t> out_data_storage,
                     absl::Span<uint8_t> out_handle_data_storage) {
  // Step 1: copy and validate the header. The message must at least be large
  // enough to encode a v0 MessageHeader, and the encoded header size and
  // version must make sense (e.g. version 0 size must be sizeof(MessageHeader))

  // Callers will always deserialize into the newest header type.
  ABSL_ASSERT(out_header_storage.size() == sizeof(LatestMessageHeaderVersion));
  ABSL_ASSERT(out_header_storage.size() >= sizeof(MessageHeaderV0));
  auto& out_header =
      *reinterpret_cast<LatestMessageHeaderVersion*>(out_header_storage.data());

  if (incoming_bytes.size() < sizeof(MessageHeaderV0)) {
    return false;
  }
  const auto& in_header =
      *reinterpret_cast<const MessageHeaderV0*>(incoming_bytes.data());

  memcpy(&out_header, &in_header, sizeof(in_header));

  if (out_header.version == 0) {
    if (out_header.size != sizeof(MessageHeaderV0)) {
      return false;
    }
  } else {
    if (out_header.size < sizeof(MessageHeaderV0)) {
      return false;
    }
  }

  // TODO: validate flags somewhere

  // Step 2: copy and validate parameter data. There must be at least enough
  // bytes following the header to encode a StructHeader.

  absl::Span<const uint8_t> bytes_after_header(
      incoming_bytes.data() + out_header.size,
      incoming_bytes.size() - out_header.size);
  if (bytes_after_header.size() < sizeof(StructHeader)) {
    return false;
  }

  const StructHeader& in_params_header =
      *reinterpret_cast<const StructHeader*>(bytes_after_header.data());
  StructHeader& out_params_header =
      *reinterpret_cast<StructHeader*>(out_data_storage.data());
  ABSL_ASSERT(out_data_storage.size() >= sizeof(StructHeader));

  memcpy(&out_params_header, &in_params_header, sizeof(StructHeader));
  if (current_params_version < out_params_header.version) {
    out_params_header.version = current_params_version;
  }

  // The param struct's header claims to consist of more data than is present in
  // the message. CAP.
  if (bytes_after_header.size() < out_params_header.size) {
    return false;
  }

  // Sender may be older or newer than this version, so copy the minimum between
  // the current known version's size and the received header's claimed size.
  memcpy(&out_params_header + 1, &in_params_header + 1,
         std::min(static_cast<size_t>(out_params_header.size),
                  out_data_storage.size()) -
             sizeof(StructHeader));

  // Step 3: copy and validate handle data. There must be at least enough bytes
  // following the parameter data to encode a StructHeader.

  absl::Span<const uint8_t> bytes_after_params(
      bytes_after_header.data() + out_params_header.size,
      bytes_after_header.size() - out_params_header.size);
  if (bytes_after_params.size() < sizeof(StructHeader)) {
    return false;
  }

  // TODO handle data needs to be versioned properly. probably more generically
  // something like a common MessageMetadata struct following the params struct,
  // with v0 MessageMetadata containing an optional array of OSHandleData as its
  // only field.

  if (bytes_after_params.size() != out_handle_data_storage.size()) {
    return false;
  }
  memcpy(out_handle_data_storage.data(), bytes_after_params.data(),
         out_handle_data_storage.size());
  return true;
}

bool DeserializeHandles(absl::Span<os::Handle> incoming_handles,
                        absl::Span<const OSHandleData> incoming_data,
                        absl::Span<os::Handle> out_handles) {
  os::Handle* out_handle = out_handles.data();
  for (const OSHandleData& data : incoming_data) {
    if (data.header.size < sizeof(OSHandleData)) {
      return false;
    }
    if (data.type == OSHandleDataType::kNone) {
      out_handle->reset();
      continue;
    }
#if defined(OS_POSIX)
    if (data.type == OSHandleDataType::kFileDescriptor) {
      size_t fd_index = static_cast<size_t>(data.value);
      if (fd_index >= incoming_handles.size()) {
        return false;
      }

      os::Handle& handle = incoming_handles[fd_index];
      if (!handle.is_valid()) {
        return false;
      }
      *out_handle = std::move(handle);
    }
#endif
  }
  return true;
}

}  // namespace internal
}  // namespace core
}  // namespace ipcz
