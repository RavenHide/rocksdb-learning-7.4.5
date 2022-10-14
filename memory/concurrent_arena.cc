//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "memory/concurrent_arena.h"
#include <thread>
#include "port/port.h"
#include "util/random.h"

namespace ROCKSDB_NAMESPACE {

thread_local size_t ConcurrentArena::tls_cpuid = 0;

namespace {
// If the shard block size is too large, in the worst case, every core
// allocates a block without populate it. If the shared block size is
// 1MB, 64 cores will quickly allocate 64MB, and may quickly trigger a
// flush. Cap the size instead.
const size_t kMaxShardBlockSize = size_t{128 * 1024};
}  // namespace

ConcurrentArena::ConcurrentArena(size_t block_size, AllocTracker* tracker,
                                 size_t huge_page_size)
    : shard_block_size_(std::min(kMaxShardBlockSize, block_size / 8)),
      shards_(),
      arena_(block_size, tracker, huge_page_size) {
  Fixup();
}

// 重新选一个cpu的local cache
ConcurrentArena::Shard* ConcurrentArena::Repick() {
  // ----------------------
  // | first   |  second  |
  // | shard   | cpu_no   |
  // ----------------------
  auto shard_and_index = shards_.AccessElementAndIndex();
  // even if we are cpu 0, use a non-zero tls_cpuid so we can tell we
  // have repicked
  // shards_.Size() = 2 的 n 次方，而 shard_and_index.second | shards_.Size() 的结果
  // 是离散分布 且呈指数级别增长， 指数为2, 如
  // 2^n - 1, 2^(n+1) - 1, 2^(n+2) -1， 而tls_cpuid 只能是 该分布下的其中一个值，
  // 并且取决于 shard_and_index.second的大小
  // 举个例子，若 2^n - 1 < shard_and_index.second <= 2^(n+1) - 1，则tls_cpuid = 2^(n+1)
  tls_cpuid = shard_and_index.second | shards_.Size();
  return shard_and_index.first;
}

}  // namespace ROCKSDB_NAMESPACE
