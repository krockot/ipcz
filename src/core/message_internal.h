// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_MESSAGE_INTERNAL_H_
#define IPCZ_SRC_CORE_MESSAGE_INTERNAL_H_

#include <cstdint>
#include <cstring>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/driver_memory.h"
#include "ipcz/ipcz.h"
#include "os/handle.h"
#include "os/process.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/abseil-cpp/absl/types/span.h"

#pragma pack(push, 1)

namespace ipcz {
namespace core {
namespace internal {

// Header which begins all messages. The header layout is versioned for
// extensibility and long-term support.
struct IPCZ_ALIGN(8) MessageHeader {
  // The size of the header in bytes.
  uint8_t size;

  // The header version in use by this message.
  uint8_t version;

  // Message ID assigned as part of a message's type definition via
  // IPCZ_MSG_BEGIN().
  uint8_t message_id;

  // Must be zero.
  uint8_t reserved[5];

  // Used for sequencing messages along a NodeLink to preserve end-to-end
  // ordering, as NodeLink messages may be transmitted either across a driver
  // transport or queues in shared memory.
  uint64_t transport_sequence_number;
};
static_assert(sizeof(MessageHeader) == 16, "Unexpected size");

using MessageHeaderV0 = MessageHeader;
using LatestMessageHeaderVersion = MessageHeaderV0;

struct IPCZ_ALIGN(8) StructHeader {
  uint32_t size;
  uint32_t version;
};
static_assert(sizeof(StructHeader) == 8, "Unexpected size");

struct IPCZ_ALIGN(8) ArrayHeader {
  uint32_t num_bytes;
  uint32_t num_elements;
};

enum OSHandleDataType : uint8_t {
  // No handle. Only valid if the handle slot was designated as optional.
  kNone = 0,

  // A POSIX file descriptor. Encoded handle value is a 0-based index into the
  // array of actual file descriptors attached to the message, indicating which
  // file descriptor to associate with this handle slot.
  kFileDescriptor = 1,

  // A Windows HANDLE. The encoded handle value is a HANDLE value. Depending on
  // the relationship between the sender and receiver, the HANDLE value may
  // belong to the sending process or the receiving process.
  kWindowsHandle = 2,

  // TODO: etc...
};

// Wire format for encoded OS handles.
//
// TODO: revisit for other platforms, version safety, extensibility.
struct IPCZ_ALIGN(8) OSHandleData {
  StructHeader header;

  OSHandleDataType type;
  uint8_t padding[3];
  uint32_t index;
  uint64_t value;
};

template <typename T>
constexpr size_t GetNumOSHandles() {
  return (sizeof(T) - sizeof(StructHeader)) / sizeof(OSHandleData);
}

struct ParamMetadata {
  uint32_t offset;
  uint32_t size;
  uint32_t array_element_size;
  bool is_handle_array;
};

class IPCZ_ALIGN(8) MessageBase {
 public:
  using SharedMemoryParams = std::tuple<uint32_t, uint32_t>;

  MessageBase(uint8_t message_id, size_t params_size);
  ~MessageBase();

  MessageHeader& header() {
    return *reinterpret_cast<MessageHeader*>(data_.data());
  }

  const MessageHeader& header() const {
    return *reinterpret_cast<const MessageHeader*>(data_.data());
  }

  absl::Span<uint8_t> data_view() { return absl::MakeSpan(data_); }
  absl::Span<uint8_t> params_data_view() {
    return absl::MakeSpan(&data_[header().size], data_.size() - header().size);
  }
  absl::Span<os::Handle> handles_view() { return absl::MakeSpan(handles_); }

  uint32_t AllocateGenericArray(size_t element_size, size_t num_elements);
  uint32_t AppendHandles(absl::Span<os::Handle> handles);

  template <typename ElementType>
  uint32_t AllocateArray(size_t num_elements) {
    return AllocateGenericArray(sizeof(ElementType), num_elements);
  }

  SharedMemoryParams AppendSharedMemory(const IpczDriver& driver,
                                        DriverMemory memory);
  DriverMemory TakeSharedMemory(const IpczDriver& driver,
                                const SharedMemoryParams& params);

  void* GetArrayData(uint32_t offset) {
    ArrayHeader& header = *reinterpret_cast<ArrayHeader*>(&data_[offset]);
    return &header + 1;
  }

  template <typename ElementType>
  absl::Span<ElementType> GetArrayView(uint32_t offset) {
    ArrayHeader& header = *reinterpret_cast<ArrayHeader*>(&data_[offset]);
    return absl::MakeSpan(reinterpret_cast<ElementType*>(&header + 1),
                          header.num_elements);
  }

  absl::Span<os::Handle> GetHandlesView(uint32_t offset) {
    absl::Span<OSHandleData> handle_data = GetArrayView<OSHandleData>(offset);
    if (handle_data.empty()) {
      return {};
    }

    // Each handle array is encoded as a contiguous series of handles, so we
    // only need the index of the first handle to index all of the handles.
    return absl::MakeSpan(handles_).subspan(handle_data[0].index,
                                            handle_data.size());
  }

  // Finalizes a message immediately before transit. The may serialize
  // additional data within the message payload, but it does not change the size
  // of the message data.
  void Serialize(absl::Span<const ParamMetadata> params,
                 const os::Process& remote_process);

 protected:
  size_t Align(size_t x) { return (x + 7) & ~7; }

  size_t SerializeHandleArray(uint32_t param_offset,
                              uint32_t base_handle_index,
                              absl::Span<os::Handle> handles,
                              const os::Process& remote_process);
  bool DeserializeDataAndHandles(
      size_t params_size,
      uint32_t params_current_version,
      absl::Span<const ParamMetadata> params_metadata,
      absl::Span<const uint8_t> data,
      absl::Span<os::Handle> handles,
      const os::Process& remote_process);

  absl::InlinedVector<uint8_t, 128> data_;
  absl::InlinedVector<os::Handle, 2> handles_;

  const uint8_t message_id_;
  const uint32_t params_size_;
};

template <typename ParamDataType>
class Message : public MessageBase {
 public:
  Message() : MessageBase(ParamDataType::kId, sizeof(ParamDataType)) {
    ParamDataType& p = *(new (&params()) ParamDataType());
    p.header.size = sizeof(p);
    p.header.version = ParamDataType::kVersion;
  }

  ~Message() = default;

  ParamDataType& params() {
    return *reinterpret_cast<ParamDataType*>(&data_[header().size]);
  }

  const ParamDataType& params() const {
    return *reinterpret_cast<const ParamDataType*>(&data_[header().size]);
  }
};

}  // namespace internal
}  // namespace core
}  // namespace ipcz

#pragma pack(pop)

#endif  // IPCZ_SRC_CORE_MESSAGE_INTERNAL_H_
