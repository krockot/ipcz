// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util/two_mutex_lock.h"

namespace ipcz {

TwoMutexLock::TwoMutexLock(absl::Mutex& a, absl::Mutex& b) : a_(a), b_(b) {
  if (&a_ < &b_) {
    a_.Lock();
    b_.Lock();
  } else {
    b_.Lock();
    a_.Lock();
  }
}

TwoMutexLock::~TwoMutexLock() {
  a_.Unlock();
  b_.Unlock();
}

}  // namespace ipcz
