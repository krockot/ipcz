// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/name.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "build/build_config.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/types/span.h"

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

void FillRandom(absl::Span<uint64_t> words) {
  size_t num_bytes = words.size() * sizeof(uint64_t);
#if defined(OS_WIN)
  char* output = reinterpret_cast<char*>(words.data());
  const bool ok = RtlGenRandom(words.data(), num_bytes);
  ABSL_ASSERT(ok);
#elif defined(OS_FUCHSIA)
  zx_cprng_draw(words.data(), num_bytes);
#elif defined(OS_LINUX) || defined(OS_CHROMEOS)
  ssize_t bytes = 0;
  while (num_bytes > 0) {
    ssize_t result = getrandom(words.data() + bytes, num_bytes, 0);
    if (result == -1) {
      ABSL_ASSERT(errno == EINTR);
      continue;
    }

    bytes += result;
    num_bytes -= result;
  }
#elif defined(OS_MAC)
  const bool ok = getentropy(words.data(), num_bytes) == 0;
  ABSL_ASSERT(ok);
#else
#error "Unsupported platform"
#endif
}

}  // namespace

Name::Name(decltype(kRandom)) {
  FillRandom(words_);
}

Name::~Name() = default;

}  // namespace core
}  // namespace ipcz
