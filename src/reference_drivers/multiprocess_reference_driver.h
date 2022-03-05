// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_REFERENCE_DRIVERS_MULTIPROCESS_REFERENCE_DRIVER_H_
#define IPCZ_SRC_REFERENCE_DRIVERS_MULTIPROCESS_REFERENCE_DRIVER_H_

#include "ipcz/ipcz.h"
#include "reference_drivers/channel.h"
#include "reference_drivers/os_process.h"

namespace ipcz::reference_drivers {

// A basic reference driver which supports multiprocess operation. This is also
// suitable for single-process usage, but unlike kSingleProcessReferenceDriver
// all transmissions through this driver are asynchronous.
extern const IpczDriver kMultiprocessReferenceDriver;

// A variation of the above driver which limits driver object transmission to
// transports with a broker on at least one of its two endpoints.
extern const IpczDriver kMultiprocessReferenceDriverWithForcedObjectBrokering;

// Conveys whether the local node on a transport is a broker.
enum MultiprocessTransportSource : uint8_t {
  kFromBroker,
  kFromNonBroker,
};

// Conveys whether the remote node on a transport is a broker.
enum MultiprocessTransportTarget : uint8_t {
  kToBroker,
  kToNonBroker,
};

// Creates a new driver transport from a Channel endpoint connected to the
// `remote_process` (if known) and returns an IpczDriverHandle to it.
IpczDriverHandle CreateTransportFromChannel(Channel channel,
                                            OSProcess remote_process,
                                            MultiprocessTransportSource source,
                                            MultiprocessTransportTarget target);

}  // namespace ipcz::reference_drivers

#endif  // IPCZ_SRC_REFERENCE_DRIVERS_MULTIPROCESS_REFERENCE_DRIVER_H_
