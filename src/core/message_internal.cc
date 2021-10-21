// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/message_internal.h"

#include "ipcz/ipcz.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {
namespace internal {

void SerializeHandles(absl::Span<os::Handle> handles,
                      absl::Span<OSHandleData> data) {}

bool DeserializeData(absl::Span<const uint8_t> incoming_data,
                     absl::Span<uint8_t> out_header,
                     absl::Span<uint8_t> out_data,
                     absl::Span<uint8_t> out_handle_data) {
  return false;
}

bool DeserializeHandles(absl::Span<os::Handle> incoming_handles,
                        absl::Span<OSHandleData> incoming_data,
                        absl::Span<os::Handle> out_handles) {
  return false;
}

}  // namespace internal
}  // namespace core
}  // namespace ipcz
