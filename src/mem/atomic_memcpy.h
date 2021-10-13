// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_MEM_ATOMIC_MEMCPY_H_
#define IPCZ_SRC_MEM_ATOMIC_MEMCPY_H_

#include <cstddef>

namespace ipcz {
namespace mem {

// Performs a copy of memory from `src` into non-overlapping memory at `dest`.
// Both addresses must be 4-byte aligned, as must be `size`. Reads from the
// memory at `src` are performed using atomic load operations.
void AtomicReadMemcpy(void* dest, const void* src, size_t size);

// Performs a copy of memory from `src` into non-overlapping memory at `dest`.
// Both addresses must be 4-byte aligned, as must be `size`. Writes into the
// memory at `dest` are performed using atomic store operations.
void AtomicWriteMemcpy(void* dest, const void* src, size_t size);

}  // namespace mem
}  // namespace ipcz

#endif  // IPCZ_SRC_MEM_ATOMIC_MEMCPY_H_
