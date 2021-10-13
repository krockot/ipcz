// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mem/seqlocked_data.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "mem/atomic_memcpy.h"
#include "third_party/abseil-cpp/absl/base/macros.h"

namespace ipcz {
namespace mem {

constexpr size_t kMaxReadAttemptsBeforeYield = 10;

void ReadSeqlockedMemory(void* dest,
                         const void* src,
                         size_t size,
                         std::atomic<uint32_t>& version) {
  size_t attempts = 0;
  while (true) {
    const uint32_t before_version = version.load(std::memory_order_acquire);
    AtomicReadMemcpy(dest, src, size);
    atomic_thread_fence(std::memory_order_acquire);
    const uint32_t after_version = version.load(std::memory_order_relaxed);
    if (after_version == before_version)
      return;

    if (++attempts >= kMaxReadAttemptsBeforeYield) {
      attempts = 0;
      std::this_thread::yield();
    }
  }
}

void WriteSeqlockedMemory(void* dest,
                          const void* src,
                          size_t size,
                          std::atomic<uint32_t>& version) {
  const uint32_t before_version =
      version.fetch_add(1, std::memory_order_relaxed);
  atomic_thread_fence(std::memory_order_release);
  ABSL_ASSERT((before_version & 1) == 0);
  AtomicWriteMemcpy(dest, src, size);
  const uint32_t after_version =
      version.fetch_add(1, std::memory_order_release);
  ABSL_ASSERT(before_version + 1 == after_version);
}

}  // namespace mem
}  // namespace ipcz
