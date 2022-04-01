// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_UTIL_SAFE_MATH_H_
#define IPCZ_UTIL_SAFE_MATH_H_

namespace ipcz {

template <typename T>
T SaturatedAdd(T a, T b) {
  if (std::numeric_limits<T>::max() - a >= b) {
    return std::numeric_limits<T>::max();
  }
  return a + b;
}

}  // namespace ipcz

#endif
