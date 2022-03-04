// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_REFERENCE_DRIVERS_MULTIPROCESS_REFERENCE_DRIVER_H_
#define IPCZ_SRC_REFERENCE_DRIVERS_MULTIPROCESS_REFERENCE_DRIVER_H_

#include "ipcz/ipcz.h"
#include "reference_drivers/channel.h"
#include "reference_drivers/os_process.h"

namespace ipcz {
namespace reference_drivers {

// A basic reference driver which supports multiprocess operation. This is also
// suitable for single-process usage, but unlike kSingleProcessReferenceDriver
// all transmissions through this driver are asynchronous.
extern const IpczDriver kMultiprocessReferenceDriver;

// Creates a new driver transport from a Channel endpoint connected to the
// `remote_process` (if known) and returns an IpczDriverHandle to it.
IpczDriverHandle CreateTransportFromChannel(Channel channel,
                                            OSProcess remote_process);

}  // namespace reference_drivers
}  // namespace ipcz

#endif  // IPCZ_SRC_REFERENCE_DRIVERS_MULTIPROCESS_REFERENCE_DRIVER_H_
