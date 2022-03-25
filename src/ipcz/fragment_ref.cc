// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/fragment_ref.h"

#include <algorithm>
#include <utility>

#include "ipcz/node_link_memory.h"
#include "ipcz/ref_counted_fragment.h"
#include "third_party/abseil-cpp/absl/base/macros.h"

#include "util/log.h"
#include "util/stack_trace.h"

namespace ipcz::internal {

GenericFragmentRef::GenericFragmentRef() = default;

GenericFragmentRef::GenericFragmentRef(Ref<NodeLinkMemory> memory,
                                       const Fragment& fragment)
    : memory_(std::move(memory)), fragment_(fragment) {
  if (fragment_.is_addressable()) {
    fragment_.As<RefCountedFragment>()->AddRef();
  }
}

GenericFragmentRef::GenericFragmentRef(
    decltype(RefCountedFragment::kAdoptExistingRef),
    Ref<NodeLinkMemory> memory,
    const Fragment& fragment)
    : memory_(std::move(memory)), fragment_(fragment) {}

GenericFragmentRef::GenericFragmentRef(GenericFragmentRef&& other)
    : memory_(std::move(other.memory_)), fragment_(other.fragment_) {
  other.fragment_ = Fragment();
}

GenericFragmentRef& GenericFragmentRef::operator=(GenericFragmentRef&& other) {
  reset();
  memory_ = std::move(other.memory_);
  std::swap(fragment_, other.fragment_);
  return *this;
}

GenericFragmentRef::GenericFragmentRef(const GenericFragmentRef& other)
    : memory_(other.memory_), fragment_(other.fragment_) {
  if (fragment_.is_addressable()) {
    fragment_.As<RefCountedFragment>()->AddRef();
  }
}

GenericFragmentRef& GenericFragmentRef::operator=(
    const GenericFragmentRef& other) {
  reset();
  memory_ = other.memory_;
  fragment_ = other.fragment_;
  if (fragment_.is_addressable()) {
    fragment_.As<RefCountedFragment>()->AddRef();
  }
  return *this;
}

GenericFragmentRef::~GenericFragmentRef() {
  reset();
}

void GenericFragmentRef::reset() {
  Ref<NodeLinkMemory> memory = std::move(memory_);
  if (fragment_.is_null()) {
    return;
  }

  Fragment fragment;
  std::swap(fragment, fragment_);
  if (!fragment.is_addressable()) {
    return;
  }

  if (fragment.As<RefCountedFragment>()->ReleaseRef() > 1 || !memory) {
    return;
  }

  memory->fragment_allocator().Free(fragment);
}

Fragment GenericFragmentRef::release() {
  Fragment fragment;
  std::swap(fragment_, fragment);
  memory_.reset();
  return fragment;
}

}  // namespace ipcz::internal
