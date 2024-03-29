//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once
#include <atomic>
#include <memory>
#include <utility>
#include "memory/allocator.h"
#include "memory/arena.h"
#include "port/lang.h"
#include "port/likely.h"
#include "util/core_local.h"
#include "util/mutexlock.h"
#include "util/thread_local.h"

// Only generate field unused warning for padding array, or build under
// GCC 4.8.1 will fail.
#ifdef __clang__
#define ROCKSDB_FIELD_UNUSED __attribute__((__unused__))
#else
#define ROCKSDB_FIELD_UNUSED
#endif  // __clang__

namespace ROCKSDB_NAMESPACE {

class Logger;

// ConcurrentArena wraps an Arena.  It makes it thread safe using a fast
// inlined spinlock, and adds small per-core allocation caches to avoid
// contention for small allocations.  To avoid any memory waste from the
// per-core shards, they are kept small, they are lazily instantiated
// only if ConcurrentArena actually notices concurrent use, and they
// adjust their size so that there is no fragmentation waste when the
// shard blocks are allocated from the underlying main arena.
class ConcurrentArena : public Allocator {
 public:
  // block_size and huge_page_size are the same as for Arena (and are
  // in fact just passed to the constructor of arena_.  The core-local
  // shards compute their shard_block_size as a fraction of block_size
  // that varies according to the hardware concurrency level.
  explicit ConcurrentArena(size_t block_size = Arena::kMinBlockSize,
                           AllocTracker* tracker = nullptr,
                           size_t huge_page_size = 0);

  char* Allocate(size_t bytes) override {
    return AllocateImpl(bytes, false /*force_arena*/,
                        [this, bytes]() { return arena_.Allocate(bytes); });
  }

  char* AllocateAligned(size_t bytes, size_t huge_page_size = 0,
                        Logger* logger = nullptr) override {
    // 修正分配的内存大小为 指针大小的整数倍
    size_t rounded_up = ((bytes - 1) | (sizeof(void*) - 1)) + 1;
    assert(rounded_up >= bytes && rounded_up < bytes + sizeof(void*) &&
           (rounded_up % sizeof(void*)) == 0);

    return AllocateImpl(rounded_up, huge_page_size != 0 /*force_arena*/,
                        [this, rounded_up, huge_page_size, logger]() {
                          return arena_.AllocateAligned(rounded_up,
                                                        huge_page_size, logger);
                        });
  }

  size_t ApproximateMemoryUsage() const {
    std::unique_lock<SpinMutex> lock(arena_mutex_, std::defer_lock);
    lock.lock();
    return arena_.ApproximateMemoryUsage() - ShardAllocatedAndUnused();
  }

  size_t MemoryAllocatedBytes() const {
    return memory_allocated_bytes_.load(std::memory_order_relaxed);
  }

  size_t AllocatedAndUnused() const {
    return arena_allocated_and_unused_.load(std::memory_order_relaxed) +
           ShardAllocatedAndUnused();
  }

  size_t IrregularBlockNum() const {
    return irregular_block_num_.load(std::memory_order_relaxed);
  }

  size_t BlockSize() const override { return arena_.BlockSize(); }

 private:
  struct Shard {
    char padding[40] ROCKSDB_FIELD_UNUSED;
    mutable SpinMutex mutex;
    char* free_begin_;
    std::atomic<size_t> allocated_and_unused_;

    Shard() : free_begin_(nullptr), allocated_and_unused_(0) {}
  };

  static thread_local size_t tls_cpuid;

  char padding0[56] ROCKSDB_FIELD_UNUSED;

  size_t shard_block_size_;

  CoreLocalArray<Shard> shards_;

  Arena arena_;
  mutable SpinMutex arena_mutex_;
  std::atomic<size_t> arena_allocated_and_unused_;
  std::atomic<size_t> memory_allocated_bytes_;
  std::atomic<size_t> irregular_block_num_;

  char padding1[56] ROCKSDB_FIELD_UNUSED;

  Shard* Repick();

  size_t ShardAllocatedAndUnused() const {
    size_t total = 0;
    for (size_t i = 0; i < shards_.Size(); ++i) {
      total += shards_.AccessAtCore(i)->allocated_and_unused_.load(
          std::memory_order_relaxed);
    }
    return total;
  }

  template <typename Func>
  char* AllocateImpl(size_t bytes, bool force_arena, const Func& func) {
    size_t cpu;

    // Go directly to the arena if the allocation is too large, or if
    // we've never needed to Repick() and the arena mutex is available
    // with no waiting.  This keeps the fragmentation penalty of
    // concurrency zero unless it might actually confer an advantage.
    std::unique_lock<SpinMutex> arena_lock(arena_mutex_, std::defer_lock);
    if (bytes > shard_block_size_ / 4 || force_arena ||
        ((cpu = tls_cpuid) == 0 &&
         !shards_.AccessAtCore(0)->allocated_and_unused_.load(
             std::memory_order_relaxed) &&
         arena_lock.try_lock())) {

      if (!arena_lock.owns_lock()) {
        arena_lock.lock();
      }
      // 调用函数进行内存分配
      auto rv = func();
      // 对当前的内存情况的统计数据进行更新
      Fixup();
      return rv;
    }

    // pick a shard from which to allocate
    // cpu & (shards_.Size() - 1)) 等价于 cpu % (shards_.Size() - 1),
    // 因为 shards_.Size() 是 等于 2 的 n次方, 其代码逻辑为 static_cast<size_t>(1) << size_shift_
    Shard* s = shards_.AccessAtCore(cpu & (shards_.Size() - 1));
    if (!s->mutex.try_lock()) {

      // 如果当前使用的cpu获取锁失败了，则尝试重新另选一个cpu，并进行加锁操作
      s = Repick();
      s->mutex.lock();
    }
    // 转移 mutex 的所有权 到 lock 实例上，这样当前函数执行完后，lock会被自动释放
    std::unique_lock<SpinMutex> lock(s->mutex, std::adopt_lock);

    size_t avail = s->allocated_and_unused_.load(std::memory_order_relaxed);
    if (avail < bytes) {
      // reload
      // 因为shard 的内存储量不足，下面需要从arena 里面取内存放到shard 里面，所有需要加锁
      std::lock_guard<SpinMutex> reload_lock(arena_mutex_);

      // If the arena's current block is within a factor of 2 of the right
      // size, we adjust our request to avoid arena waste.
      auto exact = arena_allocated_and_unused_.load(std::memory_order_relaxed);
      assert(exact == arena_.AllocatedAndUnused());
      if (exact >= bytes && arena_.IsInInlineBlock()) {
        // If we haven't exhausted arena's inline block yet, allocate from arena
        // directly. This ensures that we'll do the first few small allocations
        // without allocating any blocks.
        // In particular this prevents empty memtables from using
        // disproportionately large amount of memory: a memtable allocates on
        // the order of 1 KB of memory when created; we wouldn't want to
        // allocate a full arena block (typically a few megabytes) for that,
        // especially if there are thousands of empty memtables.
        auto rv = func();
        Fixup();
        return rv;
      }

      avail = exact >= shard_block_size_ / 2 && exact < shard_block_size_ * 2
                  ? exact
                  : shard_block_size_;
      s->free_begin_ = arena_.AllocateAligned(avail);
      Fixup();
    }
    s->allocated_and_unused_.store(avail - bytes, std::memory_order_relaxed);

    char* rv;
    // 小优化：如果当前是 指针的整数倍数的话，则按 内存对齐来分配内存，这样可以提高cpu读取内存的速度。
    // 反之，如果不是指针的整数倍的话，按不按内存对齐来分配都没有什么所谓了
    // 这里之所以是按照指针的整数倍来算，这里个人觉得是跟cpu的高速缓存和cpu之间伪共享有关系，按指针整数倍有利于
    // 充分利用高速缓存，同时减少多个cpu同时操作同一个内存页的不同数据时，因为伪共享所产生的数据同步开销的放大
    if ((bytes % sizeof(void*)) == 0) {
      // aligned allocation from the beginning
      rv = s->free_begin_;
      s->free_begin_ += bytes;
    } else {
      // unaligned from the end
      rv = s->free_begin_ + avail - bytes;
    }
    return rv;
  }

  void Fixup() {
    arena_allocated_and_unused_.store(arena_.AllocatedAndUnused(),
                                      std::memory_order_relaxed);
    memory_allocated_bytes_.store(arena_.MemoryAllocatedBytes(),
                                  std::memory_order_relaxed);
    irregular_block_num_.store(arena_.IrregularBlockNum(),
                               std::memory_order_relaxed);
  }

  ConcurrentArena(const ConcurrentArena&) = delete;
  ConcurrentArena& operator=(const ConcurrentArena&) = delete;
};

}  // namespace ROCKSDB_NAMESPACE
