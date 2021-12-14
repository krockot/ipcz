// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_DIRECTION_H_
#define IPCZ_SRC_CORE_DIRECTION_H_

#include <string>

namespace ipcz {
namespace core {

// A direction of travel along a route, relative to a frame of reference such as
// a router along the route. Moving inward from a router means moving further
// toward the terminal endpoint on its side of the route. Moving outward means
// moving instead toward the terminal endpoint of the opposite side.
struct Direction {
  enum class Value {
    kInward,
    kOutward,
  };

  static constexpr Value kInward = Value::kInward;
  static constexpr Value kOutward = Value::kOutward;

  constexpr Direction() = default;
  constexpr Direction(Value value) : value_(value) {}

  bool operator==(const Direction& rhs) const { return value_ == rhs.value_; }
  bool operator!=(const Direction& rhs) const { return value_ != rhs.value_; }

  bool is_inward() const { return value_ == Value::kInward; }
  bool is_outward() const { return value_ == Value::kOutward; }

  Value value() const { return value_; }
  Direction opposite() const {
    return is_inward() ? Value::kOutward : Value::kInward;
  }

  explicit operator Value() const { return value_; }

  std::string ToString() const;

 private:
  Value value_ = Value::kInward;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_LINK_SIDE_H_
