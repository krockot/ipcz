// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_SIDE_H_
#define IPCZ_SRC_CORE_SIDE_H_

#include <cstdint>
#include <string>

namespace ipcz {
namespace core {

// A simple wrapper around a left/right enum, with some convenience methods.
// Used to label routers along a route as left- or right-sided, and also
// generally used to index TwoSided object pairs.
struct Side {
  enum Value : uint8_t {
    kLeft = 0,
    kRight = 1,
  };

  constexpr Side() = default;
  constexpr Side(Value value) : value_(value) {}

  bool is_left() const { return value_ == kLeft; }
  bool is_right() const { return value_ == kRight; }

  Value value() const { return value_; }
  Side opposite() const { return is_left() ? kRight : kLeft; }

  operator Value() const { return value_; }

  std::string ToString() const { return value_ == kLeft ? "left" : "right"; }

 private:
  Value value_ = kLeft;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_SIDE_H_
