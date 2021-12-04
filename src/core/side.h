// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_SIDE_H_
#define IPCZ_SRC_CORE_SIDE_H_

#include <array>
#include <cstdint>
#include <string>

namespace ipcz {
namespace core {

// Typesafe enumeration for identifying two distinct sides of a portal pair or
// PortalLink. There's no special meaning or difference in behavior between the
// "left" or "right" side, they're merely monikers used to differentiate between
// one side and the other wherever we need to index any two-sided shared state.
enum class Side : uint8_t {
  kLeft = 0,
  kRight = 1,
};

inline Side Opposite(Side side) {
  return side == Side::kLeft ? Side::kRight : Side::kLeft;
}

inline uint8_t SideIndex(Side side) {
  return static_cast<uint8_t>(side);
}

inline std::string DescribeSide(Side side) {
  return side == Side::kLeft ? "left" : "right";
}

// Helper for a fixed array type that can be indexed by a Side. Useful in common
// shared state structures.
template <typename T>
struct TwoSided : public std::array<T, 2> {
  TwoSided() = default;
  TwoSided(T&& left, T&& right)
      : std::array<T, 2>({std::move(left), std::move(right)}) {}

  T& operator[](Side side) {
    return static_cast<std::array<T, 2>&>(*this)[SideIndex(side)];
  }

  const T& operator[](Side side) const {
    return static_cast<const std::array<T, 2>&>(*this)[SideIndex(side)];
  }

  T& left() { return (*this)[Side::kLeft]; }
  const T& left() const { return (*this)[Side::kLeft]; }

  T& right() { return (*this)[Side::kRight]; }
  const T& right() const { return (*this)[Side::kRight]; }
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_SIDE_H_
