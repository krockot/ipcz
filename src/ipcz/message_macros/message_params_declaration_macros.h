// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// no-include-guard-because-multiply-included

#define IPCZ_PROTOCOL_VERSION(n)

#define IPCZ_MSG_ID(x) static constexpr uint8_t kId = x
#define IPCZ_MSG_VERSION(x) static constexpr uint32_t kVersion = x

#define IPCZ_MSG_BEGIN(name, id_decl, version_decl) \
  struct IPCZ_ALIGN(8) name##_Params {              \
    name##_Params();                                \
    ~name##_Params();                               \
    id_decl;                                        \
    version_decl;                                   \
    internal::StructHeader header;

#define IPCZ_MSG_END() \
  }                    \
  ;

#define IPCZ_MSG_PARAM(type, name) type name;
#define IPCZ_MSG_PARAM_ARRAY(type, name) uint32_t name;
#define IPCZ_MSG_PARAM_HANDLE_ARRAY(name) uint32_t name;

#define IPCZ_MSG_PARAM_SHARED_MEMORY(name)                                   \
  IPCZ_MSG_PARAM_ARRAY(uint8_t, name##_data)                                 \
  IPCZ_MSG_PARAM_HANDLE_ARRAY(name##_handles)                                \
  internal::MessageBase::SharedMemoryParams name() {                         \
    return std::tie(name##_data, name##_handles);                            \
  }                                                                          \
  void set_##name(const internal::MessageBase::SharedMemoryParams& params) { \
    std::tie(name##_data, name##_handles) = params;                          \
  }
