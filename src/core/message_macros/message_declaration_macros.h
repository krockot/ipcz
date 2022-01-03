// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// no-include-guard-because-multiply-included

#define IPCZ_PROTOCOL_VERSION(n) extern const uint32_t kProtocolVersion;

#define IPCZ_MSG_ID(x) static constexpr uint8_t kId = x
#define IPCZ_MSG_VERSION(x) static constexpr uint32_t kVersion = x

#define IPCZ_MSG_BEGIN(name, id_decl, version_decl)    \
  struct IPCZ_ALIGN(16) name {                         \
    using HandlesType = name##_Handles;                \
    id_decl;                                           \
    version_decl;                                      \
    name();                                            \
    ~name();                                           \
    void Serialize();                                  \
    bool Deserialize(const DriverTransport::Message&); \
    internal::MessageHeader header;                    \
    name##_Params params;                              \
                                                       \
   private:                                            \
    name##_HandleData handle_data;                     \
                                                       \
   public:

#define IPCZ_MSG_END()                                          \
  static constexpr size_t kNumHandles =                         \
      internal::GetNumOSHandles<decltype(handle_data)>();       \
  os::Handle handle_storage[kNumHandles + 1];                   \
  HandlesType handles{&handle_storage[0]};                      \
                                                                \
  absl::Span<uint8_t> params_view() {                           \
    return absl::MakeSpan(                                      \
        reinterpret_cast<uint8_t*>(&header),                    \
        sizeof(header) + sizeof(params) + sizeof(handle_data)); \
  }                                                             \
  absl::Span<os::Handle> handles_view() {                       \
    return absl::MakeSpan(&handle_storage[0], kNumHandles);     \
  }                                                             \
  }                                                             \
  ;

#define IPCZ_MSG_PARAM(type, name)
#define IPCZ_MSG_HANDLE_OPTIONAL(name)
#define IPCZ_MSG_HANDLE_REQUIRED(name)
