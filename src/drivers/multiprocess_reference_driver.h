// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_DRIVERS_MULTIPROCESS_REFERENCE_DRIVER_H_
#define IPCZ_SRC_DRIVERS_MULTIPROCESS_REFERENCE_DRIVER_H_

#include "ipcz/ipcz.h"

namespace ipcz {
namespace drivers {

// A basic reference driver which supports multiprocess operation. This is also
// suitable for single-process usage, but unlike kSingleProcessReferenceDriver
// all transmissions through this driver are asynchronous.
extern const IpczDriver kMultiprocessReferenceDriver;

}  // namespace drivers
}  // namespace ipcz

#endif  // IPCZ_SRC_DRIVERS_MULTIPROCESS_REFERENCE_DRIVER_H_
