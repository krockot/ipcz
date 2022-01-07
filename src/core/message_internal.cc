// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/message_internal.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ipcz/ipcz.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {
namespace internal {

MessageBuilderBase::MessageBuilderBase(uint8_t message_id, size_t params_size)
    : data_(sizeof(MessageHeader) + params_size),
      message_id_(message_id),
      params_size_(params_size) {
  MessageHeader& h = header();
  h.size = sizeof(h);
  h.version = 0;
  h.message_id = message_id;
}

MessageBuilderBase::~MessageBuilderBase() = default;

uint32_t MessageBuilderBase::AllocateGenericArray(size_t element_size,
                                                  size_t num_elements) {
  size_t offset = Align(data_.size());
  size_t num_bytes = Align(sizeof(ArrayHeader) + element_size * num_elements);
  data_.resize(offset + num_bytes);
  ArrayHeader& header = *reinterpret_cast<ArrayHeader*>(&data_[offset]);
  header.num_bytes = static_cast<uint32_t>(num_bytes);
  header.num_elements = static_cast<uint32_t>(num_elements);
  return offset;
}

uint32_t MessageBuilderBase::AppendHandles(absl::Span<os::Handle> handles) {
  uint32_t offset = AllocateGenericArray(sizeof(OSHandleData), handles.size());
  handles_.reserve(handles_.size() + handles.size());
  for (os::Handle& handle : handles) {
    handles_.push_back(std::move(handle));
  }
  return offset;
}

MessageBuilderBase::SharedMemoryParams MessageBuilderBase::AppendSharedMemory(
    const IpczDriver& driver,
    DriverMemory memory) {
  SharedMemoryParams params{0, 0};
  if (!memory.is_valid()) {
    return params;
  }

  std::vector<uint8_t> data;
  std::vector<os::Handle> handles;
  IpczResult result = memory.Serialize(data, handles);
  if (result != IPCZ_RESULT_OK) {
    return params;
  }

  uint32_t data_param = AllocateArray<uint8_t>(data.size());
  memcpy(GetArrayData(data_param), data.data(), data.size());
  uint32_t handles_param = AppendHandles(absl::MakeSpan(handles));
  return {data_param, handles_param};
}

DriverMemory MessageBuilderBase::TakeSharedMemory(
    const IpczDriver& driver,
    const SharedMemoryParams& params) {
  return DriverMemory::Deserialize(driver,
                                   GetArrayView<uint8_t>(std::get<0>(params)),
                                   GetHandlesView(std::get<1>(params)));
}

size_t MessageBuilderBase::SerializeHandleArray(
    uint32_t param_offset,
    uint32_t base_handle_index,
    absl::Span<os::Handle> handles) {
  uint32_t array_offset =
      *reinterpret_cast<uint32_t*>(&params_data_view()[param_offset]);
  absl::Span<OSHandleData> handle_data =
      GetArrayView<OSHandleData>(array_offset);

#if defined(OS_POSIX)
  for (size_t i = 0; i < handle_data.size(); ++i) {
    OSHandleData& this_data = handle_data[i];
    this_data.header.size = sizeof(this_data);
    this_data.header.version = 0;
    ABSL_ASSERT(handles[i].is_valid());
    this_data.type = OSHandleDataType::kFileDescriptor;
    this_data.index = static_cast<uint32_t>(base_handle_index + i);
    this_data.value = 0;
  }
#endif

  return handle_data.size();
}

bool MessageBuilderBase::DeserializeDataAndHandles(
    size_t params_size,
    uint32_t params_current_version,
    absl::Span<const ParamMetadata> params_metadata,
    absl::Span<const uint8_t> data,
    absl::Span<os::Handle> handles) {
  // Copy the data into a local message object to avoid any TOCTOU issues in
  // case `data` is in unsafe shared memory.
  data_.resize(data.size());
  memcpy(data_.data(), data.data(), data.size());

  // Validate the header. The message must at least be large enough to encode a
  // v0 MessageHeader, and the encoded header size and version must make sense
  // (e.g. version 0 size must be sizeof(MessageHeader))
  if (data_.size() < sizeof(MessageHeaderV0)) {
    return false;
  }
  const auto& header = *reinterpret_cast<const MessageHeaderV0*>(data_.data());
  if (header.version == 0) {
    if (header.size != sizeof(MessageHeaderV0)) {
      return false;
    }
  } else {
    if (header.size < sizeof(MessageHeaderV0)) {
      return false;
    }
  }

  // Validate parameter data. There must be at least enough bytes following the
  // header to encode a StructHeader and to account for all parameter data.

  absl::Span<uint8_t> params_data = params_data_view();
  if (params_data.size() < sizeof(StructHeader)) {
    return false;
  }

  StructHeader& params_header =
      *reinterpret_cast<StructHeader*>(params_data.data());
  if (params_current_version < params_header.version) {
    params_header.version = params_current_version;
  }

  // The param struct's header claims to consist of more data than is present in
  // the message. CAP.
  if (params_data.size() < params_header.size) {
    return false;
  }

  // Finally, validate each parameter.
  handles_.resize(handles.size());
  for (const ParamMetadata& param : params_metadata) {
    if (param.offset + param.size > params_header.size) {
      return false;
    }

    if (param.array_element_size > 0) {
      const uint32_t array_offset =
          *reinterpret_cast<uint32_t*>(&params_data[param.offset]);
      if (array_offset >= data_.size()) {
        return false;
      }

      size_t bytes_available = data_.size() - array_offset;
      if (bytes_available < sizeof(ArrayHeader)) {
        return false;
      }

      ArrayHeader& header =
          *reinterpret_cast<ArrayHeader*>(&data_[array_offset]);
      if (bytes_available < header.num_bytes) {
        return false;
      }

      size_t max_num_elements =
          (header.num_bytes - sizeof(ArrayHeader)) / param.array_element_size;
      if (header.num_elements > max_num_elements) {
        return false;
      }

      if (param.is_handle_array) {
        OSHandleData* handle_data =
            reinterpret_cast<OSHandleData*>(&header + 1);
        for (size_t i = 0; i < header.num_elements; ++i) {
          const size_t index = handle_data[i].index;
          if (index >= handles.size() || handles_[index].is_valid()) {
            return false;
          }

          handles_[index] = std::move(handles[index]);
        }
      }
    }
  }

  return true;
}

}  // namespace internal
}  // namespace core
}  // namespace ipcz
