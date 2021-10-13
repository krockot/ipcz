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

// Jacked from Chromium base::HashInts64.
// Implement hashing for pairs of up-to 64-bit integer values.
// We use the compound integer hash method to produce a 64-bit hash code, by
// breaking the two 64-bit inputs into 4 32-bit values:
// http://opendatastructures.org/versions/edition-0.1d/ods-java/node33.html#SECTION00832000000000000000
// Then we reduce our result to 32 bits if required, similar to above.
size_t HashInts64(uint64_t value1, uint64_t value2) {
  uint32_t short_random1 = 842304669U;
  uint32_t short_random2 = 619063811U;
  uint32_t short_random3 = 937041849U;
  uint32_t short_random4 = 3309708029U;

  uint32_t value1a = static_cast<uint32_t>(value1 & 0xffffffff);
  uint32_t value1b = static_cast<uint32_t>((value1 >> 32) & 0xffffffff);
  uint32_t value2a = static_cast<uint32_t>(value2 & 0xffffffff);
  uint32_t value2b = static_cast<uint32_t>((value2 >> 32) & 0xffffffff);

  uint64_t product1 = static_cast<uint64_t>(value1a) * short_random1;
  uint64_t product2 = static_cast<uint64_t>(value1b) * short_random2;
  uint64_t product3 = static_cast<uint64_t>(value2a) * short_random3;
  uint64_t product4 = static_cast<uint64_t>(value2b) * short_random4;

  uint64_t hash64 = product1 + product2 + product3 + product4;

  if (sizeof(size_t) >= sizeof(uint64_t)) {
    return static_cast<size_t>(hash64);
  }

  uint64_t odd_random = 1578233944LL << 32 | 194370989LL;
  uint32_t shift_random = 20591U << 16;

  hash64 = hash64 * odd_random + shift_random;
  size_t high_bits =
      static_cast<size_t>(hash64 >> (8 * (sizeof(uint64_t) - sizeof(size_t))));
  return high_bits;
}

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

size_t Name::Hash() const {
  return HashInts64(words_[0], words_[1]);
}

}  // namespace core
}  // namespace ipcz
