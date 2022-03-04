// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_TEST_API_TEST_H_
#define IPCZ_SRC_TEST_API_TEST_H_

#include "ipcz/ipcz.h"
#include "test/test_base.h"

namespace ipcz::test {

// Base class for pure API tests which are generally designed to test the
// behavior of a single API call under specific conditions. Such tests only use
// a single node, and many use only a single local portal pair, so these things
// are provided by the APITest fixture.
class APITest : public TestBase {
 public:
  APITest();
  ~APITest() override;

  IpczHandle node;

  // Two entangled portals which are opened at construction time. These are
  // owned by the fixture and automatically closed on teardown, but tests
  // otherwise control them exclusively and can use them to test portal
  // behavior.
  IpczHandle q;
  IpczHandle p;

  void OpenPortals(IpczHandle* first, IpczHandle* second);
};

}  // namespace ipcz::test

#endif  // IPCZ_SRC_TEST_API_TEST_H_
