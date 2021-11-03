// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_NODE_NAME_H_
#define IPCZ_SRC_CORE_NODE_NAME_H_

#include <cstdint>
#include <string>
#include <utility>

#include "third_party/abseil-cpp/absl/numeric/int128.h"

namespace ipcz {
namespace core {

class NodeName {
 public:
  enum { kRandom };

  constexpr NodeName() = default;
  explicit NodeName(decltype(kRandom));
  constexpr NodeName(absl::uint128 value) : value_(value) {}
  ~NodeName();

  bool is_valid() const { return value_ != 0; }

  uint64_t high() const { return absl::Uint128High64(value_); }
  uint64_t low() const { return absl::Uint128Low64(value_); }

  bool operator==(const NodeName& rhs) const { return value_ == rhs.value_; }
  bool operator!=(const NodeName& rhs) const { return value_ != rhs.value_; }
  bool operator<(const NodeName& rhs) const { return value_ < rhs.value_; }

  // Support for absl::Hash.
  template <typename H>
  friend H AbslHashValue(H h, const NodeName& name) {
    return H::combine(std::move(h), name.value_);
  }

  std::string ToString() const;

 public:
  absl::uint128 value_ = 0;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NODE_NAME_H_
