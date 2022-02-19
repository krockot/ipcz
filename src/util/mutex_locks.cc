// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util/mutex_locks.h"

namespace ipcz {

TwoMutexLock::TwoMutexLock(absl::Mutex* a, absl::Mutex* b) : a_(a) {
  if (b) {
    locks_.emplace(std::array<absl::Mutex*, 2>({a, b}));
  } else {
    a_->Lock();
  }
}

TwoMutexLock::~TwoMutexLock() {
  if (!locks_) {
    a_->Unlock();
  }
}

ThreeMutexLock::ThreeMutexLock(absl::Mutex* a, absl::Mutex* b, absl::Mutex* c)
    : locks_({a, b, c}) {}

ThreeMutexLock::~ThreeMutexLock() = default;

FourMutexLock::FourMutexLock(absl::Mutex* a,
                             absl::Mutex* b,
                             absl::Mutex* c,
                             absl::Mutex* d)
    : locks_({a, b, c, d}) {}

FourMutexLock::~FourMutexLock() = default;

}  // namespace ipcz
