// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_MESSAGE_H_
#define IPCZ_SRC_CORE_MESSAGE_H_

#include <cstdint>
#include <type_traits>

#include "core/message_internal.h"
#include "ipcz/ipcz.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/types/span.h"

#define MSG_ID(x) static constexpr uint8_t kId = x
#define MSG_VERSION(x) static constexpr uint32_t kVersion = x

#define MSG_BEGIN(name, id_decl) \
  struct IPCZ_ALIGN(8) name {    \
    id_decl;                     \
    static constexpr bool kIsReply = false;

#define MSG_BEGIN_DATA(version_decl) \
  struct IPCZ_ALIGN(8) Data {        \
    struct IPCZ_ALIGN(8) Params {    \
      version_decl;                  \
      internal::StructHeader struct_header;

#define MSG_DATA(type, name_etc) type name_etc;

#define MSG_END_DATA()                                               \
  }                                                                  \
  ;                                                                  \
  internal::MessageHeader header;                                    \
  Params params;                                                     \
  Data() {                                                           \
    memset(this, 0, sizeof(*this));                                  \
    header.size = sizeof(header);                                    \
    header.version = 0;                                              \
    header.message_id = kId;                                         \
    header.expects_reply = kExpectsReply;                            \
    header.is_reply = kIsReply;                                      \
    params.struct_header.size = sizeof(Params);                      \
    params.struct_header.version = Params::kVersion;                 \
  }                                                                  \
  }                                                                  \
  ;                                                                  \
  internal::MessageHeader& header() { return data_storage.header; }  \
  const internal::MessageHeader& header() const {                    \
    return data_storage.header;                                      \
  }                                                                  \
  Data data_storage;                                                 \
  Data::Params& params() { return data_storage.params; }             \
  const Data::Params& params() const { return data_storage.params; } \
  absl::Span<uint8_t> data() {                                       \
    return absl::MakeSpan(reinterpret_cast<uint8_t*>(&data_storage), \
                          sizeof(data_storage));                     \
  }

#define MSG_NO_DATA(version_decl)           \
  struct IPCZ_ALIGN(8) data {               \
    struct IPCZ_ALIGN(8) Params {           \
      version_decl;                         \
      internal::StructHeader struct_header; \
  MSG_END_DATA                              \
  ()

// TODO: this is not sufficient for dealing with versioning of handle attachment
// expectations. good enough for now.
#define MSG_NUM_HANDLES(n)      \
  os::Handle handle_storage[n]; \
  absl::Span<os::Handle> handles() { return absl::MakeSpan(handle_storage); }

#define MSG_HANDLE_OPTIONAL(index, name) \
  internal::OSHandleInfo name{false, handle_storage[index]};

#define MSG_HANDLE_REQUIRED(index, name) \
  internal::OSHandleInfo name{true, handle_storage[index]};

#define MSG_NO_HANDLES() \
  absl::Span<os::Handle> handles() { return {}; }

#define MSG_NO_REPLY() static constexpr bool kExpectsReply = false;

#define MSG_REPLY()                           \
  static constexpr bool kExpectsReply = true; \
  struct IPCZ_ALIGN(8) Reply {                \
    static constexpr bool kIsReply = true;    \
    static constexpr bool kExpectsReply = false;

#define MSG_END() \
  }               \
  ;

#endif  // IPCZ_SRC_CORE_MESSAGE_H_
