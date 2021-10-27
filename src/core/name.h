// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_NAME_H_
#define IPCZ_SRC_CORE_NAME_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>

#include "third_party/abseil-cpp/absl/numeric/int128.h"

namespace ipcz {
namespace core {

class Name {
 public:
  enum { kRandom };

  constexpr Name() = default;
  explicit Name(decltype(kRandom));
  constexpr Name(absl::uint128 value) : value_(value) {}
  ~Name();

  bool is_valid() const { return value_ != 0; }

  uint64_t high() const { return absl::Uint128High64(value_); }
  uint64_t low() const { return absl::Uint128Low64(value_); }

  bool operator==(const Name& rhs) const { return value_ == rhs.value_; }
  bool operator!=(const Name& rhs) const { return value_ != rhs.value_; }
  bool operator<(const Name& rhs) const { return value_ < rhs.value_; }

  // Support for absl::Hash.
  template <typename H>
  friend H AbslHashValue(H h, const Name& name) {
    return H::combine(std::move(h), name.value_);
  }

  std::string ToString() const;

 public:
  absl::uint128 value_ = 0;
};

class PortalName : public Name {
 public:
  constexpr PortalName() = default;
  PortalName(decltype(kRandom)) : Name(kRandom) {}
  explicit PortalName(absl::uint128 value) : Name(value) {}
};

class NodeName : public Name {
 public:
  constexpr NodeName() = default;
  NodeName(decltype(kRandom)) : Name(kRandom) {}
  explicit NodeName(absl::uint128 value) : Name(value) {}
};

class PortalAddress {
 public:
  constexpr PortalAddress() = default;
  PortalAddress(NodeName node, PortalName portal)
      : node_(node), portal_(portal) {}
  ~PortalAddress() = default;

  bool is_valid() const { return node_.is_valid() && portal_.is_valid(); }

  NodeName node() const { return node_; }
  PortalName portal() const { return portal_; }

  bool operator==(const PortalAddress& rhs) const {
    return std::tie(node_, portal_) == std::tie(rhs.node_, rhs.portal_);
  }

  bool operator!=(const PortalAddress& rhs) const {
    return std::tie(node_, portal_) != std::tie(rhs.node_, rhs.portal_);
  }
  bool operator<(const PortalAddress& rhs) const {
    return std::tie(node_, portal_) < std::tie(rhs.node_, rhs.portal_);
  }

  // Support for absl::Hash.
  template <typename H>
  friend H AbslHashValue(H h, const PortalAddress& address) {
    return H::combine(std::move(h), address.node_, address.portal_);
  }

  std::string ToString() const;

 private:
  NodeName node_;
  PortalName portal_;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_NAME_H_
