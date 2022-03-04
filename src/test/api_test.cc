// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test/api_test.h"

#include "ipcz/ipcz.h"
#include "reference_drivers/single_process_reference_driver.h"
#include "third_party/abseil-cpp/absl/base/macros.h"

namespace ipcz::test {

APITest::APITest() {
  IpczResult result = ipcz.CreateNode(
      &reference_drivers::kSingleProcessReferenceDriver,
      IPCZ_INVALID_DRIVER_HANDLE, IPCZ_NO_FLAGS, nullptr, &node);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);

  result = ipcz.OpenPortals(node, IPCZ_NO_FLAGS, nullptr, &q, &p);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
}

APITest::~APITest() {
  IpczResult result = ipcz.Close(q, IPCZ_NO_FLAGS, nullptr);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);

  result = ipcz.Close(p, IPCZ_NO_FLAGS, nullptr);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);

  result = ipcz.Close(node, IPCZ_NO_FLAGS, nullptr);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
}

void APITest::OpenPortals(IpczHandle* first, IpczHandle* second) {
  TestBase::OpenPortals(node, first, second);
}

}  // namespace ipcz::test
