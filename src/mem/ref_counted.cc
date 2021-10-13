// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mem/ref_counted.h"

#include "third_party/abseil-cpp/absl/base/macros.h"

namespace ipcz {
namespace mem {

RefCounted::RefCounted() = default;

RefCounted::~RefCounted() = default;

void RefCounted::AcquireRef() {
  ref_count_.fetch_add(1, std::memory_order_relaxed);
}

void RefCounted::ReleaseRef() {
  uint64_t last_count = ref_count_.fetch_sub(1, std::memory_order_release);
  ABSL_ASSERT(last_count > 0);
  if (last_count == 1) {
    std::atomic_thread_fence(std::memory_order_acquire);
    delete this;
  }
}

GenericRef::GenericRef() = default;

GenericRef::GenericRef(RefCounted* ptr) : ptr_(ptr) {
  if (ptr_) {
    ptr_->AcquireRef();
  }
}

GenericRef::GenericRef(GenericRef&& other) {
  ptr_ = other.ptr_;
  other.ptr_ = nullptr;
}

GenericRef& GenericRef::operator=(GenericRef&& other) {
  reset();
  ptr_ = other.ptr_;
  other.ptr_ = nullptr;
  return *this;
}

GenericRef::GenericRef(const GenericRef& other) : ptr_(other.ptr_) {
  if (ptr_) {
    ptr_->AcquireRef();
  }
}

GenericRef& GenericRef::operator=(const GenericRef& other) {
  reset();
  ptr_ = other.ptr_;
  if (ptr_) {
    ptr_->AcquireRef();
  }
  return *this;
}

GenericRef::~GenericRef() {
  reset();
}

void GenericRef::reset() {
  if (ptr_) {
    ptr_->ReleaseRef();
    ptr_ = nullptr;
  }
}

}  // namespace mem
}  // namespace ipcz
