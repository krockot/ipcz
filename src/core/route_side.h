// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_ROUTE_SIDE_H_
#define IPCZ_SRC_CORE_ROUTE_SIDE_H_

#include <cstdint>
#include <string>

namespace ipcz {
namespace core {

// A RouteSide conveys the identity of one side of a route, relative to some
// frame of reference. Unlike individual links along a route, routes themselves
// do not have an objective side A or side B; but the route is still divided
// into two halves conceptually, and each Router along the route implicitly
// belongs to one half or the other.
//
// At any Router along the route, a RouteSide conveys a relative reference to
// either that Router's own half (kSame), or the other half (kOther).
struct RouteSide {
  enum class Value : uint8_t {
    kSame = 0,
    kOther = 1,
  };

  static constexpr Value kSame = Value::kSame;
  static constexpr Value kOther = Value::kOther;

  constexpr RouteSide() = default;
  constexpr RouteSide(Value value) : value_(value) {}

  bool operator==(const RouteSide& rhs) const { return value_ == rhs.value_; }
  bool operator!=(const RouteSide& rhs) const { return value_ != rhs.value_; }

  bool is_same_side() const { return value_ == Value::kSame; }
  bool is_other_side() const { return value_ == Value::kOther; }

  Value value() const { return value_; }
  RouteSide opposite() const {
    return is_same_side() ? Value::kOther : Value::kSame;
  }

  explicit operator Value() const { return value_; }

  std::string ToString() const {
    return value_ == Value::kSame ? "same" : "other";
  }

 private:
  Value value_ = Value::kSame;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_ROUTE_SIDE_H_
