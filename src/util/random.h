// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_UTIL_RANDOM_H_
#define IPCZ_SRC_UTIL_RANDOM_H_

#include "third_party/abseil-cpp/absl/numeric/int128.h"

namespace ipcz {

absl::uint128 RandomUint128();

}  // namespace ipcz

#endif  // IPCZ_SRC_UTIL_RANDOM_H_
