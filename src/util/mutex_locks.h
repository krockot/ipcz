// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_UTIL_MUTEX_LOCKS_H_
#define IPCZ_SRC_UTIL_MUTEX_LOCKS_H_

#include <array>

#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace ipcz {

namespace internal {

// Helper for the explicit lockers defined below. We use std::sort() on the
// assumption that most modern implementations optimize small cases very well,
// and that in practice we only care about 2- 3- and (rarely) 4-mutex cases.
//
// We don't use this helper directly outside of the definitions below, because
// there's no way for it to convey a variadic list of mutexes to clang's
// exclusive_lock_function() attribute.
template <size_t N>
class MultiMutexLock {
 public:
  explicit MultiMutexLock(const std::array<absl::Mutex*, N>& mutexes)
      : mutexes_(mutexes) {
    std::sort(mutexes_.begin(), mutexes_.end());
    for (absl::Mutex* mutex : mutexes_) {
      mutex->Lock();
    }
  }

  MultiMutexLock(const MultiMutexLock&) = delete;
  MultiMutexLock& operator=(const MultiMutexLock&) = delete;

  ~MultiMutexLock() {
    for (absl::Mutex* mutex : mutexes_) {
      mutex->Unlock();
    }
  }

 private:
  std::array<absl::Mutex*, N> mutexes_;
};

}  // namespace internal

// Holds two overlapping mutexes acquired in a globally consistent order. For
// convenience, the second mutex may be null and will be ignored in that case.
class ABSL_SCOPED_LOCKABLE TwoMutexLock {
 public:
  TwoMutexLock(absl::Mutex* a, absl::Mutex* b)
      ABSL_EXCLUSIVE_LOCK_FUNCTION(*a, *b);
  ~TwoMutexLock() ABSL_UNLOCK_FUNCTION();

 private:
  absl::Mutex* const a_;
  absl::optional<internal::MultiMutexLock<2>> locks_;
};

// Holds three overlapping mutexes acquired in a globally consistent order.
class ABSL_SCOPED_LOCKABLE ThreeMutexLock {
 public:
  ThreeMutexLock(absl::Mutex* a, absl::Mutex* b, absl::Mutex* c)
      ABSL_EXCLUSIVE_LOCK_FUNCTION(*a, *b, *c);
  ~ThreeMutexLock() ABSL_UNLOCK_FUNCTION();

 private:
  internal::MultiMutexLock<3> locks_;
};

// Holds FOUR overlapping mutexes acquired in a globally consistent order.
class ABSL_SCOPED_LOCKABLE FourMutexLock {
 public:
  FourMutexLock(absl::Mutex* a, absl::Mutex* b, absl::Mutex* c, absl::Mutex* d)
      ABSL_EXCLUSIVE_LOCK_FUNCTION(*a, *b, *c, *d);
  ~FourMutexLock() ABSL_UNLOCK_FUNCTION();

 private:
  internal::MultiMutexLock<4> locks_;
};

}  // namespace ipcz

#endif  // IPCZ_SRC_UTIL_MUTEX_LOCKS_H_
