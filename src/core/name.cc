// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/name.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

#include "build/build_config.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/numeric/int128.h"

#if defined(OS_WIN)
#include <windows.h>
#elif defined(OS_FUCHSIA)
#include <zircon/syscalls.h>
#elif defined(OS_LINUX) || defined(OS_CHROMEOS)
#include <errno.h>
#include <sys/random.h>
#elif defined(OS_MAC)
#include <unistd.h>
#endif

#if defined(OS_WIN)
// #define needed to link in RtlGenRandom(), a.k.a. SystemFunction036.  See the
// "Community Additions" comment on MSDN here:
// http://msdn.microsoft.com/en-us/library/windows/desktop/aa387694.aspx
#define SystemFunction036 NTAPI SystemFunction036
#include <NTSecAPI.h>
#undef SystemFunction036
#endif

namespace ipcz {
namespace core {

namespace {

void RandomUint128(absl::uint128& value) {
#if defined(OS_WIN)
  char* output = reinterpret_cast<char*>(words.data());
  const bool ok = RtlGenRandom(&value, sizeof(value));
  ABSL_ASSERT(ok);
#elif defined(OS_FUCHSIA)
  zx_cprng_draw(&value, sizeof(value));
#elif defined(OS_LINUX) || defined(OS_CHROMEOS)
  ssize_t bytes = 0;
  size_t num_bytes = sizeof(value);
  while (num_bytes > 0) {
    ssize_t result =
        getrandom(reinterpret_cast<uint8_t*>(&value) + bytes, num_bytes, 0);
    if (result == -1) {
      ABSL_ASSERT(errno == EINTR);
      continue;
    }

    bytes += result;
    num_bytes -= result;
  }
#elif defined(OS_MAC)
  const bool ok = getentropy(&value, sizeof(value)) == 0;
  ABSL_ASSERT(ok);
#else
#error "Unsupported platform"
#endif
}

}  // namespace

Name::Name(decltype(kRandom)) {
  RandomUint128(value_);
}

Name::~Name() = default;

std::string Name::ToString() const {
  char chars[33];
  int length = snprintf(chars, 33, "%016lx%016lx", absl::Uint128High64(value_),
                        absl::Uint128Low64(value_));
  ABSL_ASSERT(length == 32);
  return std::string(chars, 32);
}

std::string PortalAddress::ToString() const {
  return node_.ToString() + ":" + portal_.ToString();
}

}  // namespace core
}  // namespace ipcz
