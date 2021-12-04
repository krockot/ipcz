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
  enum class Value : uint8_t {
    kLeft = 0,
    kRight = 1,
  };

  static constexpr Value kLeft = Value::kLeft;
  static constexpr Value kRight = Value::kRight;

  constexpr Side() = default;
  constexpr Side(Value value) : value_(value) {}

  bool operator==(const Side& rhs) const { return value_ == rhs.value_; }
  bool operator!=(const Side& rhs) const { return value_ != rhs.value_; }

  bool is_left() const { return value_ == Value::kLeft; }
  bool is_right() const { return value_ == Value::kRight; }

  Value value() const { return value_; }
  Side opposite() const { return is_left() ? Value::kRight : Value::kLeft; }

  explicit operator Value() const { return value_; }

  std::string ToString() const {
    return value_ == Value::kLeft ? "left" : "right";
  }

 private:
  Value value_ = Value::kLeft;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_SIDE_H_
