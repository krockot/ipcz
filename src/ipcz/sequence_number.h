// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_SEQUENCE_NUMBER_H_
#define IPCZ_SRC_IPCZ_SEQUENCE_NUMBER_H_

#include <cstdint>

namespace ipcz {

// Used to order parcels relative to other parcels in the same conceptual queue,
// i.e. originating from the same portal and arriving at the same portal.
//
// TODO: strong alias?
using SequenceNumber = uint64_t;

constexpr SequenceNumber kInvalidSequenceNumber =
    ~static_cast<SequenceNumber>(0);

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_SEQUENCE_NUMBER_H_
