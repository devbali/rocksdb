//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <algorithm>

#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <iostream>

#include "monitoring/statistics_impl.h"
#include "port/port.h"
#include "rocksdb/system_clock.h"
#include "test_util/sync_point.h"
#include "util/aligned_buffer.h"
#include "util/rate_limiter_multi_tenant_impl.h"
#include "util/tg_thread_local.h"

namespace ROCKSDB_NAMESPACE {

size_t RateLimiter::RequestToken(size_t bytes, size_t alignment,
                                 Env::IOPriority io_priority, Statistics* stats,
                                 RateLimiter::OpType op_type) {
  if (io_priority < Env::IO_TOTAL && IsRateLimited(op_type)) {
    bytes = std::min(bytes, static_cast<size_t>(GetSingleBurstBytes()));

    if (alignment > 0) {
      // Here we may actually require more than burst and block
      // as we can not write/read less than one page at a time on direct I/O
      // thus we do not want to be strictly constrained by burst
      bytes = std::max(alignment, TruncateToPageBoundary(alignment, bytes));
    }
    Request(bytes, io_priority, stats, op_type);
  }
  return bytes;
}

// Pending request
struct MultiTenantRateLimiter::Req {
  explicit Req(int64_t _bytes, port::Mutex* _mu)
      : request_bytes(_bytes), bytes(_bytes), cv(_mu) {}
  int64_t request_bytes;
  int64_t bytes;
  port::CondVar cv;
};

MultiTenantRateLimiter::MultiTenantRateLimiter(
    int64_t rate_bytes_per_sec, int64_t refill_period_us, int32_t fairness,
    RateLimiter::Mode mode, const std::shared_ptr<SystemClock>& clock,
    bool auto_tuned, int64_t single_burst_bytes, int64_t read_rate_bytes_per_sec)
    : RateLimiter(mode),
      refill_period_us_(refill_period_us),
      rate_bytes_per_sec_(auto_tuned ? rate_bytes_per_sec / 2
                                     : rate_bytes_per_sec),
      refill_bytes_per_period_(
          CalculateRefillBytesPerPeriodLocked(rate_bytes_per_sec_)),
      raw_single_burst_bytes_(single_burst_bytes),
      clock_(clock),
      stop_(false),
      exit_cv_(&request_mutex_),
      requests_to_wait_(0),
      // available_bytes_(0),
      next_refill_us_(NowMicrosMonotonicLocked()),
      fairness_(fairness > 100 ? 100 : fairness),
      rnd_((uint32_t)time(nullptr)),
      wait_until_refill_pending_(false),
      available_bytes_arr_{},
      read_rate_bytes_per_sec_(read_rate_bytes_per_sec) {
  for (int i = Env::IO_LOW; i < Env::IO_TOTAL; ++i) {
    total_requests_[i] = 0;
    total_bytes_through_[i] = 0;
  }
  for (int i = 0; i < kTGNumClients; ++i) {
    calls_per_client_[i] = 0;
    available_bytes_arr_[i] = 0;
  }
  std::cout << "[TGRIGGS_LOG] rate_bytes_per_sec_ = " << rate_bytes_per_sec_ << std::endl;

  if (read_rate_bytes_per_sec_ > 0) {
    read_rate_limiter_ = NewMultiTenantRateLimiter(
        read_rate_bytes_per_sec_, // <rate_limit> MB/s rate limit
        100 * 1000,        // Refill period = 100ms (default)
        10,                // Fairness (default)
        rocksdb::RateLimiter::Mode::kWritesOnly, // Apply only to writes
        false,              // Disable auto-tuning
        /* read_rate_limit = */ 0
    );
  }
}

MultiTenantRateLimiter::~MultiTenantRateLimiter() {
  MutexLock g(&request_mutex_);
  stop_ = true;
  std::deque<Req*>::size_type queues_size_sum = 0;
  for (int i = Env::IO_LOW; i < Env::IO_TOTAL; ++i) {
    queues_size_sum += queue_[i].size();
  }
  requests_to_wait_ = static_cast<int32_t>(queues_size_sum);

  for (int i = Env::IO_TOTAL - 1; i >= Env::IO_LOW; --i) {
    std::deque<Req*> queue = queue_[i];
    for (auto& r : queue) {
      r->cv.Signal();
    }
  }

  while (requests_to_wait_ > 0) {
    exit_cv_.Wait();
  }
}

// This API allows user to dynamically change rate limiter's bytes per second.
void MultiTenantRateLimiter::SetBytesPerSecond(int64_t bytes_per_second) {
  MutexLock g(&request_mutex_);
  SetBytesPerSecondLocked(bytes_per_second);
}

void MultiTenantRateLimiter::SetBytesPerSecondLocked(int64_t bytes_per_second) {
  assert(bytes_per_second > 0);
  rate_bytes_per_sec_.store(bytes_per_second, std::memory_order_relaxed);
  refill_bytes_per_period_.store(
      CalculateRefillBytesPerPeriodLocked(bytes_per_second),
      std::memory_order_relaxed);
}

Status MultiTenantRateLimiter::SetSingleBurstBytes(int64_t single_burst_bytes) {
  if (single_burst_bytes < 0) {
    return Status::InvalidArgument(
        "`single_burst_bytes` must be greater than or equal to 0");
  }

  MutexLock g(&request_mutex_);
  raw_single_burst_bytes_.store(single_burst_bytes, std::memory_order_relaxed);
  return Status::OK();
}

void MultiTenantRateLimiter::TGprintStackTrace() {
  void *array[10];
  size_t size;
  char **strings;
  size_t i;

  size = backtrace(array, 10);
  strings = backtrace_symbols(array, size);

  printf("Obtained %zd stack frames.\n", size);

  for (i = 0; i < size; i++)
      printf("%s\n", strings[i]);

  free(strings);
}

void MultiTenantRateLimiter::Request(int64_t bytes, const Env::IOPriority pri,
                                 Statistics* stats, OpType op_type) {
  if (op_type == RateLimiter::OpType::kRead) {
    if (read_rate_limiter_ != nullptr) {
      read_rate_limiter_->Request(bytes, pri, stats);
    }
    return;
  } else {
    Request(bytes, pri, stats);
  }
}
                                

void MultiTenantRateLimiter::Request(int64_t bytes, const Env::IOPriority pri,
                                 Statistics* stats) {
  auto& thread_metadata = TG_GetThreadMetadata();

  if (thread_metadata.client_id == 0) {
    // TGprintStackTrace();
  }

  if (thread_metadata.client_id == -2) {
    std::cout << "[TGRIGGS_LOG] bad input" << std::endl;
    return;
  }

  // Extract client ID from thread-local metadata.
  int client_id = TG_GetThreadMetadata().client_id;

  // Flush - don't block them (for now)
  // TODO:
  //    Idea: give support for splitting across certain clients
  if (thread_metadata.client_id == -1) {
    // std::cout << "[TGRIGGS_LOG] un-set client id" << std::endl;

    // Assiggn flushes to client 1
    client_id = 1;
    // return;
  }

  // std::cout << "[TGRIGGS_LOG] RL for client " << thread_metadata.client_id << std::endl;
  calls_per_client_[thread_metadata.client_id]++;
  if (total_calls_++ >= 1000) {
    total_calls_ = 0;
    std::cout << "[TGRIGGS_LOG] RL calls per-clients for ";
    if (read_rate_bytes_per_sec_ == 0) {
      std::cout << "READ: ";
    } else {
      std::cout << "WRITE: ";
    }
    for (const auto& calls : calls_per_client_) {
      std::cout << calls << ", ";
    }
    std::cout << std::endl;
  }

  assert(bytes <= GetSingleBurstBytes());
  bytes = std::max(static_cast<int64_t>(0), bytes);
  TEST_SYNC_POINT("MultiTenantRateLimiter::Request");
  TEST_SYNC_POINT_CALLBACK("MultiTenantRateLimiter::Request:1",
                           &rate_bytes_per_sec_);
  MutexLock g_lock(&request_mutex_);

  if (stop_) {
    // It is now in the clean-up of ~MultiTenantRateLimiter().
    // Therefore any new incoming request will exit from here
    // and not get satiesfied.
    return;
  }

  ++total_requests_[pri];

  // Draw from per-client token buckets.
  if (available_bytes_arr_[client_id] > 0) {
    int64_t bytes_through = std::min(available_bytes_arr_[client_id], bytes);
    total_bytes_through_[pri] += bytes_through;
    available_bytes_arr_[client_id] -= bytes_through;
    bytes -= bytes_through;
  } 

  if (bytes == 0) {
    // Granted!
    return;
  }

  // Request cannot be satisfied at this moment, enqueue
  Req req(bytes, &request_mutex_);

  // std::cout << "[TGRIGGS_LOG] Pushing back for client,pri,bytes: " << client_id << "," << pri << "," << bytes << std::endl;
  multi_tenant_queue_[client_id][pri].push_back(&req);
  // queue_[pri].push_back(&r);
  TEST_SYNC_POINT_CALLBACK("MultiTenantRateLimiter::Request:PostEnqueueRequest",
                           &request_mutex_);
  // A thread representing a queued request coordinates with other such threads.
  // There are two main duties.
  //
  // (1) Waiting for the next refill time.
  // (2) Refilling the bytes and granting requests.
  do {
    int64_t time_until_refill_us = next_refill_us_ - NowMicrosMonotonicLocked();
    if (time_until_refill_us > 0) {
      if (wait_until_refill_pending_) {
        // Somebody is performing (1). Trust we'll be woken up when our request
        // is granted or we are needed for future duties.
        req.cv.Wait();
      } else {
        // Whichever thread reaches here first performs duty (1) as described
        // above.
        int64_t wait_until = clock_->NowMicros() + time_until_refill_us;
        RecordTick(stats, NUMBER_RATE_LIMITER_DRAINS);
        wait_until_refill_pending_ = true;
        clock_->TimedWait(&req.cv, std::chrono::microseconds(wait_until));
        TEST_SYNC_POINT_CALLBACK("MultiTenantRateLimiter::Request:PostTimedWait",
                                 &time_until_refill_us);
        wait_until_refill_pending_ = false;
      }
    } else {
      // Whichever thread reaches here first performs duty (2) as described
      // above.
      RefillBytesAndGrantRequestsLocked();
    }
    if (req.request_bytes == 0) {
      // If there is any remaining requests, make sure there exists at least
      // one candidate is awake for future duties by signaling a front request
      // of a queue.
      for (int i = 0; i < kTGNumClients; ++i) {
        for (int j = Env::IO_TOTAL - 1; j >= Env::IO_LOW; --j) {
          auto& queue = multi_tenant_queue_[i][j];
          // auto& queue = queue_[i];
          if (!queue.empty()) {
            queue.front()->cv.Signal();
            break;
          }
        }
      }
    }
    // Invariant: non-granted request is always in one queue, and granted
    // request is always in zero queues.
// #ifndef NDEBUG
//     int num_found = 0;
//     for (int i = Env::IO_LOW; i < Env::IO_TOTAL; ++i) {
//       if (std::find(queue_[i].begin(), queue_[i].end(), &r) !=
//           queue_[i].end()) {
//         ++num_found;
//       }
//     }
//     if (r.request_bytes == 0) {
//       assert(num_found == 0);
//     } else {
//       assert(num_found == 1);
//     }
// #endif  // NDEBUG
  } while (!stop_ && req.request_bytes > 0);

  if (stop_) {
    // It is now in the clean-up of ~MultiTenantRateLimiter().
    // Therefore any woken-up request will have come out of the loop and then
    // exit here. It might or might not have been satisfied.
    --requests_to_wait_;
    exit_cv_.Signal();
  }
}

std::vector<Env::IOPriority>
MultiTenantRateLimiter::GeneratePriorityIterationOrderLocked() {
  std::vector<Env::IOPriority> pri_iteration_order(Env::IO_TOTAL /* 4 */);
  // We make Env::IO_USER a superior priority by always iterating its queue
  // first
  pri_iteration_order[0] = Env::IO_USER;

  bool high_pri_iterated_after_mid_low_pri = rnd_.OneIn(fairness_);
  TEST_SYNC_POINT_CALLBACK(
      "MultiTenantRateLimiter::GeneratePriorityIterationOrderLocked::"
      "PostRandomOneInFairnessForHighPri",
      &high_pri_iterated_after_mid_low_pri);
  bool mid_pri_itereated_after_low_pri = rnd_.OneIn(fairness_);
  TEST_SYNC_POINT_CALLBACK(
      "MultiTenantRateLimiter::GeneratePriorityIterationOrderLocked::"
      "PostRandomOneInFairnessForMidPri",
      &mid_pri_itereated_after_low_pri);

  if (high_pri_iterated_after_mid_low_pri) {
    pri_iteration_order[3] = Env::IO_HIGH;
    pri_iteration_order[2] =
        mid_pri_itereated_after_low_pri ? Env::IO_MID : Env::IO_LOW;
    pri_iteration_order[1] =
        (pri_iteration_order[2] == Env::IO_MID) ? Env::IO_LOW : Env::IO_MID;
  } else {
    pri_iteration_order[1] = Env::IO_HIGH;
    pri_iteration_order[3] =
        mid_pri_itereated_after_low_pri ? Env::IO_MID : Env::IO_LOW;
    pri_iteration_order[2] =
        (pri_iteration_order[3] == Env::IO_MID) ? Env::IO_LOW : Env::IO_MID;
  }

  TEST_SYNC_POINT_CALLBACK(
      "MultiTenantRateLimiter::GeneratePriorityIterationOrderLocked::"
      "PreReturnPriIterationOrder",
      &pri_iteration_order);
  return pri_iteration_order;
}

// TODO:
// 1) need to create N "available_bytes_" to track separate token buckets for each user. 
// 2) for compaction: belongs to single client_id
// 3) for flush: belongs to MULTIPLE client_id's, draw from each?? even if goes below zero??
//     a) idea: never block flush (except behind user requests) even if tokens are out
// 4) how to refresh tokens?
void MultiTenantRateLimiter::RefillBytesAndGrantRequestsLocked() {
  TEST_SYNC_POINT_CALLBACK(
      "MultiTenantRateLimiter::RefillBytesAndGrantRequestsLocked", &request_mutex_);
  next_refill_us_ = NowMicrosMonotonicLocked() + refill_period_us_;

  // Carry over the left over quota from the last period
  // TODO: don't understand how this ^^ is happening?
  auto refill_bytes_per_period =
      refill_bytes_per_period_.load(std::memory_order_relaxed);

  // TODO: are we breaking an invariant commenting this out?
  // assert(available_bytes_ == 0);
  // available_bytes_ = refill_bytes_per_period;

  // TODO: allow weighting of different clients.
  for (int i = 0; i < kTGNumClients; ++i) {
    available_bytes_arr_[i] = refill_bytes_per_period;
  }

  int client_order[kTGNumClients] = {0, 1, 2, 3, 4};
  std::random_shuffle(std::begin(client_order), std::end(client_order));

  // Logic
  // 1) iterate through each client (in random order)
  // 2) for each client, do strict priority order from IO_USER to IO_LOW
  for (int i = 0; i < kTGNumClients; ++i) {
    for (int j = Env::IO_TOTAL - 1; j >= Env::IO_LOW; --j) {
      auto* queue = &multi_tenant_queue_[client_order[i]][j];
      while (!queue->empty()) {
        auto* next_req = queue->front();
        if (available_bytes_arr_[i] < next_req->request_bytes) {
          // Grant partial request_bytes even if request is for more than
          // `available_bytes_`, which can happen in a few situations:
          //
          // - The available bytes were partially consumed by other request(s)
          // - The rate was dynamically reduced while requests were already
          //   enqueued
          // - The burst size was explicitly set to be larger than the refill size
          next_req->request_bytes -= available_bytes_arr_[i];
          available_bytes_arr_[i] = 0;
          break;
        }
        available_bytes_arr_[i] -= next_req->request_bytes;
        next_req->request_bytes = 0;
        total_bytes_through_[j] += next_req->bytes;
        // std::cout << "[TGRIGGS_LOG] Popping client,pri: " << i << "," << j << std::endl;
        queue->pop_front();

        // Quota granted, signal the thread to exit
        next_req->cv.Signal();
      }
    }
  }

  // std::vector<Env::IOPriority> pri_iteration_order =
  //     GeneratePriorityIterationOrderLocked();

  // for (int i = Env::IO_LOW; i < Env::IO_TOTAL; ++i) {
  //   assert(!pri_iteration_order.empty());
  //   Env::IOPriority current_pri = pri_iteration_order[i];
  //   auto* queue = &queue_[current_pri];
  //   while (!queue->empty()) {
  //     auto* next_req = queue->front();
  //     if (available_bytes_ < next_req->request_bytes) {
  //       // Grant partial request_bytes even if request is for more than
  //       // `available_bytes_`, which can happen in a few situations:
  //       //
  //       // - The available bytes were partially consumed by other request(s)
  //       // - The rate was dynamically reduced while requests were already
  //       //   enqueued
  //       // - The burst size was explicitly set to be larger than the refill size
  //       next_req->request_bytes -= available_bytes_;
  //       available_bytes_ = 0;
  //       break;
  //     }
  //     available_bytes_ -= next_req->request_bytes;
  //     next_req->request_bytes = 0;
  //     total_bytes_through_[current_pri] += next_req->bytes;
  //     queue->pop_front();

  //     // Quota granted, signal the thread to exit
  //     next_req->cv.Signal();
  //   }
  // }
}

int64_t MultiTenantRateLimiter::CalculateRefillBytesPerPeriodLocked(
    int64_t rate_bytes_per_sec) {
  if (std::numeric_limits<int64_t>::max() / rate_bytes_per_sec <
      refill_period_us_) {
    // Avoid unexpected result in the overflow case. The result now is still
    // inaccurate but is a number that is large enough.
    return std::numeric_limits<int64_t>::max() / kMicrosecondsPerSecond;
  } else {
    return rate_bytes_per_sec * refill_period_us_ / kMicrosecondsPerSecond;
  }
}
RateLimiter* NewMultiTenantRateLimiter(
    int64_t rate_bytes_per_sec, int64_t refill_period_us /* = 100 * 1000 */,
    int32_t fairness /* = 10 */,
    RateLimiter::Mode mode /* = RateLimiter::Mode::kWritesOnly */,
    bool auto_tuned /* = false */, int64_t single_burst_bytes /* = 0 */,
    int64_t read_rate_bytes_per_sec /* = 0 */) {
  assert(rate_bytes_per_sec > 0);
  assert(refill_period_us > 0);
  assert(fairness > 0);
  std::unique_ptr<RateLimiter> limiter(new MultiTenantRateLimiter(
      rate_bytes_per_sec, refill_period_us, fairness, mode,
      SystemClock::Default(), auto_tuned, single_burst_bytes, read_rate_bytes_per_sec));
  return limiter.release();
}

}  // namespace ROCKSDB_NAMESPACE
