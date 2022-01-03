// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// no-include-guard-because-multiply-included

#define IPCZ_PROTOCOL_VERSION(n)

#define IPCZ_MSG_ID(x) static constexpr uint8_t kId = x
#define IPCZ_MSG_VERSION(x) static constexpr uint32_t kVersion = x

// TODO: this is not version safe
#define IPCZ_MSG_BEGIN(name, id_decl, version_decl) \
  struct IPCZ_ALIGN(8) name##_HandleData {          \
    version_decl;                                   \
    internal::StructHeader header;                  \
    static constexpr uint8_t kHandleCounter[] = {
#define IPCZ_MSG_END()                                         \
  }                                                            \
  ;                                                            \
  static const bool kRequiredBits[sizeof(kHandleCounter)];     \
  internal::OSHandleData handles[sizeof(kHandleCounter)] = {}; \
  }                                                            \
  ;

#define IPCZ_MSG_PARAM(type, name)

#define IPCZ_MSG_HANDLE_OPTIONAL(name) 0,
#define IPCZ_MSG_HANDLE_REQUIRED(name) 1,
