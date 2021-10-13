// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test/api_test.h"

namespace ipcz {
namespace test {

APITest::APITest() {
  ipcz.size = sizeof(ipcz);
  IpczGetAPI(&ipcz);
  ipcz.CreateNode(IPCZ_NO_FLAGS, nullptr, &node_);
}

APITest::~APITest() {
  ipcz.DestroyNode(node_, IPCZ_NO_FLAGS, nullptr);
}

}  // namespace test
}  // namespace ipcz
