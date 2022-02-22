// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/box.h"

#include <utility>

#include "ipcz/ipcz.h"
#include "third_party/abseil-cpp/absl/base/macros.h"

namespace ipcz {

Box::Box(DriverObject object) : APIObject(kBox), object_(std::move(object)) {}

Box::~Box() = default;

IpczResult Box::Close() {
  object_.reset();
  return IPCZ_RESULT_OK;
}

bool Box::CanSendFrom(Portal& sender) {
  // Unserializable driver objects can't be boxed in the first place.
  ABSL_ASSERT(object_.IsSerializable());
  return true;
}

}  // namespace ipcz
