// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test/api_test.h"

#include "drivers/single_process_reference_driver.h"
#include "ipcz/ipcz.h"
#include "third_party/abseil-cpp/absl/base/macros.h"

namespace ipcz {
namespace test {

APITest::APITest() {
  IpczResult result = ipcz.CreateNode(&drivers::kSingleProcessReferenceDriver,
                                      IPCZ_INVALID_DRIVER_HANDLE, IPCZ_NO_FLAGS,
                                      nullptr, &node);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);

  result = ipcz.OpenPortals(node, IPCZ_NO_FLAGS, nullptr, &q, &p);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
}

APITest::~APITest() {
  IpczResult result = ipcz.ClosePortal(q, IPCZ_NO_FLAGS, nullptr);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);

  result = ipcz.ClosePortal(p, IPCZ_NO_FLAGS, nullptr);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);

  result = ipcz.DestroyNode(node, IPCZ_NO_FLAGS, nullptr);
  ABSL_ASSERT(result == IPCZ_RESULT_OK);
}

void APITest::OpenPortals(IpczHandle* first, IpczHandle* second) {
  TestBase::OpenPortals(node, first, second);
}

}  // namespace test
}  // namespace ipcz
