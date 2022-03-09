// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstring>
#include <string_view>

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

// Creates a test driver Blob object with an inlined data payload and a shared
// memory object with an embedded message.
IpczDriverHandle CreateTestBlob(std::string_view inline_message,
                                std::string_view shm_message) {
  reference_drivers::Memory memory(shm_message.size());
  auto mapping = memory.Map();
  memcpy(mapping.base(), shm_message.data(), shm_message.size());
  reference_drivers::OSHandle memory_handle = memory.TakeHandle();
  return reference_drivers::Blob::Create(inline_message, {&memory_handle, 1});
}

bool BlobContentsMatch(IpczDriverHandle blob_handle,
                       std::string_view expected_inline_message,
                       std::string_view expected_shm_message) {
  Ref<Blob> blob = Blob::ReleaseFromHandle(blob_handle);
  if (expected_inline_message != blob->message()) {
    return false;
  }

  ABSL_ASSERT(blob->handles().size() == 1);
  ABSL_ASSERT(blob->handles()[0].is_valid());
  reference_drivers::Memory memory = reference_drivers::Memory(
      std::move(blob->handles()[0]), expected_shm_message.size());

  auto new_mapping = memory.Map();
  if (expected_shm_message != std::string_view(new_mapping.As<char>())) {
    return false;
  }

  return true;
}

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

  IpczDriverHandle blob_handle = CreateTestBlob(kMessage1, kMessage2);
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
  EXPECT_TRUE(BlobContentsMatch(blob_handle, kMessage1, kMessage2));

  CloseHandles({a, b, node1, node0});
}

TEST_P(BoxTest, TransferBoxBetweenNonBrokers) {
  IpczHandle node0 = CreateBrokerNode();
  IpczHandle node1 = CreateNonBrokerNode();
  IpczHandle node2 = CreateNonBrokerNode();

  IpczHandle a, b;
  Connect(node0, node1, &a, &b);

  IpczHandle c, d;
  Connect(node0, node2, &c, &d);

  // Create a new portal pair and send each end to one of the two non-brokers so
  // they'll establish a direct link.
  IpczHandle e, f;
  OpenPortals(node0, &e, &f);
  Put(a, "", {&e, 1});
  Put(c, "", {&f, 1});

  Parcel p;
  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(b, p));
  ASSERT_EQ(1u, p.handles.size());
  e = p.handles[0];

  EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(d, p));
  ASSERT_EQ(1u, p.handles.size());
  f = p.handles[0];

  constexpr const char kMessage1[] = "Hello, world?";
  constexpr const char kMessage2[] = "Hello, world!";
  constexpr const char kMessage3[] = "Hello! World!";

  // Send messages end-to-end in each direction from one non-broker to the
  // other, each carrying a box with some data and a driver object. This covers
  // message relaying for multinode tests running with forced object brokering
  // enabled.
  constexpr size_t kNumIterations = 10;
  for (size_t i = 0; i < kNumIterations; ++i) {
    IpczDriverHandle blob_handle = CreateTestBlob(kMessage1, kMessage2);
    IpczHandle box;
    EXPECT_EQ(IPCZ_RESULT_OK,
              ipcz.Box(node0, blob_handle, IPCZ_NO_FLAGS, nullptr, &box));

    const IpczHandle sender = i % 2 ? e : f;
    const IpczHandle receiver = i % 2 ? f : e;

    Put(sender, kMessage3, {&box, 1});

    EXPECT_EQ(IPCZ_RESULT_OK, WaitToGet(receiver, p));
    EXPECT_EQ(kMessage3, p.message);
    ASSERT_EQ(1u, p.handles.size());

    box = p.handles[0];
    EXPECT_EQ(IPCZ_RESULT_OK,
              ipcz.Unbox(box, IPCZ_NO_FLAGS, nullptr, &blob_handle));
    EXPECT_TRUE(BlobContentsMatch(blob_handle, kMessage1, kMessage2));
  }

  CloseHandles({a, b, c, d, e, f, node2, node1, node0});
}

INSTANTIATE_MULTINODE_TEST_SUITE_P(BoxTest);

}  // namespace
}  // namespace ipcz
