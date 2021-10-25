// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_NAME_H_
#define IPCZ_SRC_CORE_NAME_H_

#include <cstddef>
#include <cstdint>
#include <tuple>

namespace ipcz {
namespace core {

class Name {
 public:
  enum { kRandom };

  constexpr Name() : words_{0, 0} {}
  explicit Name(decltype(kRandom));
  constexpr Name(uint64_t high, uint64_t low) : words_{high, low} {}
  ~Name();

  bool is_valid() const { return high() != 0 && low() != 0; }

  uint64_t high() const { return words_[0]; }
  uint64_t low() const { return words_[1]; }

  bool operator==(const Name& rhs) const {
    return std::tie(words_[0], words_[1]) ==
           std::tie(rhs.words_[0], rhs.words_[1]);
  }

  bool operator!=(const Name& rhs) const { return !(*this == rhs); }

  bool operator<(const Name& rhs) const {
    return std::tie(words_[0], words_[1]) <
           std::tie(rhs.words_[0], rhs.words_[1]);
  }

  // Support for absl::Hash.
  template <typename H>
  friend H AbslHashValue(H h, const Name& name) {
    return H::combine(std::move(h), name.words_[0], name.words_[1]);
  }

 public:
  uint64_t words_[2];
};

class PortalName : public Name {
 public:
  constexpr PortalName() = default;
  PortalName(decltype(kRandom)) : Name(kRandom) {}
  PortalName(uint64_t high, uint64_t low) : Name(high, low) {}
};

class NodeName : public Name {
 public:
  constexpr NodeName() = default;
  NodeName(decltype(kRandom)) : Name(kRandom) {}
  NodeName(uint64_t high, uint64_t low) : Name(high, low) {}
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NAME_H_
