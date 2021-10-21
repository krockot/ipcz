// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// no-include-guard-because-multiply-included

#define IPCZ_ENUM_BEGIN(name, type)
#define IPCZ_ENUM_VALUE(name, value)
#define IPCZ_ENUM_VALUE_DEFAULT(name, value)
#define IPCZ_ENUM_END()

#define IPCZ_MSG_ID(x) static constexpr uint8_t kId = x
#define IPCZ_MSG_VERSION(x) static constexpr uint32_t kVersion = x

#define IPCZ_MSG_BEGIN(name, id_decl, version_decl, reply_decl) \
  struct IPCZ_ALIGN(8) name {                                   \
    using HandlesType = name##_Handles;                         \
    id_decl;                                                    \
    version_decl;                                               \
    reply_decl;                                                 \
    static constexpr bool kIsReply = false;                     \
    name();                                                     \
    ~name();                                                    \
    void Serialize();                                           \
    bool Deserialize(os::Channel::Message&);                    \
    internal::MessageHeader header;                             \
    name##_Data data;                                           \
                                                                \
   private:                                                     \
    name##_HandleData handle_data;                              \
                                                                \
   public:

#define IPCZ_MSG_REPLY_DECL(name)             \
  static constexpr bool kExpectsReply = true; \
  using Reply = name##_Reply
#define IPCZ_MSG_NO_REPLY_DECL() static constexpr bool kExpectsReply = false

#define IPCZ_MSG_NO_REPLY(name, id_decl, version_decl) \
  IPCZ_MSG_BEGIN(name, id_decl, version_decl, IPCZ_MSG_NO_REPLY_DECL())
#define IPCZ_MSG_WITH_REPLY(name, id_decl, version_decl) \
  struct name##_Reply;                                   \
  IPCZ_MSG_BEGIN(name, id_decl, version_decl, IPCZ_MSG_REPLY_DECL(name))
#define IPCZ_MSG_REPLY(name, version_decl)                               \
  IPCZ_MSG_BEGIN(name##_Reply, static constexpr uint8_t kId = name::kId, \
                 version_decl, IPCZ_MSG_NO_REPLY_DECL())

#define IPCZ_MSG_END()                                        \
  static constexpr size_t kNumHandles =                       \
      internal::GetNumOSHandles<decltype(handle_data)>();     \
  os::Handle handle_storage[kNumHandles + 1];                 \
  HandlesType handles{&handle_storage[0]};                    \
                                                              \
  absl ::Span<uint8_t> data_view() {                          \
    return absl ::MakeSpan(                                   \
        reinterpret_cast<uint8_t*>(&header),                  \
        sizeof(header) + sizeof(data) + sizeof(handle_data)); \
  }                                                           \
  absl ::Span<os ::Handle> handles_view() {                   \
    return absl ::MakeSpan(&handle_storage[0], kNumHandles);  \
  }                                                           \
  }                                                           \
  ;

#define IPCZ_MSG_DATA(type, name)
#define IPCZ_MSG_HANDLE_OPTIONAL(name)
#define IPCZ_MSG_HANDLE_REQUIRED(name)
