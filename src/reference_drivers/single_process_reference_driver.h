// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_DRIVERS_SINGLE_PROCESS_REFERENCE_DRIVER_H_
#define IPCZ_SRC_DRIVERS_SINGLE_PROCESS_REFERENCE_DRIVER_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ipcz/ipcz.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz::reference_drivers {

// This is a fully synchronous driver for single-process use cases. Transmitting
// on one transport directly calls into the activity handler of its peer, so all
// node operations and therefore all ipcz operations complete synchronously from
// end to end.
extern const IpczDriver kSingleProcessReferenceDriver;

// Creates an unserializable test object. This object cannot be boxed.
IpczDriverHandle CreateUnserializableTestObject();

}  // namespace ipcz::reference_drivers

#endif  // IPCZ_SRC_DRIVERS_SINGLE_PROCESS_REFERENCE_DRIVER_H_
