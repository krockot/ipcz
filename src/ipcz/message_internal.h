// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_MESSAGE_INTERNAL_H_
#define IPCZ_SRC_IPCZ_MESSAGE_INTERNAL_H_

#include <cstdint>
#include <cstring>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "ipcz/driver_memory.h"
#include "ipcz/driver_object.h"
#include "ipcz/ipcz.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/abseil-cpp/absl/types/span.h"
#include "util/ref_counted.h"

#pragma pack(push, 1)

namespace ipcz {

class DriverTransport;
class Node;

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

struct IPCZ_ALIGN(8) DriverObjectData {
  uint32_t driver_data_array;
  uint16_t first_driver_handle;
  uint16_t num_driver_handles;
};

enum class ParamType {
  kData,
  kDataArray,
  kDriverObject,
  kDriverObjectArray,
};

struct ParamMetadata {
  uint32_t offset;
  uint32_t size;
  uint32_t array_element_size;
  ParamType type;
};

// TODO: These functions have been hastily ripped out of MessageBase for reuse
// by message relay in NodeLink. Fix it.
MessageHeaderV0& GetMessageHeader(absl::Span<uint8_t> message_data);
bool IsMessageHeaderValid(absl::Span<uint8_t> message_data);

template <typename ElementType>
absl::Span<ElementType> GetMessageDataArrayView(
    absl::Span<uint8_t> message_data,
    uint32_t offset) {
  ArrayHeader& header = *reinterpret_cast<ArrayHeader*>(&message_data[offset]);
  return absl::MakeSpan(reinterpret_cast<ElementType*>(&header + 1),
                        header.num_elements);
}

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
  absl::Span<DriverObject> driver_objects() {
    return absl::MakeSpan(driver_objects_);
  }
  absl::Span<IpczDriverHandle> transmissible_driver_handles() {
    return absl::MakeSpan(transmissible_driver_handles_);
  }

  uint32_t AllocateGenericArray(size_t element_size, size_t num_elements);
  uint32_t AppendDriverObjects(absl::Span<DriverObject> objects);

  void AppendDriverObject(DriverObject object, DriverObjectData& data);
  DriverObject TakeDriverObject(const DriverObjectData& data);

  template <typename ElementType>
  uint32_t AllocateArray(size_t num_elements) {
    return AllocateGenericArray(sizeof(ElementType), num_elements);
  }

  void* GetArrayData(uint32_t offset) {
    ArrayHeader& header = *reinterpret_cast<ArrayHeader*>(&data_[offset]);
    return &header + 1;
  }

  template <typename ElementType>
  absl::Span<ElementType> GetArrayView(uint32_t offset) {
    if (!offset) {
      return {};
    }
    return GetMessageDataArrayView<ElementType>(absl::MakeSpan(data_), offset);
  }

  // Helper to retrieve a typed value from the message given an absolute byte
  // offset from the start of the message.
  template <typename T>
  T& GetValueAt(uint32_t data_offset) {
    return *reinterpret_cast<T*>(&data_[data_offset]);
  }

  // Helper to retrieve a typed value from the message given a byte offset from
  // the start of the message's parameter data.
  template <typename T>
  T& GetParamValueAt(uint32_t param_offset) {
    return GetValueAt<T>(GetDataOffset(&params_data_view()[param_offset]));
  }

  // Checks and indicates whether this message can be transmitted over
  // `transport`, which depends on whether the driver is able to transmit all of
  // the attached driver objects over that transport.
  bool CanTransmitOn(const DriverTransport& transport);

  // Attempts to finalize a message for transit over `transport`, potentially
  // mutating the message data in-place. Returns true iff sucessful.
  bool Serialize(absl::Span<const ParamMetadata> params,
                 const DriverTransport& transport);

  // Adopts an already-deserialized set of data and driver objects, as embedded
  // within a relayed message. This is messy.
  // TODO: clean this up along with other details of message relay.
  void Adopt(absl::Span<uint8_t> data, absl::Span<DriverObject> objects);

 protected:
  size_t Align(size_t x) { return (x + 7) & ~7; }

  uint32_t GetDataOffset(const void* data) {
    return static_cast<uint32_t>(static_cast<const uint8_t*>(data) -
                                 data_.data());
  }

  bool DeserializeFromTransport(size_t params_size,
                                uint32_t params_current_version,
                                absl::Span<const ParamMetadata> params_metadata,
                                absl::Span<const uint8_t> data,
                                absl::Span<const IpczDriverHandle> handles,
                                const DriverTransport& transport);

  absl::InlinedVector<uint8_t, 128> data_;
  absl::InlinedVector<DriverObject, 2> driver_objects_;
  absl::InlinedVector<IpczDriverHandle, 2> transmissible_driver_handles_;

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
}  // namespace ipcz

#pragma pack(pop)

#endif  // IPCZ_SRC_IPCZ_MESSAGE_INTERNAL_H_
