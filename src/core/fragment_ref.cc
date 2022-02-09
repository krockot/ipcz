// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/fragment_ref.h"

#include <algorithm>
#include <utility>

#include "core/node_link_memory.h"
#include "core/ref_counted_fragment.h"
#include "third_party/abseil-cpp/absl/base/macros.h"

#include "debug/log.h"
#include "debug/stack_trace.h"

namespace ipcz {
namespace core {
namespace internal {

GenericFragmentRef::GenericFragmentRef() = default;

GenericFragmentRef::GenericFragmentRef(mem::Ref<NodeLinkMemory> memory,
                                       const Fragment& fragment)
    : memory_(std::move(memory)), fragment_(fragment) {
  if (!fragment_.is_null()) {
    fragment_.As<RefCountedFragment>()->AddRef();
  }
}

GenericFragmentRef::GenericFragmentRef(
    decltype(RefCountedFragment::kAdoptExistingRef),
    mem::Ref<NodeLinkMemory> memory,
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
  if (!fragment_.is_null()) {
    fragment_.As<RefCountedFragment>()->AddRef();
  }
}

GenericFragmentRef& GenericFragmentRef::operator=(
    const GenericFragmentRef& other) {
  reset();
  memory_ = other.memory_;
  fragment_ = other.fragment_;
  if (!fragment_.is_null()) {
    fragment_.As<RefCountedFragment>()->AddRef();
  }
  return *this;
}

GenericFragmentRef::~GenericFragmentRef() {
  reset();
}

void GenericFragmentRef::reset() {
  mem::Ref<NodeLinkMemory> memory = std::move(memory_);
  if (fragment_.is_null()) {
    return;
  }

  Fragment fragment;
  std::swap(fragment, fragment_);

  if (fragment.As<RefCountedFragment>()->ReleaseRef() > 1 || !memory) {
    return;
  }

  memory->FreeFragment(fragment);
}

Fragment GenericFragmentRef::release() {
  Fragment fragment;
  std::swap(fragment_, fragment);
  memory_.reset();
  return fragment;
}

}  // namespace internal
}  // namespace core
}  // namespace ipcz
