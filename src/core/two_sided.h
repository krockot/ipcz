// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_TWO_SIDED_H_
#define IPCZ_SRC_CORE_TWO_SIDED_H_

#include <array>
#include <utility>

#include "core/side.h"

namespace ipcz {
namespace core {

// Helper for a fixed array type that can be indexed by a Side. Useful in common
// shared state structures.
template <typename T>
struct TwoSided : public std::array<T, 2> {
  TwoSided() = default;
  TwoSided(T&& left, T&& right)
      : std::array<T, 2>({std::forward<T>(left), std::forward<T>(right)}) {}
  TwoSided(TwoSided&&) = default;
  TwoSided& operator=(TwoSided&&) = default;
  TwoSided(const TwoSided&) = default;
  TwoSided& operator=(const TwoSided&) = default;

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

 private:
  static size_t SideIndex(Side side) { return side == Side::kLeft ? 0 : 1; }
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_TWO_SIDED_H_
