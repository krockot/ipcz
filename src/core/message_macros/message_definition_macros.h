// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// no-include-guard-because-multiply-included

#define IPCZ_ENUM_BEGIN(name, type)
#define IPCZ_ENUM_VALUE(name, value)
#define IPCZ_ENUM_VALUE_DEFAULT(name, value)
#define IPCZ_ENUM_END()

#define IPCZ_MSG_ID(x)
#define IPCZ_MSG_VERSION(x)

#define IPCZ_MSG_BEGIN(name, version_decl)                              \
  constexpr size_t kDataSize_##name = sizeof(internal::MessageHeader) + \
                                      sizeof(name##_Data) +             \
                                      sizeof(name##_HandleData);        \
  name::name() {                                                        \
    memset(&header, 0, kDataSize_##name);                               \
    header.size = sizeof(header);                                       \
    header.version = 0;                                                 \
    header.message_id = kId;                                            \
    header.expects_reply = kExpectsReply;                               \
    header.is_reply = kIsReply;                                         \
    data.header.size = sizeof(data);                                    \
    data.header.version = kVersion;                                     \
    handle_data.header.size = sizeof(handle_data);                      \
    handle_data.header.version = 0;                                     \
  }                                                                     \
  name::~name() = default;                                              \
  void name::Serialize() {                                              \
    internal::SerializeHandles(                                         \
        absl::MakeSpan(&handle_storage[0], kNumHandles),                \
        absl::MakeSpan(&handle_data.handles[0], kNumHandles));          \
  }                                                                     \
  bool name::Deserialize(os::Channel::Message& message) {               \
    return internal::DeserializeData(                                   \
               message.data,                                            \
               absl::MakeSpan(reinterpret_cast<uint8_t*>(&header),      \
                              sizeof(header)),                          \
               absl::MakeSpan(reinterpret_cast<uint8_t*>(&data),        \
                              sizeof(data)),                            \
               absl::MakeSpan(reinterpret_cast<uint8_t*>(&handle_data), \
                              sizeof(handle_data))) &&                  \
           internal::DeserializeHandles(                                \
               message.handles,                                         \
               absl::MakeSpan(&handle_data.handles[0], kNumHandles),    \
               handles_view());                                         \
  }

#define IPCZ_MSG_END()

#define IPCZ_MSG_NO_REPLY(name, id_decl, version_decl) \
  IPCZ_MSG_BEGIN(name, version_decl)
#define IPCZ_MSG_WITH_REPLY(name, id_decl, version_decl) \
  IPCZ_MSG_BEGIN(name, version_decl)
#define IPCZ_MSG_REPLY(name, version_decl) \
  IPCZ_MSG_BEGIN(name##_Reply, version_decl)

#define IPCZ_MSG_DATA(type, name)
#define IPCZ_MSG_HANDLE_OPTIONAL(name)
#define IPCZ_MSG_HANDLE_REQUIRED(name)
