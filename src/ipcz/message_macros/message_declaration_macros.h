// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// no-include-guard-because-multiply-included

#define IPCZ_PROTOCOL_VERSION(n) extern const uint32_t kProtocolVersion;

#define IPCZ_MSG_ID(x) static constexpr uint8_t kId = x
#define IPCZ_MSG_VERSION(x) static constexpr uint32_t kVersion = x

#define IPCZ_MSG_BEGIN(name, id_decl, version_decl)      \
  class name : public internal::Message<name##_Params> { \
   public:                                               \
    using ParamsType = name##_Params;                    \
    id_decl;                                             \
    version_decl;                                        \
    name();                                              \
    ~name();                                             \
    bool Deserialize(const DriverTransport::Message&,    \
                     const OSProcess& remote_process);   \
                                                         \
    static constexpr internal::ParamMetadata kMetadata[] = {
#define IPCZ_MSG_END() \
  }                    \
  ;                    \
  }                    \
  ;

#define IPCZ_MSG_PARAM(type, name) \
  {offsetof(ParamsType, name), sizeof(ParamsType::name), 0, false},
#define IPCZ_MSG_PARAM_ARRAY(type, name) \
  {offsetof(ParamsType, name), sizeof(ParamsType::name), sizeof(type), false},
#define IPCZ_MSG_PARAM_HANDLE_ARRAY(name)                \
  {offsetof(ParamsType, name), sizeof(ParamsType::name), \
   sizeof(internal::OSHandleData), true},

#define IPCZ_MSG_PARAM_SHARED_MEMORY(name)   \
  IPCZ_MSG_PARAM_ARRAY(uint8_t, name##_data) \
  IPCZ_MSG_PARAM_HANDLE_ARRAY(name##_handles)
