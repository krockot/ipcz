// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_SIDE_H_
#define IPCZ_SRC_CORE_SIDE_H_

#include <array>
#include <cstdint>

namespace ipcz {
namespace core {

// Typesafe enumeration for identifying two distinct sides of a portal pair or
// PortalLink. There's no special meaning or difference in behavior between the
// "left" or "right" side, they're merely monikers used to differentiate between
// one side and the other wherever we need to index any two-sided shared state.
// kPredecessor and kSuccessor are aliases for kLeft and kRight, as successor
// and predecessor links are always two sides of the same route and always exist
// on the same side (left or right) of a portal pair.
enum class Side : uint8_t {
  kLeft = 0,
  kPredecessor = 0,
  kRight = 1,
  kSuccessor = 1,
};

inline Side Opposite(Side side) {
  return side == Side::kLeft ? Side::kRight : Side::kLeft;
}

inline uint8_t SideIndex(Side side) {
  return static_cast<uint8_t>(side);
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
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_SIDE_H_
