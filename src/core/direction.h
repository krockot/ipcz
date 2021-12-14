// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_DIRECTION_H_
#define IPCZ_SRC_CORE_DIRECTION_H_

namespace ipcz {
namespace core {

// A direction of travel along a route, relative to a frame of reference such as
// a router along the route. Moving inward from a router means moving further
// toward the terminal endpoint on its side of the route. Moving outward means
// moving instead toward the terminal endpoint of the opposite side.
enum class Direction {
  kInward,
  kOutward,
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_DIRECTION_H_
