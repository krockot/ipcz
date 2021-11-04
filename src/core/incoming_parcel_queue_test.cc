// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/incoming_parcel_queue.h"

#include "core/parcel.h"
#include "core/sequence_number.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/abseil-cpp/absl/base/macros.h"

namespace ipcz {
namespace core {
namespace {

TEST(IncomingParcelQueueTest, Empty) {
  IncomingParcelQueue q;
  EXPECT_EQ(0u, q.GetSize());
  EXPECT_TRUE(q.IsExpectingMoreParcels());
  EXPECT_FALSE(q.HasNextParcel());

  Parcel p;
  EXPECT_FALSE(q.Pop(p));
}

TEST(IncomingParcelQueueTest, SetHighestExpectedSequenceNumber) {
  IncomingParcelQueue q;
  q.SetHighestExpectedSequenceNumber(2);
  EXPECT_EQ(3u, q.GetSize());
  EXPECT_TRUE(q.IsExpectingMoreParcels());
  EXPECT_FALSE(q.HasNextParcel());

  Parcel p;
  EXPECT_FALSE(q.Pop(p));

  p = Parcel(2);
  EXPECT_TRUE(q.Push(p));
  EXPECT_FALSE(q.HasNextParcel());
  EXPECT_FALSE(q.Pop(p));
  EXPECT_EQ(3u, q.GetSize());
  EXPECT_TRUE(q.IsExpectingMoreParcels());

  p = Parcel(0);
  EXPECT_TRUE(q.Push(p));
  EXPECT_TRUE(q.HasNextParcel());
  EXPECT_TRUE(q.IsExpectingMoreParcels());
  EXPECT_TRUE(q.Pop(p));
  EXPECT_EQ(0u, p.sequence_number());

  EXPECT_EQ(2u, q.GetSize());
  EXPECT_FALSE(q.HasNextParcel());
  EXPECT_FALSE(q.Pop(p));
  EXPECT_TRUE(q.IsExpectingMoreParcels());

  p = Parcel(1);
  EXPECT_TRUE(q.Push(p));
  EXPECT_EQ(2u, q.GetSize());
  EXPECT_FALSE(q.IsExpectingMoreParcels());
  EXPECT_TRUE(q.HasNextParcel());
  EXPECT_TRUE(q.Pop(p));
  EXPECT_EQ(1u, p.sequence_number());
  EXPECT_EQ(1u, q.GetSize());
  EXPECT_FALSE(q.IsExpectingMoreParcels());
  EXPECT_TRUE(q.HasNextParcel());
  EXPECT_TRUE(q.Pop(p));
  EXPECT_EQ(2u, p.sequence_number());
  EXPECT_FALSE(q.IsExpectingMoreParcels());
  EXPECT_FALSE(q.HasNextParcel());
  EXPECT_EQ(0u, q.GetSize());
}

TEST(IncomingParcelQueueTest, SequenceTooLow) {
  IncomingParcelQueue q;

  Parcel p(0);
  EXPECT_TRUE(q.Push(p));
  EXPECT_TRUE(q.Pop(p));

  // We can't push another parcel with sequence number 0.
  EXPECT_FALSE(q.Push(p));

  // Out-of-order is of course fine.
  p = Parcel(2);
  EXPECT_TRUE(q.Push(p));
  p = Parcel(1);
  EXPECT_TRUE(q.Push(p));

  EXPECT_TRUE(q.Pop(p));
  EXPECT_TRUE(q.Pop(p));

  // But we can't revisit sequence number 1 or 2 either.
  p = Parcel(2);
  EXPECT_FALSE(q.Push(p));
  p = Parcel(1);
  EXPECT_FALSE(q.Push(p));
}

TEST(IncomingParcelQueueTest, SequenceTooHigh) {
  IncomingParcelQueue q;
  q.SetHighestExpectedSequenceNumber(4);

  Parcel p(5);
  EXPECT_FALSE(q.Push(p));
}

TEST(IncomingParcelQueueTest, SparseSequence) {
  IncomingParcelQueue q;

  // Push a sparse but eventually complete sequence of messages into a queue and
  // ensure that they can only be popped out in sequence-order.
  SequenceNumber next_expected_pop = 0;
  SequenceNumber kMessageSequence[] = {5, 2, 1,  0,  4,  3,  9,  6,
                                       8, 7, 10, 11, 12, 15, 13, 14};
  for (SequenceNumber n : kMessageSequence) {
    Parcel p(n);
    EXPECT_TRUE(q.Push(p));
    while (q.Pop(p)) {
      EXPECT_EQ(next_expected_pop, p.sequence_number());
      ++next_expected_pop;
    }
  }

  EXPECT_EQ(0u, q.GetSize());
  EXPECT_EQ(16u, next_expected_pop);
}

}  // namespace
}  // namespace core
}  // namespace ipcz
