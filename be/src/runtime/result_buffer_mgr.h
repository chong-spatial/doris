// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include <cctz/time_zone.h>
#include <gen_cpp/Types_types.h>
#include <gen_cpp/segment_v2.pb.h>

#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "common/status.h"
#include "gutil/ref_counted.h"
#include "util/countdown_latch.h"
#include "util/hash_util.hpp"

namespace arrow {
class RecordBatch;
class Schema;
} // namespace arrow

namespace doris {

class BufferControlBlock;
struct GetResultBatchCtx;
struct GetArrowResultBatchCtx;
class PUniqueId;
class RuntimeState;
class MemTrackerLimiter;
class Thread;
namespace vectorized {
class Block;
} // namespace vectorized

// manage all result buffer control block in one backend
class ResultBufferMgr {
public:
    ResultBufferMgr();
    ~ResultBufferMgr() = default;
    // init Result Buffer Mgr, start cancel thread
    Status init();

    void stop();

    // create one result sender for this query_id
    // the returned sender do not need release
    // sender is not used when call cancel or unregister
    Status create_sender(const TUniqueId& query_id, int buffer_size,
                         std::shared_ptr<BufferControlBlock>* sender, RuntimeState* state);

    // fetch data result to FE
    void fetch_data(const PUniqueId& finst_id, GetResultBatchCtx* ctx);
    // fetch data result to Arrow Flight Client
    Status fetch_arrow_data(const TUniqueId& finst_id, std::shared_ptr<vectorized::Block>* result,
                            cctz::time_zone& timezone_obj);
    // fetch data result to Other BE forwards to Client
    void fetch_arrow_data(const PUniqueId& finst_id, GetArrowResultBatchCtx* ctx);
    Status find_mem_tracker(const TUniqueId& finst_id,
                            std::shared_ptr<MemTrackerLimiter>* mem_tracker);
    Status find_arrow_schema(const TUniqueId& query_id, std::shared_ptr<arrow::Schema>* schema);

    // cancel
    void cancel(const TUniqueId& fragment_id);

    // cancel one query at a future time.
    void cancel_at_time(time_t cancel_time, const TUniqueId& query_id);

private:
    using BufferMap = std::unordered_map<TUniqueId, std::shared_ptr<BufferControlBlock>>;
    using TimeoutMap = std::map<time_t, std::vector<TUniqueId>>;

    std::shared_ptr<BufferControlBlock> find_control_block(const TUniqueId& query_id);

    // used to erase the buffer that fe not clears
    // when fe crush, this thread clear the buffer avoid memory leak in this backend
    void cancel_thread();

    // lock for buffer map
    std::shared_mutex _buffer_map_lock;
    // buffer block map
    BufferMap _buffer_map;

    // lock for timeout map
    std::mutex _timeout_lock;

    // map (cancel_time : query to be cancelled),
    // cancel time maybe equal, so use one list
    TimeoutMap _timeout_map;

    CountDownLatch _stop_background_threads_latch;
    scoped_refptr<Thread> _clean_thread;
};

} // namespace doris
