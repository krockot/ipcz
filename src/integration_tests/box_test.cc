// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/ipcz.h"
#include "reference_drivers/blob.h"
#include "test/multinode_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "util/os_handle.h"

namespace ipcz {
namespace {

using BoxTest = test::MultinodeTestWithDriver;

using Blob = reference_drivers::Blob;

TEST_P(BoxTest, BoxAndUnbox) {
  IpczHandle node = CreateBrokerNode();

  constexpr const char kMessage[] = "Hello, world?";
  IpczDriverHandle blob_handle = Blob::Create(kMessage);
  IpczHandle box;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.Box(node, blob_handle, IPCZ_NO_FLAGS, nullptr, &box));

  blob_handle = IPCZ_INVALID_DRIVER_HANDLE;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.Unbox(box, IPCZ_NO_FLAGS, nullptr, &blob_handle));

  Ref<Blob> blob = Blob::ReleaseFromHandle(blob_handle);
  EXPECT_EQ(kMessage, blob->message());

  CloseHandles({node});
}

TEST_P(BoxTest, CloseBox) {
  IpczHandle node = CreateBrokerNode();

  Ref<Blob> blob = MakeRefCounted<Blob>("meh");
  Ref<Blob::RefCountedFlag> destroyed = blob->destruction_flag();
  IpczDriverHandle blob_handle = Blob::AcquireHandle(std::move(blob));

  IpczHandle box;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.Box(node, blob_handle, IPCZ_NO_FLAGS, nullptr, &box));

  EXPECT_FALSE(destroyed->get());
  EXPECT_EQ(IPCZ_RESULT_OK, ipcz.Close(box, IPCZ_NO_FLAGS, nullptr));
  EXPECT_TRUE(destroyed->get());

  CloseHandles({node});
}

INSTANTIATE_MULTINODE_TEST_SUITE_P(BoxTest);

}  // namespace
}  // namespace ipcz
