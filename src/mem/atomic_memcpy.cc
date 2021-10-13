// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mem/atomic_memcpy.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "third_party/abseil-cpp/absl/base/macros.h"

namespace ipcz {
namespace mem {

void AtomicReadMemcpy(void* dest, const void* src, size_t size) {
  ABSL_ASSERT(reinterpret_cast<std::uintptr_t>(dest) % 4 == 0u);
  ABSL_ASSERT(reinterpret_cast<std::uintptr_t>(src) % 4 == 0u);
  ABSL_ASSERT(size % 4 == 0u);
  for (size_t i = 0; i < size / 4; ++i) {
    reinterpret_cast<uint32_t*>(dest)[i] =
        reinterpret_cast<const std::atomic<uint32_t>*>(src)[i].load(
            std::memory_order_relaxed);
  }
}

void AtomicWriteMemcpy(void* dest, const void* src, size_t size) {
  ABSL_ASSERT(reinterpret_cast<std::uintptr_t>(dest) % 4 == 0u);
  ABSL_ASSERT(reinterpret_cast<std::uintptr_t>(src) % 4 == 0u);
  ABSL_ASSERT(size % 4 == 0u);
  for (size_t i = 0; i < size / 4; ++i) {
    reinterpret_cast<std::atomic<uint32_t>*>(dest)[i].store(
        reinterpret_cast<const uint32_t*>(src)[i], std::memory_order_relaxed);
  }
}

}  // namespace mem
}  // namespace ipcz
