// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util/mpsc_queue.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "ipcz/ipcz.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace internal {

namespace {

// Bits in each cell's atomic status word.
constexpr uint32_t kLapMask = 0x3fffffff;
constexpr uint32_t kBusyBit = 0x40000000;
constexpr uint32_t kFullBit = 0x80000000;

using CellStatus = std::atomic<uint32_t>;

}  // namespace

struct IPCZ_ALIGN(8) MpscQueueBase::Data {
  std::atomic<uint32_t> tail;
  uint32_t reserved0;

  CellStatus& cell(size_t element_size, size_t index) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(this + 1);
    const size_t offset = index * (element_size + sizeof(CellStatus));
    return *reinterpret_cast<CellStatus*>(base + offset);
  }

  static size_t GetMaxNumCells(size_t region_size, size_t element_size) {
    const size_t max_num_cells =
        (region_size - sizeof(Data)) / (element_size + sizeof(CellStatus));
    ABSL_ASSERT(ComputeSpaceRequiredFor(element_size, max_num_cells) <=
                region_size);
    return max_num_cells;
  }
};

MpscQueueBase::MpscQueueBase() = default;

MpscQueueBase::MpscQueueBase(absl::Span<uint8_t> region, size_t element_size)
    : region_(region),
      element_size_(element_size),
      num_cells_(Data::GetMaxNumCells(region.size(), element_size)),
      max_index_(((kLapMask + 1) / num_cells_) * num_cells_) {
  ABSL_ASSERT(element_size_ < region_.size());
}

MpscQueueBase::MpscQueueBase(const MpscQueueBase&) = default;

MpscQueueBase& MpscQueueBase::operator=(const MpscQueueBase&) = default;

MpscQueueBase::~MpscQueueBase() = default;

// static
size_t MpscQueueBase::ComputeSpaceRequiredFor(size_t element_size,
                                              size_t num_elements) {
  return sizeof(Data) + num_elements * (element_size + sizeof(CellStatus));
}

void MpscQueueBase::InitializeRegion() {
  memset(region_.data(), 0, region_.size());
}

bool MpscQueueBase::PushBytes(absl::Span<const uint8_t> bytes) {
  ABSL_ASSERT(bytes.size() == element_size_);

  uint32_t tail = data().tail.load(std::memory_order_relaxed);
  for (;;) {
    CellStatus& cell = data().cell(element_size_, tail % num_cells_);
    const uint32_t tail_lap = tail / num_cells_;
    uint32_t status = tail_lap;
    if (cell.compare_exchange_strong(status, tail_lap | kBusyBit,
                                     std::memory_order_acquire,
                                     std::memory_order_relaxed)) {
      data().tail.store((tail + 1) % max_index_, std::memory_order_relaxed);
      memcpy(&cell + 1, bytes.data(), bytes.size());
      cell.store(tail_lap | kFullBit, std::memory_order_release);
      return true;
    }

    if ((status & kLapMask) != tail_lap) {
      // The queue is full.
      return false;
    }

    tail = data().tail.load(std::memory_order_relaxed);
  }
}

bool MpscQueueBase::Pop() {
  const uint32_t head_lap = head_ / num_cells_;
  CellStatus& cell = data().cell(element_size_, head_ % num_cells_);
  uint32_t status = head_lap | kFullBit;
  if (!cell.compare_exchange_strong(status, head_lap | kBusyBit,
                                    std::memory_order_acquire,
                                    std::memory_order_relaxed)) {
    return false;
  }

  head_ = (head_ + 1) % max_index_;
  cell.store((head_lap + 1) & kLapMask, std::memory_order_release);
  return true;
}

void* MpscQueueBase::PeekBytes() {
  CellStatus& cell = data().cell(element_size_, head_ % num_cells_);
  if ((cell.load(std::memory_order_relaxed) & kFullBit) == 0) {
    return nullptr;
  }

  return &cell + 1;
}

}  // namespace internal
}  // namespace ipcz
