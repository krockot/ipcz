// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "test/api_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using MergePortalsTest = test::APITest;

TEST_F(MergePortalsTest, LocalMerge) {
  IpczHandle a, b;
  OpenPortals(&a, &b);

  IpczHandle c, d;
  OpenPortals(&c, &d);

  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.MergePortals(b, c, IPCZ_NO_FLAGS, nullptr));

  Put(a, "hi", {}, {});

  Parcel p = Get(d);
  EXPECT_EQ("hi", p.message);

  ClosePortals({a, d});
}

}  // namespace
}  // namespace ipcz
