// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_BUFFER_ID_H_
#define IPCZ_SRC_IPCZ_BUFFER_ID_H_

#include <cstdint>

namespace ipcz {

// Identifies a shared memory buffer scoped to a NodeLink and owned by its
// NodeLinkMemory. New BufferIds are allocated atomically by either side of the
// NodeLink.
//
// TODO: Use a strong alias?
using BufferId = uint64_t;

constexpr BufferId kInvalidBufferId = ~0;

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_BUFFER_ID_H_
