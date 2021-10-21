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

// NOTE: not a wire structure
#define IPCZ_MSG_BEGIN(name)                      \
  struct name##_Handles {                         \
    explicit name##_Handles(os::Handle* storage); \
    ~name##_Handles();

#define IPCZ_MSG_NO_REPLY(name, id_decl, version_decl) IPCZ_MSG_BEGIN(name)
#define IPCZ_MSG_WITH_REPLY(name, id_decl, version_decl) IPCZ_MSG_BEGIN(name)
#define IPCZ_MSG_REPLY(name, version_decl) IPCZ_MSG_BEGIN(name##_Reply)

#define IPCZ_MSG_END()           \
  uint8_t sentinel_macro_helper; \
  }                              \
  ;

#define IPCZ_MSG_DATA(type, name)
#define IPCZ_MSG_HANDLE_OPTIONAL(name) os::Handle& name;
#define IPCZ_MSG_HANDLE_REQUIRED(name) os::Handle& name;
