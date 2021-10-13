// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_MEM_SPSC_QUEUE_H_
#define IPCZ_SRC_MEM_SPSC_QUEUE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "mem/atomic_memcpy.h"

namespace ipcz {
namespace mem {

// SpscQueue is a single-producer, single-consumer, bounded, lock-free queue
// structure suitable for use by up to two concurrent threads: one producer and
// one consumer. SpscQueue objects do not contain heap references and are safe
// to allocate and use within shared memory regions as long as the
// single-producer and single-consumer constraints are upheld.
//
// The underlying element type T must be trivially copyable, and the consumer
// should pop frequently to avoid starving the producer.
template <typename T, size_t kCapacity>
class SpscQueue {
 public:
  using ElementType = T;

  static_assert(std::is_trivially_copyable<ElementType>::value,
                "SpscQueue elements must be trivially copyable");

  // Maximum number of iterations to spin in PushBack when the queue is full.
  // Arbitrary smallish number.
  static constexpr size_t kMaxPushAttempts = 10;

  SpscQueue() = default;
  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;
  ~SpscQueue() = default;

  // Tries to push `value` onto the end of the queue. If there's no capacity
  // after a few cycles of spinning (kMaxPushAttempts), returns `false` to
  // indicate failure. The queue is unmodified in this case and no data is
  // copied. Otherwise a copy of `value` is enqueued and this returns `true`.
  //
  // Note that this is designed to support only a single concurrent caller. If
  // multiple threads race to call this method, behavior is undefined.
  bool PushBack(const ElementType& value) {
    size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_relaxed);
    size_t new_tail = tail + 1;
    if (new_tail > kCapacity)
      new_tail = 0;

    // Queue buffer would be full, which is indistinguishable from empty (i.e.
    // head==tail). Spin until there's more space.
    size_t attempts = 0;
    while (new_tail == head) {
      head = head_.load(std::memory_order_relaxed);
      if (attempts >= kMaxPushAttempts)
        return false;
    }

    AtomicWriteMemcpy(&elements_[tail], &value, sizeof(value));
    tail_.store(new_tail, std::memory_order_release);
    return true;
  }

  // Pops an element off the front of the queue and into `value` if the queue is
  // non-empty. Returns `false` to indicate an empty queue, leaving `value`
  // unmodified. Otherwise the frontmost element is copied into `value` and
  // `true` is returned.
  bool PopFront(ElementType& value) {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_relaxed);
    if (head == tail)
      return false;

    AtomicReadMemcpy(&value, &elements_[head], sizeof(value));
    size_t new_head = head + 1;
    if (new_head > kCapacity)
      new_head = 0;
    head_.store(new_head, std::memory_order_release);
    return true;
  }

 private:
  // Index of the current head of the queue in `elements_`. Only written by the
  // consumer (PopFront()).
  std::atomic<size_t> head_;

  // Index of the current tail of the queue in `elements_`. Only written by the
  // producer (PushBack()).
  std::atomic<size_t> tail_;

  // While access to `elements_` is not explicitly guarded, it is rendered safe
  // with proper usage of this class: as long as there are no racing concurrent
  // calls to PushBack, and no racing concurrent calls to PopFront, no region of
  // this array will be subjected to unsafe overlapping access.
  ElementType elements_[kCapacity + 1];
};

}  // namespace mem
}  // namespace ipcz

#endif  // IPCZ_SRC_MEM_SPSC_QUEUE_H_
