// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util/random.h"

#include <cstdint>
#include <cstddef>

#include "build/build_config.h"
#include "third_party/abseil-cpp/absl/base/macros.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#elif BUILDFLAG(IS_FUCHSIA)
#include <zircon/syscalls.h>
#elif BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
#include <errno.h>
#include <sys/random.h>
#elif BUILDFLAG(IS_MAC)
#include <unistd.h>
#elif BUILDFLAG(IS_NACL)
#include <nacl/nacl_random.h>
#endif

#if BUILDFLAG(IS_WIN)
// #define needed to link in RtlGenRandom(), a.k.a. SystemFunction036.  See the
// "Community Additions" comment on MSDN here:
// http://msdn.microsoft.com/en-us/library/windows/desktop/aa387694.aspx
#define SystemFunction036 NTAPI SystemFunction036
#include <NTSecAPI.h>
#undef SystemFunction036
#endif

namespace ipcz {

uint64_t RandomUint64() {
  uint64_t value;
#if BUILDFLAG(IS_WIN)
  const bool ok = RtlGenRandom(&value, sizeof(value));
  ABSL_ASSERT(ok);
#elif BUILDFLAG(IS_FUCHSIA)
  zx_cprng_draw(&value, sizeof(value));
#elif BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
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
#elif BUILDFLAG(IS_MAC)
  const bool ok = getentropy(&value, sizeof(value)) == 0;
  ABSL_ASSERT(ok);
#elif BUILDFLAG(IS_NACL)
  size_t bytes_needed = sizeof(value);
  uint8_t* storage = reinterpret_cast<uint8_t*>(&value);
  while (bytes_needed > 0) {
    size_t nread;
    nacl_secure_random(storage, bytes_needed, &nread);
    storage += nread;
    bytes_needed -= nread;
  }
#else
#error "Unsupported platform"
#endif
  return value;
}

}  // namespace ipcz
