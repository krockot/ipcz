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

 private:
  IpczHandle node_;
};

}  // namespace test
}  // namespace ipcz

#endif  // IPCZ_SRC_API_API_TEST_H_
