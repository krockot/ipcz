// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_MEM_SEQLOCKED_DATA_H_
#define IPCZ_SRC_MEM_SEQLOCKED_DATA_H_

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ipcz {
namespace mem {

// Helper to perform an atomic mempcy out of `src` and into `dest`. Only returns
// once the entire copy can be completed without the value at `version`
// changing mid-copy.
void ReadSeqlockedMemory(void* dest,
                         const void* src,
                         size_t size,
                         std::atomic<uint32_t>& version);

// Helper to perform an atomic memcpy into `dest` from `src`. Increments
// `version` once before and after the copy to serve as a barrier for consumers
// of the data at `dest` using ReadSeqlockedData above.
void WriteSeqlockedMemory(void* dest,
                          const void* src,
                          size_t size,
                          std::atomic<uint32_t>& version);

// SeqlockedData defines a data object suitable for concurrent access by a
// single infrequent writer and many frequent readers. Access is guarded by a
// sequence lock. The underlying type T must be trivially copyable.
//
// Instances of this object can live in shared memory and be used for
// low-latency data synchronization between processes.
//
// Additional reading:
//   https://en.wikipedia.org/wiki/Seqlock
//   http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2019/p1478r1.html
template <typename T>
class SeqlockedData {
 public:
  using DataType = T;

  static_assert(std::is_trivially_copyable<DataType>::value,
                "Seqlocked data must be trivially copyable");

  template <typename... Args>
  constexpr explicit SeqlockedData(Args&&... args)
      : data_(std::forward<Args>(args)...) {}

  SeqlockedData(const SeqlockedData&) = delete;
  SeqlockedData& operator=(const SeqlockedData&) = delete;

  ~SeqlockedData() = default;

  DataType Get() {
    union {
      struct {
      } dummy;
      DataType data;
    };
    ReadSeqlockedMemory(&data, &data_, sizeof(data), version_);
    return data;
  }

  void Set(const DataType& data) {
    WriteSeqlockedMemory(&data_, &data, sizeof(data), version_);
  }

 private:
  std::atomic<uint32_t> version_{0};
  DataType data_;
};

}  // namespace mem
}  // namespace ipcz

#endif  // IPCZ_SRC_MEM_SEQLOCKED_DATA_H_
