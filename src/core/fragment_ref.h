// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_FRAGMENT_REF_H_
#define IPCZ_SRC_CORE_FRAGMENT_REF_H_

#include <algorithm>
#include <type_traits>
#include <utility>

#include "core/fragment.h"
#include "core/fragment_descriptor.h"
#include "core/ref_counted_fragment.h"
#include "mem/ref_counted.h"

namespace ipcz {
namespace core {

class NodeLinkMemory;

namespace internal {

// Base class for any FragmentRef<T>, implementing common behavior for managing
// the underlying RefCountedFragment.
class GenericFragmentRef {
 public:
  GenericFragmentRef();

  // Does not increase the ref count, effectively assuming ownership of a
  // previously acquired ref.
  GenericFragmentRef(mem::Ref<NodeLinkMemory> memory, const Fragment& fragment);
  GenericFragmentRef(decltype(RefCountedFragment::kAdoptExistingRef),
                     mem::Ref<NodeLinkMemory> memory,
                     const Fragment& fragment);
  GenericFragmentRef(GenericFragmentRef&& other);
  GenericFragmentRef& operator=(GenericFragmentRef&& other);
  GenericFragmentRef(const GenericFragmentRef& other);
  GenericFragmentRef& operator=(const GenericFragmentRef& other);
  ~GenericFragmentRef();

  const mem::Ref<NodeLinkMemory>& memory() const { return memory_; }
  const Fragment& fragment() const { return fragment_; }

  explicit operator bool() const { return !fragment_.is_null(); }

  void reset();
  Fragment release();

  int32_t ref_count_for_testing() const {
    return fragment_.As<RefCountedFragment>()->ref_count_for_testing();
  }

 protected:
  mem::Ref<NodeLinkMemory> memory_;
  Fragment fragment_;
};

}  // namespace internal

// Holds a reference to a RefCountedFragment. When this object is constructed,
// the RefCountedFragment's ref count is increased. When destroyed, the ref
// count is decreased. If the ref count is decreased to zero, the underlying
// Fragment is returned to its NodeLink's memory pool.
template <typename T>
class FragmentRef : public internal::GenericFragmentRef {
 public:
  static_assert(std::is_base_of<RefCountedFragment, T>::value,
                "T must inherit RefCountedFragment for FragmentRef<T>");

  constexpr FragmentRef() = default;
  constexpr FragmentRef(std::nullptr_t) : FragmentRef() {}
  FragmentRef(mem::Ref<NodeLinkMemory> memory, const Fragment& fragment)
      : GenericFragmentRef(std::move(memory), fragment) {}
  FragmentRef(decltype(RefCountedFragment::kAdoptExistingRef),
              mem::Ref<NodeLinkMemory> memory,
              const Fragment& fragment)
      : GenericFragmentRef(RefCountedFragment::kAdoptExistingRef,
                           std::move(memory),
                           fragment) {}
  FragmentRef(decltype(RefCountedFragment::kUnmanagedRef),
              const Fragment& fragment)
      : GenericFragmentRef(nullptr, fragment) {}

  FragmentRef(const FragmentRef<T>& other)
      : FragmentRef(other.memory(), other.fragment()) {}
  FragmentRef(FragmentRef<T>&& other) noexcept
      : FragmentRef(RefCountedFragment::kAdoptExistingRef,
                    other.memory(),
                    other.fragment()) {
    other.release();
  }

  FragmentRef<T>& operator=(const FragmentRef<T>& other) {
    reset();
    memory_ = other.memory();
    fragment_ = other.fragment();
    if (!fragment_.is_null()) {
      fragment_.As<RefCountedFragment>()->AddRef();
    }
    return *this;
  }

  FragmentRef<T>& operator=(FragmentRef<T>&& other) {
    reset();
    memory_ = std::move(other.memory_);
    std::swap(fragment_, other.fragment_);
    return *this;
  }

  T* get() const { return static_cast<T*>(fragment_.As<T>()); }
  T* operator->() const { return get(); }
  T& operator*() const { return *get(); }
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_FRAGMENT_REF_H_
