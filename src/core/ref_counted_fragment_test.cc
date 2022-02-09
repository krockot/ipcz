// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/ref_counted_fragment.h"

#include <atomic>
#include <tuple>

#include "core/fragment.h"
#include "core/fragment_ref.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ipcz {
namespace core {
namespace {

using RefCountedFragmentTest = testing::Test;

using TestObject = RefCountedFragment;

TEST_F(RefCountedFragmentTest, NullRef) {
  FragmentRef<TestObject> ref;
  EXPECT_FALSE(ref);

  ref.reset();
  EXPECT_FALSE(ref);

  FragmentRef<TestObject> other1 = ref;
  EXPECT_FALSE(ref);
  EXPECT_FALSE(other1);

  FragmentRef<TestObject> other2 = std::move(ref);
  EXPECT_FALSE(ref);
  EXPECT_FALSE(other2);

  ref = other1;
  EXPECT_FALSE(ref);
  EXPECT_FALSE(other1);

  ref = std::move(other2);
  EXPECT_FALSE(ref);
  EXPECT_FALSE(other2);
}

TEST_F(RefCountedFragmentTest, SimpleRef) {
  TestObject object;

  FragmentRef<TestObject> ref(RefCountedFragment::kUnmanagedRef,
                              Fragment(FragmentDescriptor(0, 0, 0), &object));
  EXPECT_EQ(1, object.ref_count_for_testing());
  ref.reset();
  EXPECT_EQ(0, object.ref_count_for_testing());
}

TEST_F(RefCountedFragmentTest, Copy) {
  TestObject object1;

  FragmentRef<TestObject> ref1(RefCountedFragment::kUnmanagedRef,
                               Fragment(FragmentDescriptor(0, 0, 0), &object1));

  FragmentRef<TestObject> other1 = ref1;
  EXPECT_EQ(2, object1.ref_count_for_testing());
  ref1.reset();
  EXPECT_EQ(1, object1.ref_count_for_testing());
  EXPECT_FALSE(ref1);
  other1.reset();
  EXPECT_EQ(0, object1.ref_count_for_testing());
  EXPECT_FALSE(other1);

  ref1 =
      FragmentRef<TestObject>(RefCountedFragment::kUnmanagedRef,
                              Fragment(FragmentDescriptor(0, 0, 0), &object1));

  TestObject object2;
  auto ref2 =
      FragmentRef<TestObject>(RefCountedFragment::kUnmanagedRef,
                              Fragment(FragmentDescriptor(0, 0, 0), &object2));
  EXPECT_EQ(1, object1.ref_count_for_testing());
  EXPECT_EQ(1, object2.ref_count_for_testing());
  ref2 = ref1;
  EXPECT_EQ(2, object1.ref_count_for_testing());
  EXPECT_EQ(0, object2.ref_count_for_testing());
  EXPECT_TRUE(ref1);
  EXPECT_TRUE(ref2);
  ref1.reset();
  EXPECT_EQ(1, object1.ref_count_for_testing());
  EXPECT_EQ(0, object2.ref_count_for_testing());
  EXPECT_FALSE(ref1);
  ref2.reset();
  EXPECT_EQ(0, object1.ref_count_for_testing());
  EXPECT_EQ(0, object2.ref_count_for_testing());
  EXPECT_FALSE(ref2);
}

TEST_F(RefCountedFragmentTest, Move) {
  TestObject object1;
  TestObject object2;

  FragmentRef<TestObject> ref1(RefCountedFragment::kUnmanagedRef,
                               Fragment(FragmentDescriptor(0, 0, 0), &object1));
  EXPECT_EQ(1, ref1.ref_count_for_testing());

  FragmentRef<TestObject> other1 = std::move(ref1);
  EXPECT_EQ(1, object1.ref_count_for_testing());
  EXPECT_TRUE(other1);
  EXPECT_FALSE(ref1);
  other1.reset();
  EXPECT_FALSE(other1);
  EXPECT_EQ(0, object1.ref_count_for_testing());

  ref1 =
      FragmentRef<TestObject>(RefCountedFragment::kUnmanagedRef,
                              Fragment(FragmentDescriptor(0, 0, 0), &object1));
  FragmentRef<TestObject> ref2(RefCountedFragment::kUnmanagedRef,
                               Fragment(FragmentDescriptor(0, 0, 0), &object2));

  EXPECT_TRUE(ref1);
  EXPECT_TRUE(ref2);
  EXPECT_EQ(1, object1.ref_count_for_testing());
  EXPECT_EQ(1, object2.ref_count_for_testing());
  ref2 = std::move(ref1);
  EXPECT_EQ(1, object1.ref_count_for_testing());
  EXPECT_EQ(0, object2.ref_count_for_testing());
  EXPECT_FALSE(ref1);
  EXPECT_TRUE(ref2);
  ref2.reset();
  EXPECT_FALSE(ref2);
  EXPECT_EQ(0, object1.ref_count_for_testing());
  EXPECT_EQ(0, object2.ref_count_for_testing());
}

TEST_F(RefCountedFragmentTest, Adopt) {
  TestObject object;

  FragmentRef<TestObject> ref1(RefCountedFragment::kUnmanagedRef,
                               Fragment(FragmentDescriptor(0, 0, 0), &object));
  EXPECT_EQ(1, object.ref_count_for_testing());

  FragmentRef<TestObject> ref2(RefCountedFragment::kAdoptExistingRef, nullptr,
                               Fragment(FragmentDescriptor(0, 0, 0), &object));
  EXPECT_EQ(1, object.ref_count_for_testing());
  std::ignore = ref1.release();
  ref2.reset();
  EXPECT_EQ(0, object.ref_count_for_testing());
}

}  // namespace
}  // namespace core
}  // namespace ipcz
