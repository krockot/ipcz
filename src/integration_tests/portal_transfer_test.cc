// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "test/api_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace {

using PortalTransferTest = test::APITest;

TEST_F(PortalTransferTest, LocalToLocal) {
  IpczHandle a, b;
  OpenPortals(&a, &b);

  Put(q, "", {&a, 1});

  // Local transfer is always instant.
  Parcel m = Get(p);
  ASSERT_EQ(1u, m.portals.size());
  IpczHandle c = m.portals[0];

  // Local transfer preserves handle values.
  EXPECT_EQ(a, c);

  ClosePortals({b, c});
}

TEST_F(PortalTransferTest, LocalToLocalWithParcels) {
  IpczHandle a, b;
  OpenPortals(&a, &b);

  const std::string kMessage = "hello!";
  Put(a, kMessage);
  Put(q, {}, {&b, 1});

  Parcel m1 = Get(p);
  ASSERT_EQ(1u, m1.portals.size());
  IpczHandle c = m1.portals[0];

  Parcel m2 = Get(c);
  EXPECT_EQ(kMessage, m2.message);

  ClosePortals({a, c});
}

}  // namespace
}  // namespace ipcz
