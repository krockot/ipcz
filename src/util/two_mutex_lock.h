// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_UTIL_TWO_MUTEX_LOCK_H_
#define IPCZ_SRC_UTIL_TWO_MUTEX_LOCK_H_

#include "third_party/abseil-cpp/absl/synchronization/mutex.h"

namespace ipcz {

class ABSL_SCOPED_LOCKABLE TwoMutexLock {
 public:
  TwoMutexLock(absl::Mutex* a, absl::Mutex* b)
      ABSL_EXCLUSIVE_LOCK_FUNCTION(*a, *b);
  ~TwoMutexLock() ABSL_UNLOCK_FUNCTION();

 private:
  absl::Mutex* a_;
  absl::Mutex* b_;
};

}  // namespace ipcz

#endif  // IPCZ_SRC_UTIL_TWO_MUTEX_LOCK_H_
