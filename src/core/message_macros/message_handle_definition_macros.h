// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// no-include-guard-because-multiply-included

#define IPCZ_PROTOCOL_VERSION(n)

#define IPCZ_ENUM_BEGIN(name, type)
#define IPCZ_ENUM_VALUE(name, value)
#define IPCZ_ENUM_VALUE_DEFAULT(name, value)
#define IPCZ_ENUM_END()

#define IPCZ_MSG_ID(x)
#define IPCZ_MSG_VERSION(x)

#define IPCZ_MSG_BEGIN(name)                   \
  name##_Handles::~name##_Handles() = default; \
  name##_Handles::name##_Handles(os::Handle* storage) :

#define IPCZ_MSG_NO_REPLY(name, id_decl, version_decl) IPCZ_MSG_BEGIN(name)
#define IPCZ_MSG_WITH_REPLY(name, id_decl, version_decl) IPCZ_MSG_BEGIN(name)
#define IPCZ_MSG_REPLY(name, version_decl) IPCZ_MSG_BEGIN(name##_Reply)

#define IPCZ_MSG_END() \
  sentinel_macro_helper(0) {}

#define IPCZ_MSG_PARAM(type, name)
#define IPCZ_MSG_HANDLE_OPTIONAL(name) name(*storage++),
#define IPCZ_MSG_HANDLE_REQUIRED(name) name(*storage++),
