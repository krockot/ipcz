// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_MEM_MPSC_QUEUE_H_
#define IPCZ_SRC_MEM_MPSC_QUEUE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "mem/atomic_memcpy.h"
#include "third_party/abseil-cpp/absl/base/macros.h"

namespace ipcz {
namespace mem {

// MpscQueue is a multiple-producer, single-consumer, bounded, lock-free queue
// structure suitable for use by a single consumer thread and any number of
// concurrent producer threads. MqscQueue objects do not contain heap references
// and are safe to allocate and use within shared memory regions as long as the
// single-consumer constraint is upheld.
//
// The underlying element type T must be trivially copyable, and the consumer
// should pop frequently to avoid starving producers.
template <typename T, size_t kCapacity>
class MpscQueue {
 public:
  using DataType = T;

  static_assert(std::is_trivially_copyable<DataType>::value,
                "MpscQueue elements must be trivially copyable");

  // Maximum number of iterations to spin in PushBack when the queue is full.
  // Arbitrary smallish number.
  static constexpr size_t kMaxPushAttempts = 10;

  // Maximum number of iterations to spin in PopBack when a head element is
  // present but busy, before giving up and treating the queue as still empty.
  static constexpr size_t kMaxPopAttempts = 10;

  MpscQueue() = default;
  MpscQueue(const MpscQueue&) = delete;
  MpscQueue& operator=(const MpscQueue&) = delete;
  ~MpscQueue() = default;

  // Tries to push `value` onto the end of the queue. If there's no capacity
  // after a few cycles of spinning (kMaxPushAttempts), returns `false` to
  // indicate failure. The queue is unmodified in this case and no data is
  // copied. Otherwise a copy of `value` is enqueued and this returns `true`.
  // This is safe to call concurrently from any number of producer threads.
  bool PushBack(const DataType& value) {
    size_t attempts = 0;
    while (count_.fetch_add(1, std::memory_order_acquire) >= kCapacity) {
      count_.fetch_sub(1, std::memory_order_release);
      if (++attempts > kMaxPushAttempts)
        return false;
    }

    size_t index = tail_.fetch_add(1, std::memory_order_acquire);
    if (index >= kCapacity) {
      index -= kCapacity;
      if (index == 0)
        tail_.fetch_sub(kCapacity, std::memory_order_relaxed);
    }

    Element& e = elements_[index];
    ABSL_ASSERT(!e.ready.load(std::memory_order_acquire));
    AtomicWriteMemcpy(&e.data, &value, sizeof(value));
    e.ready.store(true, std::memory_order_release);
    return true;
  }

  // Pops an element off the front of the queue and into `value` if the queue is
  // non-empty. Returns `false` to indicate an empty queue, leaving `value`
  // unmodified. Otherwise the frontmost element is copied into `value` and
  // `true` is returned. This must never be called by more than one thread at a
  // time, but it's fine to overlap with concurrent PushBack calls.
  bool PopFront(DataType& value) {
    const size_t count = count_.load(std::memory_order_acquire);
    if (count == 0)
      return false;

    const size_t head = head_.load(std::memory_order_acquire);
    Element& e = elements_[head];
    size_t attempts = 0;
    bool ready = true;
    while (!e.ready.compare_exchange_weak(
        ready, false, std::memory_order_acquire, std::memory_order_relaxed)) {
      if (++attempts > kMaxPopAttempts)
        return false;
      ready = true;
    }

    AtomicReadMemcpy(&value, &e.data, sizeof(value));

    size_t next_head = head + 1;
    if (next_head == kCapacity)
      next_head = 0;
    head_.store(next_head, std::memory_order_relaxed);

    count_.fetch_sub(1, std::memory_order_release);
    return true;
  }

 private:
  struct Element {
    DataType data;
    std::atomic_bool ready;
  };

  // Number of elements present in the queue. Note that this is just a count of
  // elements which have been allocated be some producer; not all elements are
  // such necessarily available yet.
  std::atomic<size_t> count_{0};

  // Index of the current head of the queue in `elements_`. Only written by
  // consumers (PopBack()).
  std::atomic<size_t> head_{0};

  // Index of the current tail of the queue in `elements_`. Only written by
  // producers (PushBack()), and only once the producer has been able to reserve
  // space by incrementing `count_` without overflowing capacity.
  std::atomic<size_t> tail_{0};

  // While access to `elements_` is not explicitly guarded, it is rendered safe
  // with proper usage of this class: as long as there are no racing concurrent
  // calls to PopFront, no region of this array will be subjected to unsafe
  // overlapping access.
  Element elements_[kCapacity];
};

}  // namespace mem
}  // namespace ipcz

#endif  // IPCZ_SRC_MEM_MPSC_QUEUE_H_
