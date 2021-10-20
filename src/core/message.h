// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_MESSAGE_H_
#define IPCZ_SRC_CORE_MESSAGE_H_

#include <cstdint>
#include <type_traits>

#include "core/message_internal.h"
#include "ipcz/ipcz.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/types/span.h"

#define MSG_ID(x) static constexpr uint8_t kId = x
#define MSG_VERSION(x) static constexpr uint8_t kCurrentVersion = x

#define MSG_START(name, id_decl, version_decl)                                \
  struct IPCZ_ALIGN(8) name : Message {                                       \
    id_decl;                                                                  \
    version_decl;                                                             \
    name() : Message(kId, sizeof(name) - sizeof(Message), kCurrentVersion) {} \
    absl::Span<uint8_t> data() {                                              \
      return absl::MakeSpan(reinterpret_cast<uint8_t*>(this), sizeof(*this)); \
    }                                                                         \
    absl::Span<os::Handle> handles() { return {}; }

#define MSG_FIELD(type, name_etc) type name_etc;
#define MSG_END() \
  }               \
  ;

namespace ipcz {
namespace core {

struct IPCZ_ALIGN(8) Message {
  explicit Message(uint8_t id, uint32_t size, uint32_t version) {
    memset(&header, 0, sizeof(header));
    header.size = sizeof(header);
    header.version = 0;
    header.message_id = id;
    params_header.size = size;
    params_header.version = version;
  }

  internal::MessageHeader header;
  internal::StructHeader params_header;
};
static_assert(sizeof(Message) == 16, "Unexpected size");

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_MESSAGE_H_
