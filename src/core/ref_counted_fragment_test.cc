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
  EXPECT_TRUE(ref.is_null());
  EXPECT_FALSE(ref.is_addressable());

  ref.reset();
  EXPECT_TRUE(ref.is_null());
  EXPECT_FALSE(ref.is_addressable());

  FragmentRef<TestObject> other1 = ref;
  EXPECT_TRUE(ref.is_null());
  EXPECT_FALSE(ref.is_addressable());
  EXPECT_TRUE(other1.is_null());
  EXPECT_FALSE(other1.is_addressable());

  FragmentRef<TestObject> other2 = std::move(ref);
  EXPECT_TRUE(ref.is_null());
  EXPECT_FALSE(ref.is_addressable());
  EXPECT_TRUE(other2.is_null());
  EXPECT_FALSE(other2.is_addressable());

  ref = other1;
  EXPECT_TRUE(ref.is_null());
  EXPECT_FALSE(ref.is_addressable());
  EXPECT_TRUE(other1.is_null());
  EXPECT_FALSE(other1.is_addressable());

  ref = std::move(other2);
  EXPECT_TRUE(ref.is_null());
  EXPECT_FALSE(ref.is_addressable());
  EXPECT_TRUE(other1.is_null());
  EXPECT_FALSE(other1.is_addressable());
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
  EXPECT_TRUE(ref1.is_null());
  EXPECT_FALSE(ref1.is_addressable());
  other1.reset();
  EXPECT_EQ(0, object1.ref_count_for_testing());
  EXPECT_TRUE(other1.is_null());
  EXPECT_FALSE(other1.is_addressable());

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
  EXPECT_FALSE(ref1.is_null());
  EXPECT_TRUE(ref1.is_addressable());
  EXPECT_FALSE(ref2.is_null());
  EXPECT_TRUE(ref2.is_addressable());
  ref1.reset();
  EXPECT_EQ(1, object1.ref_count_for_testing());
  EXPECT_EQ(0, object2.ref_count_for_testing());
  EXPECT_TRUE(ref1.is_null());
  EXPECT_FALSE(ref1.is_addressable());
  ref2.reset();
  EXPECT_EQ(0, object1.ref_count_for_testing());
  EXPECT_EQ(0, object2.ref_count_for_testing());
  EXPECT_TRUE(ref2.is_null());
  EXPECT_FALSE(ref2.is_addressable());
}

TEST_F(RefCountedFragmentTest, Move) {
  TestObject object1;
  TestObject object2;

  FragmentRef<TestObject> ref1(RefCountedFragment::kUnmanagedRef,
                               Fragment(FragmentDescriptor(0, 0, 0), &object1));
  EXPECT_EQ(1, ref1.ref_count_for_testing());

  FragmentRef<TestObject> other1 = std::move(ref1);
  EXPECT_EQ(1, object1.ref_count_for_testing());
  EXPECT_FALSE(other1.is_null());
  EXPECT_TRUE(other1.is_addressable());
  EXPECT_TRUE(ref1.is_null());
  EXPECT_FALSE(ref1.is_addressable());
  other1.reset();
  EXPECT_TRUE(other1.is_null());
  EXPECT_FALSE(other1.is_addressable());
  EXPECT_EQ(0, object1.ref_count_for_testing());

  ref1 =
      FragmentRef<TestObject>(RefCountedFragment::kUnmanagedRef,
                              Fragment(FragmentDescriptor(0, 0, 0), &object1));
  FragmentRef<TestObject> ref2(RefCountedFragment::kUnmanagedRef,
                               Fragment(FragmentDescriptor(0, 0, 0), &object2));

  EXPECT_FALSE(ref1.is_null());
  EXPECT_TRUE(ref1.is_addressable());
  EXPECT_FALSE(ref2.is_null());
  EXPECT_TRUE(ref2.is_addressable());
  EXPECT_EQ(1, object1.ref_count_for_testing());
  EXPECT_EQ(1, object2.ref_count_for_testing());
  ref2 = std::move(ref1);
  EXPECT_EQ(1, object1.ref_count_for_testing());
  EXPECT_EQ(0, object2.ref_count_for_testing());
  EXPECT_TRUE(ref1.is_null());
  EXPECT_FALSE(ref1.is_addressable());
  EXPECT_FALSE(ref2.is_null());
  EXPECT_TRUE(ref2.is_addressable());
  ref2.reset();
  EXPECT_TRUE(ref2.is_null());
  EXPECT_FALSE(ref2.is_addressable());
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
