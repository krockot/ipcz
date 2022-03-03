// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstring>

#include "ipcz/ipcz.h"
#include "reference_drivers/blob.h"
#include "reference_drivers/memory.h"
#include "reference_drivers/os_handle.h"
#include "reference_drivers/wrapped_os_handle.h"
#include "test/multinode_test.h"
#include "testing/gtest/include/gtest/gtest.h"

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

TEST_P(BoxTest, Peek) {
  IpczHandle node = CreateBrokerNode();

  constexpr const char kMessage[] = "Hello, world?";
  IpczDriverHandle blob_handle = Blob::Create(kMessage);
  IpczHandle box;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.Box(node, blob_handle, IPCZ_NO_FLAGS, nullptr, &box));

  blob_handle = IPCZ_INVALID_DRIVER_HANDLE;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.Unbox(box, IPCZ_UNBOX_PEEK, nullptr, &blob_handle));
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.Unbox(box, IPCZ_UNBOX_PEEK, nullptr, &blob_handle));
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.Unbox(box, IPCZ_UNBOX_PEEK, nullptr, &blob_handle));

  Blob* blob = Blob::FromHandle(blob_handle);
  EXPECT_EQ(kMessage, blob->message());

  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.Unbox(box, IPCZ_NO_FLAGS, nullptr, &blob_handle));

  Ref<Blob> released_blob = Blob::ReleaseFromHandle(blob_handle);
  EXPECT_EQ(blob, released_blob.get());

  CloseHandles({node});
}

TEST_P(BoxTest, TransferBox) {
  IpczHandle node0 = CreateBrokerNode();
  IpczHandle node1 = CreateNonBrokerNode();

  IpczHandle a, b;
  Connect(node0, node1, &a, &b);

  constexpr const char kMessage1[] = "Hello, world?";
  constexpr const char kMessage2[] = "Hello, world!";
  constexpr const char kMessage3[] = "Hello! World!";
  reference_drivers::Memory memory(64);
  auto mapping = memory.Map();
  memcpy(mapping.base(), kMessage1, sizeof(kMessage1));
  reference_drivers::OSHandle memory_handle = memory.TakeHandle();

  // Wrap up some data and the shared memory handle into a driver-defined blob
  // object.
  IpczDriverHandle blob_handle =
      reference_drivers::Blob::Create(kMessage2, {&memory_handle, 1});

  IpczHandle box;
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.Box(node0, blob_handle, IPCZ_NO_FLAGS, nullptr, &box));

  Put(a, kMessage3, {&box, 1});

  Parcel p;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(b, p));
  EXPECT_EQ(kMessage3, p.message);
  ASSERT_EQ(1u, p.handles.size());

  box = p.handles[0];
  EXPECT_EQ(IPCZ_RESULT_OK,
            ipcz.Unbox(box, IPCZ_NO_FLAGS, nullptr, &blob_handle));
  Ref<Blob> blob = Blob::ReleaseFromHandle(blob_handle);
  EXPECT_EQ(kMessage2, blob->message());
  ASSERT_EQ(1u, blob->handles().size());
  ASSERT_TRUE(blob->handles()[0].is_valid());
  memory = reference_drivers::Memory(std::move(blob->handles()[0]), 64);

  auto new_mapping = memory.Map();
  EXPECT_EQ(kMessage1, std::string_view(new_mapping.As<char>()));

  CloseHandles({a, b, node1, node0});
}

INSTANTIATE_MULTINODE_TEST_SUITE_P(BoxTest);

}  // namespace
}  // namespace ipcz
