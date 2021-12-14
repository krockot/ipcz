// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_MEM_REF_COUNTED_H_
#define IPCZ_SRC_MEM_REF_COUNTED_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace ipcz {
namespace mem {

class RefCounted {
 public:
  enum { kAdoptExistingRef };

  RefCounted();
  virtual ~RefCounted();

  void AcquireRef();
  void ReleaseRef();

 private:
  std::atomic<uint64_t> ref_count_{0};
};

class GenericRef {
 public:
  constexpr GenericRef() = default;

  // Does not increase the ref count, effectively assuming ownership of a
  // previously acquired ref.
  GenericRef(decltype(RefCounted::kAdoptExistingRef), RefCounted* ptr);
  GenericRef(RefCounted* ptr);
  GenericRef(GenericRef&& other);
  GenericRef& operator=(GenericRef&& other);
  GenericRef(const GenericRef& other);
  GenericRef& operator=(const GenericRef& other);
  ~GenericRef();

  explicit operator bool() const { return ptr_ != nullptr; }

  void reset();

 protected:
  void* ReleaseImpl();

  RefCounted* ptr_ = nullptr;
};

template <typename T>
class Ref : public GenericRef {
 public:
  constexpr Ref() = default;
  constexpr Ref(std::nullptr_t) {}
  Ref(T* ptr) : GenericRef(ptr) {}
  Ref(decltype(RefCounted::kAdoptExistingRef), T* ptr)
      : GenericRef(RefCounted::kAdoptExistingRef, ptr) {}

  template <typename U>
  using EnableIfConvertible =
      typename std::enable_if<std::is_convertible<U*, T*>::value>::type;

  template <typename U, typename = EnableIfConvertible<U>>
  Ref(const Ref<U>& other) : Ref(other.get()) {}

  template <typename U, typename = EnableIfConvertible<U>>
  Ref(Ref<U>&& other) noexcept
      : Ref(RefCounted::kAdoptExistingRef, other.release()) {}

  T* get() const { return static_cast<T*>(ptr_); }
  T* operator->() const { return get(); }
  T& operator*() const { return *get(); }

  bool operator==(const Ref<T>& other) const { return ptr_ == other.ptr_; }
  bool operator!=(const Ref<T>& other) const { return ptr_ != other.ptr_; }

  T* release() { return static_cast<T*>(ReleaseImpl()); }

  template <typename H>
  friend H AbslHashValue(H h, const Ref<T>& ref) {
    return H::combine(std::move(h), ref.get());
  }
};

template <typename T, typename... Args>
Ref<T> MakeRefCounted(Args&&... args) {
  return Ref<T>(new T(std::forward<Args>(args)...));
}

template <typename T>
Ref<T> WrapRefCounted(T* ptr) {
  return Ref<T>(ptr);
}

}  // namespace mem
}  // namespace ipcz

#endif  // IPCZ_SRC_MEM_REF_COUNTED_H_
