// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// no-include-guard-because-multiply-included

#define IPCZ_PROTOCOL_VERSION(n) const uint32_t kProtocolVersion = n;

#define IPCZ_MSG_ID(x)
#define IPCZ_MSG_VERSION(x)

#define IPCZ_MSG_BEGIN(name, id_decl, version_decl)                           \
  name::name() = default;                                                     \
  name::~name() = default;                                                    \
  bool name::Deserialize(const DriverTransport::Message& message,             \
                         const OSProcess& remote_process) {                   \
    return DeserializeDataAndHandles(sizeof(ParamsType), kVersion,            \
                                     absl::MakeSpan(kMetadata), message.data, \
                                     message.handles, remote_process);        \
  }                                                                           \
  constexpr internal::ParamMetadata name::kMetadata[];

#define IPCZ_MSG_END()

#define IPCZ_MSG_PARAM(type, name)
#define IPCZ_MSG_PARAM_ARRAY(type, name)
#define IPCZ_MSG_PARAM_HANDLE_ARRAY(name)

#define IPCZ_MSG_PARAM_SHARED_MEMORY(name)
