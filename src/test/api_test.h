// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_API_API_TEST_H_
#define IPCZ_SRC_API_API_TEST_H_

#include "ipcz/ipcz.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace test {

class APITest : public testing::Test {
 public:
  APITest();
  ~APITest() override;

  IpczHandle node() const { return node_; }

  IpczAPI ipcz;

  // For convenience, every API test starts with connected portals q and p.
  IpczHandle q;
  IpczHandle p;

  // Shorthand for opening portals on node() and asserting success.
  void OpenPortals(IpczHandle* a, IpczHandle* b) {
    ASSERT_EQ(IPCZ_RESULT_OK,
              ipcz.OpenPortals(node_, IPCZ_NO_FLAGS, nullptr, a, b));
  }

  // Shorthand for closing a list of portals.
  template <size_t N>
  void ClosePortals(const IpczHandle (&handles)[N]) {
    for (IpczHandle handle : handles) {
      ASSERT_EQ(IPCZ_RESULT_OK,
                ipcz.ClosePortal(handle, IPCZ_NO_FLAGS, nullptr));
    }
  }

 private:
  IpczHandle node_;
};

}  // namespace test
}  // namespace ipcz

#endif  // IPCZ_SRC_API_API_TEST_H_
