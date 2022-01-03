// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_MEM_MPMC_QUEUE_H_
#define IPCZ_SRC_MEM_MPMC_QUEUE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "ipcz/ipcz.h"
#include "mem/atomic_memcpy.h"
#include "third_party/abseil-cpp/absl/base/macros.h"

namespace ipcz {
namespace mem {

namespace internal {

// Queue data which potentially resides in shared memory. The capacity of the
// queue is not stored here since it should not be trusted between arbitrary
// processes.
template <typename T>
struct MpmcQueueData {
  MpmcQueueData() = default;
  ~MpmcQueueData() = default;

  void Initialize(size_t capacity) {
    new (this) MpmcQueueData<T>();
    memset(&first_cell_, 0, sizeof(Cell) * capacity);
  }

  // Tries to push `value` onto the tail of the queue. Returns false on failure,
  // either if the queue size would exceed `capacity`.
  bool Push(const T& value, const size_t capacity) {
    uint32_t tail = tail_.load(std::memory_order_relaxed);
    for (;;) {
      const uint32_t tail_lap = tail / capacity;
      const uint32_t empty_status = tail_lap << 2;
      Cell& c = cell(tail % capacity);
      uint32_t status = empty_status;
      if (c.status.compare_exchange_weak(status, empty_status | kBusyBit,
                                         std::memory_order_release,
                                         std::memory_order_relaxed)) {
        // Successfully claimed the slot. Copy in the data, increment the tail
        // index if necessary, and mark the slot as full and no longer busy.
        AtomicWriteMemcpy(&c.data, &value, sizeof(c.data));
        tail_.compare_exchange_strong(tail, tail + 1,
                                      std::memory_order_relaxed);
        c.status.store(empty_status | kFullBit, std::memory_order_release);
        return true;
      }

      // If this was a spurious CAS failure, simply retry as-is.
      if (status == empty_status) {
        continue;
      }

      // If the cell was already full and is on the lap before our current tail
      // lap, we've just bumped into the head. The queue is full.
      const uint32_t next_cell_lap = (status >> 2) + 1;
      if (next_cell_lap == tail_lap && (status & kFullBit) != 0) {
        return false;
      }

      // Otherwise we've just lost a race. Reload the tail index and try again.
      tail = tail_.load(std::memory_order_relaxed);
    }
  }

  // Tries to pop a value off the front of the queue and into `value`. Returns
  // false if the queue was empty.
  bool Pop(T& value, const size_t capacity) {
    uint32_t head = head_.load(std::memory_order_relaxed);
    for (;;) {
      const uint32_t head_lap = head / capacity;
      const uint32_t full_status = (head_lap << 2) | kFullBit;
      Cell& c = cell(head % capacity);
      uint32_t status = full_status;
      if (c.status.compare_exchange_weak(status, full_status | kBusyBit,
                                         std::memory_order_release,
                                         std::memory_order_relaxed)) {
        // Successfully claimed the slot.
        AtomicReadMemcpy(&value, &c.data, sizeof(value));
        head_.compare_exchange_strong(head, head + 1,
                                      std::memory_order_relaxed);

        c.status.store((head_lap + 1) << 2, std::memory_order_release);
        return true;
      }

      // Spurious failure. Retry as-is.
      if (status == full_status) {
        continue;
      }

      // If kFullBit is not set and the cell is still on our current head lap,
      // the slot is empty and there are no populated slots which follow it; so
      // the queue itself is empty.
      const uint32_t current_cell_lap = status >> 2;
      if (!(status & kFullBit) && current_cell_lap == head_lap) {
        return false;
      }

      // Otherwise we've just lost a race. Reload the tail index and try again.
      head = head_.load(std::memory_order_relaxed);
    }
  }

  static constexpr size_t GetFixedStorageSize() {
    return ComputeStorageSize(1) - GetPerElementStorageSize();
  }

  static constexpr size_t GetPerElementStorageSize() { return sizeof(Cell); }

  static constexpr size_t ComputeStorageSize(size_t capacity) {
    return sizeof(MpmcQueueData<T>) +
           GetPerElementStorageSize() * (capacity - 1);
  }

 private:
  static constexpr uint32_t kBusyBit = 1 << 0;
  static constexpr uint32_t kFullBit = 1 << 1;

  struct Cell {
    T data;
    std::atomic<uint32_t> status{0};
  };

  Cell& cell(size_t index) { return *(&first_cell_ + index); }

  std::atomic<uint32_t> head_{0};
  std::atomic<uint32_t> tail_{0};

  // The first cell of the queue. Remaining cells follow in contiguous memory,
  Cell first_cell_;
};

}  // namespace internal

// MpmcQueue is a multiple-producer, multiple-consumer, bounded, lock-free queue
// structure suitable for use by any number of concurrent producers and
// consumers. The underlying data type T must be trivially copyable, and
// consumers should pop frequently to avoid starving producers.
//
// MpmcQueue itself should live in a process's private memory, but the
// underlying data structure may live in untrusted shared memory.
template <typename T>
class MpmcQueue {
 public:
  enum { kInitializeData };
  enum { kAlreadyInitialized };

  static_assert(std::is_trivially_copyable<T>::value,
                "data must be trivially copyable");

  static constexpr size_t ComputeStorageSize(size_t capacity) {
    return internal::MpmcQueueData<T>::ComputeStorageSize(capacity);
  }

  MpmcQueue(void* memory, size_t capacity, decltype(kInitializeData))
      : MpmcQueue(memory, capacity, kAlreadyInitialized) {
    data_.Initialize(capacity);
  }

  MpmcQueue(void* memory, size_t capacity, decltype(kAlreadyInitialized))
      : capacity_(capacity),
        data_(*static_cast<internal::MpmcQueueData<T>*>(memory)) {}
  ~MpmcQueue() = default;

  bool Push(const T& value) { return data_.Push(value, capacity_); }

  bool Pop(T& value) { return data_.Pop(value, capacity_); }

  static constexpr size_t GetFixedStorageSize() {
    return internal::MpmcQueueData<T>::GetFixedStorageSize();
  }

  static constexpr size_t GetPerElementStorageSize() {
    return internal::MpmcQueueData<T>::GetPerElementStorageSize();
  }

 private:
  const size_t capacity_;
  internal::MpmcQueueData<T>& data_;
};

}  // namespace mem
}  // namespace ipcz

#endif  // IPCZ_SRC_MEM_MPMC_QUEUE_H_
