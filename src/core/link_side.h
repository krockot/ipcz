// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_LINK_SIDE_H_
#define IPCZ_SRC_CORE_LINK_SIDE_H_

#include <cstdint>
#include <string>

namespace ipcz {
namespace core {

// A simple wrapper around an A/B enum to consistently distinguish between two
// sides of a single conceptual link between routers. Side A and side B are
// arbitrary choices, but it's important that each side of a link identify as
// one or the other respectively.
struct LinkSide {
  enum class Value : uint8_t {
    kA = 0,
    kB = 1,
  };

  static constexpr Value kA = Value::kA;
  static constexpr Value kB = Value::kB;

  constexpr LinkSide() = default;
  constexpr LinkSide(Value value) : value_(value) {}

  bool operator==(const LinkSide& rhs) const { return value_ == rhs.value_; }
  bool operator!=(const LinkSide& rhs) const { return value_ != rhs.value_; }

  bool is_side_a() const { return value_ == Value::kA; }
  bool is_side_b() const { return value_ == Value::kB; }

  Value value() const { return value_; }
  LinkSide opposite() const { return is_side_a() ? Value::kB : Value::kA; }

  explicit operator Value() const { return value_; }

  std::string ToString() const {
    return value_ == Value::kA ? "left" : "right";
  }

 private:
  Value value_ = Value::kA;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_LINK_SIDE_H_
