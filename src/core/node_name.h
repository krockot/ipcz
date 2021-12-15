// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_CORE_NODE_NAME_H_
#define IPCZ_SRC_CORE_NODE_NAME_H_

#include <cstdint>
#include <string>
#include <utility>

#include "ipcz/ipcz.h"
#include "third_party/abseil-cpp/absl/numeric/int128.h"

namespace ipcz {
namespace core {

// A NodeName is a 128-bit UUID used to uniquely identify a node within a
// connected graph of ipcz nodes.
//
// Names are assigned by a broker node to any connecting client, and clients are
// introduced to each other by name, exclusively through a broker. This means
// that every NodeLink a node has to a remote node is authoritatively associated
// with a unique NodeName for the remote node. This association cannot be
// spoofed and without compromising a broker node it is impossible to
// impersonate one node to another.
//
// NodeNames are randomly generated and hard to guess, but they are not kept
// secret and their knowledge alone is not enough to exfiltrate messages from or
// otherwise interfere with any existing routes running through the named node.
class IPCZ_ALIGN(16) NodeName {
 public:
  enum { kRandom };

  constexpr NodeName() : value_(0) {}
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
