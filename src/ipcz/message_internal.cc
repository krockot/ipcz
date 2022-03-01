// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/message_internal.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "build/build_config.h"
#include "ipcz/ipcz.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/os_handle.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif

namespace ipcz {
namespace internal {

namespace {

size_t SerializeHandleArray(absl::Span<uint8_t> message_data,
                            uint32_t param_offset,
                            uint32_t base_handle_index,
                            absl::Span<OSHandle> handles,
                            const OSProcess& remote_process) {
  absl::Span<uint8_t> params_data = GetMessageParamsData(message_data);
  uint32_t array_offset =
      *reinterpret_cast<uint32_t*>(&params_data[param_offset]);
  absl::Span<OSHandleData> handle_data =
      GetMessageDataArrayView<OSHandleData>(message_data, array_offset);

  for (size_t i = 0; i < handle_data.size(); ++i) {
    OSHandleData& this_data = handle_data[i];
    this_data.header.size = sizeof(this_data);
    this_data.header.version = 0;
    ABSL_ASSERT(handles[i].is_valid());

#if defined(OS_POSIX)
    this_data.type = OSHandleDataType::kFileDescriptor;
    this_data.index = static_cast<uint32_t>(base_handle_index + i);
    this_data.value = 0;
#elif BUILDFLAG(IS_WIN)
    HANDLE h = handles[i].ReleaseHandle();
    if (remote_process.is_valid()) {
      // If we have a valid handle to the remote process, we assume we must
      // duplicate every HANDLE value to it before encoding them.
      HANDLE remote_handle = INVALID_HANDLE_VALUE;
      BOOL ok = ::DuplicateHandle(
          ::GetCurrentProcess(), h, remote_process.handle(), &remote_handle, 0,
          FALSE, DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS);
      ABSL_ASSERT(ok);
      h = remote_handle;
    }
    this_data.type = OSHandleDataType::kWindowsHandle;
    this_data.index = static_cast<uint32_t>(base_handle_index + i);
    this_data.value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(h));
#endif
  }

  return handle_data.size();
}

}  // namespace

MessageHeaderV0& GetMessageHeader(absl::Span<uint8_t> message_data) {
  return *reinterpret_cast<MessageHeaderV0*>(message_data.data());
}

bool IsMessageHeaderValid(absl::Span<uint8_t> message_data) {
  return message_data.size() >= sizeof(internal::MessageHeaderV0) &&
         GetMessageHeader(message_data).size <= message_data.size();
}

absl::Span<uint8_t> GetMessageParamsData(absl::Span<uint8_t> message_data) {
  ABSL_ASSERT(IsMessageHeaderValid(message_data));
  return message_data.subspan(GetMessageHeader(message_data).size);
}

void SerializeMessageHandleData(absl::Span<uint8_t> message_data,
                                absl::Span<OSHandle> handles,
                                absl::Span<const ParamMetadata> params,
                                const OSProcess& remote_process) {
  size_t base_handle_index = 0;
  for (const ParamMetadata& param : params) {
    if (param.is_handle_array) {
      size_t num_handles =
          SerializeHandleArray(message_data, param.offset, base_handle_index,
                               handles, remote_process);
      handles = handles.subspan(num_handles);
      base_handle_index += num_handles;
    }
  }
}

MessageBase::MessageBase(uint8_t message_id, size_t params_size)
    : data_(sizeof(MessageHeader) + params_size),
      message_id_(message_id),
      params_size_(params_size) {
  MessageHeader& h = header();
  h.size = sizeof(h);
  h.version = 0;
  h.message_id = message_id;
}

MessageBase::~MessageBase() = default;

uint32_t MessageBase::AllocateGenericArray(size_t element_size,
                                           size_t num_elements) {
  size_t offset = Align(data_.size());
  size_t num_bytes = Align(sizeof(ArrayHeader) + element_size * num_elements);
  data_.resize(offset + num_bytes);
  ArrayHeader& header = *reinterpret_cast<ArrayHeader*>(&data_[offset]);
  header.num_bytes = static_cast<uint32_t>(num_bytes);
  header.num_elements = static_cast<uint32_t>(num_elements);
  return offset;
}

uint32_t MessageBase::AppendHandles(absl::Span<OSHandle> handles) {
  uint32_t offset = AllocateGenericArray(sizeof(OSHandleData), handles.size());
  handles_.reserve(handles_.size() + handles.size());
  for (OSHandle& handle : handles) {
    handles_.push_back(std::move(handle));
  }
  return offset;
}

MessageBase::SharedMemoryParams MessageBase::AppendSharedMemory(
    DriverMemory memory) {
  SharedMemoryParams params{0, 0};
  if (!memory.is_valid()) {
    return params;
  }

  std::vector<uint8_t> data;
  std::vector<OSHandle> handles;
  IpczResult result = memory.Serialize(data, handles);
  if (result != IPCZ_RESULT_OK) {
    return params;
  }

  uint32_t data_param = AllocateArray<uint8_t>(data.size());
  memcpy(GetArrayData(data_param), data.data(), data.size());
  uint32_t handles_param = AppendHandles(absl::MakeSpan(handles));
  return {data_param, handles_param};
}

DriverMemory MessageBase::TakeSharedMemory(Ref<Node> node,
                                           const SharedMemoryParams& params) {
  return DriverMemory::Deserialize(std::move(node),
                                   GetArrayView<uint8_t>(std::get<0>(params)),
                                   GetHandlesView(std::get<1>(params)));
}

void MessageBase::Serialize(absl::Span<const ParamMetadata> params,
                            const OSProcess& remote_process) {
  SerializeMessageHandleData(data_view(), handles_view(), params,
                             remote_process);
#if BUILDFLAG(IS_WIN)
  // On Windows, all handles have been released and absorbed into the data
  // payload.
  handles_.clear();
#endif
}

bool MessageBase::DeserializeDataAndHandles(
    size_t params_size,
    uint32_t params_current_version,
    absl::Span<const ParamMetadata> params_metadata,
    absl::Span<const uint8_t> data,
    absl::Span<OSHandle> handles,
    const OSProcess& remote_process) {
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

  const auto& message_header =
      *reinterpret_cast<const MessageHeaderV0*>(data_.data());
  if (message_header.version == 0) {
    if (message_header.size != sizeof(MessageHeaderV0)) {
      return false;
    }
  } else {
    if (message_header.size < sizeof(MessageHeaderV0)) {
      return false;
    }
  }

  if (message_header.size > data_.size()) {
    return false;
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
      if (bytes_available < header.num_bytes ||
          header.num_bytes < sizeof(ArrayHeader)) {
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
#if BUILDFLAG(IS_WIN)
          if (index >= handles_.size()) {
            handles_.resize(index + 1);
          }
          if (handles_[index].is_valid()) {
            return false;
          }

          HANDLE h = reinterpret_cast<HANDLE>(
              static_cast<uintptr_t>(handle_data[i].value));
          if (h != INVALID_HANDLE_VALUE && remote_process.is_valid()) {
            HANDLE local_handle = INVALID_HANDLE_VALUE;
            BOOL ok = ::DuplicateHandle(
                remote_process.handle(), h, ::GetCurrentProcess(),
                &local_handle, 0, FALSE,
                DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE);
            ABSL_ASSERT(ok);
            h = local_handle;
          }

          OSHandle handle(h);
          if (!handle.is_valid()) {
            return false;
          }

          handles_[index] = std::move(handle);
#else
          if (index >= handles.size() || handles_[index].is_valid()) {
            return false;
          }

          handles_[index] = std::move(handles[index]);
#endif
        }
      }
    }
  }

  return true;
}

}  // namespace internal
}  // namespace ipcz
