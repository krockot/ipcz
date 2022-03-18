// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_SEQUENCE_NUMBER_H_
#define IPCZ_SRC_IPCZ_SEQUENCE_NUMBER_H_

#include <cstdint>

namespace ipcz {

// Used to number arbitrary objects in a sequence.
//
// More specifically this is used by ipcz to maintain relative ordering of
// parcels against other parcels from the same source portal, and NodeLink
// messages against other NodeLink messages from the NodeLink endpoint.
//
// TODO: strong alias?
using SequenceNumber = uint64_t;

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_SEQUENCE_NUMBER_H_
