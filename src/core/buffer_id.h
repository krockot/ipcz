// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_BUFFER_ID_H_
#define IPCZ_SRC_CORE_BUFFER_ID_H_

#include <cstdint>

namespace ipcz {
namespace core {

// Identifies a shared memory buffer scoped to a NodeLink and owned by its
// NodeLinkMemory. New BufferIds are allocated atomically by either side of the
// NodeLink.
//
// TODO: Use a strong alias?
using BufferId = uint64_t;

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_BUFFER_ID_H_
