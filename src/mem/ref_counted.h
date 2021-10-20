// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_MEM_REF_COUNTED_H_
#define IPCZ_SRC_MEM_REF_COUNTED_H_

#include <atomic>
#include <cstdint>
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
  GenericRef();

  // Does not increase the ref count, effectively assuming ownership of a
  // previously acquired ref.
  GenericRef(decltype(RefCounted::kAdoptExistingRef), RefCounted* ptr);

  explicit GenericRef(RefCounted* ptr);
  GenericRef(GenericRef&& other);
  GenericRef& operator=(GenericRef&& other);
  GenericRef(const GenericRef& other);
  GenericRef& operator=(const GenericRef& other);
  ~GenericRef();

  explicit operator bool() const { return ptr_ != nullptr; }
  bool operator==(const GenericRef& other) const { return ptr_ == other.ptr_; }
  bool operator!=(const GenericRef& other) const { return ptr_ != other.ptr_; }

  void reset();

 protected:
  void* ReleaseImpl();

  RefCounted* ptr_ = nullptr;
};

template <typename T>
class Ref : public GenericRef {
 public:
  Ref() = default;
  explicit Ref(T* ptr) : GenericRef(ptr) {}
  Ref(decltype(RefCounted::kAdoptExistingRef), T* ptr)
      : GenericRef(RefCounted::kAdoptExistingRef, ptr) {}

  T* get() const { return static_cast<T*>(ptr_); }
  T* operator->() const { return get(); }
  T& operator*() const { return *get(); }
  operator T*() const { return get(); }

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
