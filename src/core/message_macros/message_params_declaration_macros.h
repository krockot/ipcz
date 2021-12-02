// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// no-include-guard-because-multiply-included

#define IPCZ_PROTOCOL_VERSION(n)

#define IPCZ_ENUM_BEGIN(name, type) enum name : type {
#define IPCZ_ENUM_VALUE(name, value) name = value,
#define IPCZ_ENUM_VALUE_DEFAULT(name, value) name = value, kDefaultValue = name,
#define IPCZ_ENUM_END()                                   \
  kFirstUnknownValue, kMaxValue = kFirstUnknownValue - 1, \
  }                                                       \
  ;

#define IPCZ_MSG_ID(x) static constexpr uint8_t kId = x
#define IPCZ_MSG_VERSION(x) static constexpr uint32_t kVersion = x

#define IPCZ_MSG_BEGIN(name, version_decl) \
  struct IPCZ_ALIGN(16) name##_Params {    \
    name##_Params();                       \
    ~name##_Params();                      \
    version_decl;                          \
    internal::StructHeader header;

#define IPCZ_MSG_NO_REPLY(name, id_decl, version_decl) \
  IPCZ_MSG_BEGIN(name, version_decl)
#define IPCZ_MSG_WITH_REPLY(name, id_decl, version_decl) \
  IPCZ_MSG_BEGIN(name, version_decl)
#define IPCZ_MSG_REPLY(name, version_decl) \
  IPCZ_MSG_BEGIN(name##_Reply, version_decl)

#define IPCZ_MSG_END() \
  }                    \
  ;

#define IPCZ_MSG_PARAM(type, name) type name;
#define IPCZ_MSG_HANDLE_OPTIONAL(name)
#define IPCZ_MSG_HANDLE_REQUIRED(name)
