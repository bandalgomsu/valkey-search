/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#include "src/query/fanout.h"

#include <netinet/in.h>

#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "grpcpp/support/status.h"
#include "src/attribute_data_type.h"
#include "src/coordinator/client_pool.h"
#include "src/coordinator/coordinator.pb.h"
#include "src/coordinator/search_converter.h"
#include "src/coordinator/util.h"
#include "src/indexes/vector_base.h"
#include "src/query/neighbor_comparator.h"
#include "src/query/search.h"
#include "src/utils/cancel.h"
#include "src/utils/string_interning.h"
#include "src/valkey_search.h"
#include "valkey_search_options.h"
#include "vmsdk/src/debug.h"
#include "vmsdk/src/log.h"
#include "vmsdk/src/managed_pointers.h"
#include "vmsdk/src/status/status_macros.h"
#include "vmsdk/src/thread_pool.h"
#include "vmsdk/src/type_conversions.h"
#include "vmsdk/src/utils.h"
#include "vmsdk/src/valkey_module_api/valkey_module.h"

namespace valkey_search::query::fanout {

CONTROLLED_BOOLEAN(ForceInvalidSlotFingerprint, false);
DEV_INTEGER_COUNTER(query_stats, fanout_content_fetch_retry_count);

uint64_t EstimateContentLimit(uint64_t candidate_limit, size_t shard_count,
                              options::FanoutContentFetchMode mode) {
  if (candidate_limit == 0 || shard_count == 0 ||
      mode == options::FanoutContentFetchMode::kDisabled) {
    return candidate_limit;
  }
  const uint64_t shard_count_u64 = static_cast<uint64_t>(shard_count);
  const uint64_t fair_share = candidate_limit / shard_count_u64 +
                              (candidate_limit % shard_count_u64 != 0);
  if (mode == options::FanoutContentFetchMode::kAggressive) {
    return fair_share;
  }
  const uint64_t gap = candidate_limit - fair_share;
  return candidate_limit - gap / 4;
}

namespace {

absl::Status PerformSearchFanoutAsyncImpl(
    ValkeyModuleCtx *ctx,
    std::vector<vmsdk::cluster_map::NodeInfo> &search_targets,
    coordinator::ClientPool *coordinator_client_pool,
    std::unique_ptr<SearchParameters> parameters,
    vmsdk::ThreadPool *thread_pool, bool force_full_content,
    std::optional<std::vector<std::optional<uint64_t>>> slot_fingerprints =
        std::nullopt);

}  // namespace

// SearchPartitionResultsTracker is a thread-safe class that tracks the results
// of a query fanout. It aggregates the results from multiple nodes and returns
// the top k results to the callback.
struct SearchPartitionResultsTracker {
  absl::Mutex mutex;
  // Holds the LocalResponderSearch after it completes, keeping its
  // SearchParameters fields (return_attributes, sortby_parameter, etc.) alive.
  // Neighbors moved from the local search into `results` contain RecordsMap
  // entries whose string_view keys point into those fields. Without this, the
  // LocalResponderSearch would be destroyed immediately after adding its
  // results to the tracker, leaving dangling string_view keys that are read
  // when the priority queue reallocates (triggering absl::flat_hash_map rehash
  // on move).
  //
  // Since there can only be a single LocalResponder, this doesn't need a lock.
  //
  std::unique_ptr<SearchParameters> local_responder_;
  std::priority_queue<indexes::Neighbor, std::vector<indexes::Neighbor>,
                      NeighborComparator>
      results ABSL_GUARDED_BY(mutex);
  int outstanding_requests ABSL_GUARDED_BY(mutex);
  std::unique_ptr<SearchParameters> parameters ABSL_GUARDED_BY(mutex);
  // Error tracking
  std::atomic_bool consistency_failed{false};
  std::atomic<size_t> accumulated_total_count{0};
  std::atomic_bool has_successful_node{false};  // Whether any node succeeded
  std::atomic_bool has_node_error{false};       // Whether any node failed
  absl::Status first_node_error
      ABSL_GUARDED_BY(mutex);  // First error encountered
  std::function<void(std::unique_ptr<SearchParameters>)> content_retry;

  SearchPartitionResultsTracker(
      int outstanding_requests, int k,
      std::unique_ptr<SearchParameters> parameters,
      std::function<void(std::unique_ptr<SearchParameters>)> content_retry = {})
      : outstanding_requests(outstanding_requests),
        parameters(std::move(parameters)),
        content_retry(std::move(content_retry)) {}

  void HandleResponse(coordinator::SearchIndexPartitionResponse &response,
                      const std::string &address, const grpc::Status &status) {
    if (!status.ok()) {
      absl::MutexLock lock(&mutex);
      // Store first error for partial results disabled case
      if (!has_node_error.load()) {
        has_node_error.store(true);
        first_node_error = ToAbslStatus(status);
      }
      if (parameters->enable_consistency &&
          status.error_code() == grpc::FAILED_PRECONDITION) {
        consistency_failed.store(true);
      }
      // Cancel for consistency failures or when partial results are disabled
      bool should_cancel =
          consistency_failed.load() || !parameters->enable_partial_results;
      if (should_cancel) {
        parameters->cancellation_token->Cancel();
      }
      VMSDK_LOG_EVERY_N_SEC(DEBUG, nullptr, 1)
          << "Error during handling of FT.SEARCH on node " << address
          << ", Error code: " << status.error_code();
      return;
    }
    // Success case
    has_successful_node.store(true);
    absl::MutexLock lock(&mutex);
    accumulated_total_count.fetch_add(response.total_count(),
                                      std::memory_order_relaxed);
    while (response.neighbors_size() > 0) {
      auto neighbor_entry = std::unique_ptr<coordinator::NeighborEntry>(
          response.mutable_neighbors()->ReleaseLast());
      std::optional<RecordsMap> attribute_contents;
      if (!neighbor_entry->content_omitted()) {
        attribute_contents.emplace();
        for (const auto &attribute_content :
             neighbor_entry->attribute_contents()) {
          auto identifier =
              vmsdk::MakeUniqueValkeyString(attribute_content.identifier());
          auto identifier_view = vmsdk::ToStringView(identifier.get());
          attribute_contents->emplace(
              identifier_view,
              RecordsMapValue(
                  std::move(identifier),
                  vmsdk::MakeUniqueValkeyString(attribute_content.content())));
        }
      }
      indexes::Neighbor neighbor{
          StringInternStore::Intern(neighbor_entry->key()),
          neighbor_entry->score(), std::move(attribute_contents)};
      AddResult(neighbor);
    }
  }

  void AddResults(std::vector<indexes::Neighbor> &neighbors) {
    absl::MutexLock lock(&mutex);
    for (auto &neighbor : neighbors) {
      AddResult(neighbor);
    }
  }

  void AddTotalCount(size_t count) {
    accumulated_total_count.fetch_add(count, std::memory_order_relaxed);
  }

  void AddResult(indexes::Neighbor &neighbor)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex) {
    // For non-vector queries, we can add the result directly.
    if (parameters->IsNonVectorQuery()) {
      results.emplace(std::move(neighbor));
      return;
    }
    if (results.size() < parameters->k) {
      results.emplace(std::move(neighbor));
    } else if (NeighborComparator{}(neighbor, results.top())) {
      results.emplace(std::move(neighbor));
      results.pop();
    }
  }

  ~SearchPartitionResultsTracker() {
    std::unique_ptr<SearchParameters> completed_parameters;
    std::function<void(std::unique_ptr<SearchParameters>)> retry;
    {
      absl::MutexLock lock(&mutex);
      absl::Status status;
      if (consistency_failed) {
        // Consistency failures always take precedence
        status = absl::FailedPreconditionError(kFailedPreconditionMsg);
      } else if (has_node_error.load() &&
                 (!parameters->enable_partial_results ||
                  !has_successful_node.load())) {
        // Use first error when:
        // - Partial results disabled (any error fails the operation), OR
        // - Partial results enabled but no nodes succeeded (all failed)
        status = first_node_error;
      } else {
        // No errors detected - success case
        std::vector<indexes::Neighbor> neighbors;
        neighbors.resize(results.size());
        size_t i = neighbors.size();
        while (!results.empty()) {
          CHECK(i != 0);
          neighbors[--i] =
              std::move(const_cast<indexes::Neighbor &>(results.top()));
          results.pop();
        }
        CHECK(i == 0);
        // Note: We do not sort neighbors here because we do not have the
        // content of the local shard yet. In the SendReply function, we will
        // sort the all neighbors based on the content if sorting is required.
        // SearchResult construction automatically applies trimming based on
        // LIMIT offset count IF the command allows it (ie - it does not require
        // complete results).
        parameters->search_result = SearchResult(
            accumulated_total_count, std::move(neighbors), *parameters, true);
        status = absl::OkStatus();
        if (content_retry) {
          const auto range =
              parameters->search_result.GetSerializationRange(*parameters);
          const bool missing_content = std::any_of(
              parameters->search_result.neighbors.begin() + range.start_index,
              parameters->search_result.neighbors.begin() + range.end_index,
              [](const indexes::Neighbor &neighbor) {
                return !neighbor.attribute_contents.has_value();
              });
          if (missing_content) {
            fanout_content_fetch_retry_count.Increment();
            parameters->search_result.neighbors.clear();
            retry = std::move(content_retry);
          }
        }
      }
      parameters->search_result.status = status;
      completed_parameters = std::move(parameters);
    }
    if (retry) {
      retry(std::move(completed_parameters));
      return;
    }
    // The destructor runs on whichever thread drops the last shared_ptr
    // reference. If remote shards complete first and the local shard (which
    // completes on the main thread via content resolution) drops the last
    // reference, we'll be on the main thread here.
    if (vmsdk::IsMainThread()) {
      completed_parameters->QueryCompleteMainThread(
          std::move(completed_parameters));
    } else {
      completed_parameters->QueryCompleteBackground(
          std::move(completed_parameters));
    }
  }
};

// SearchParameters subclass for local responder (local shard in fanout).
// Handles in-flight retry completion by adding results to the tracker.
class LocalResponderSearch : public query::SearchParameters {
 public:
  std::shared_ptr<SearchPartitionResultsTracker> tracker;

  void QueryCompleteMainThread(
      std::unique_ptr<SearchParameters> self) override {
    CHECK(vmsdk::IsMainThread());
    QueryCompleteImpl(std::move(self));
  }

  void QueryCompleteBackground(
      std::unique_ptr<SearchParameters> self) override {
    CHECK(!vmsdk::IsMainThread());
    QueryCompleteImpl(std::move(self));
  }

 private:
  void QueryCompleteImpl(std::unique_ptr<SearchParameters> self) {
    if (search_result.status.ok()) {
      tracker->has_successful_node.store(true);
      tracker->AddResults(search_result.neighbors);
      tracker->AddTotalCount(search_result.total_count);
    } else {
      // Store first error for partial results disabled case
      {
        absl::MutexLock lock(&tracker->mutex);
        if (!tracker->has_node_error.load()) {
          tracker->has_node_error.store(true);
          tracker->first_node_error = search_result.status;
        }
      }
      // Only add results if partial results are enabled
      if (enable_partial_results) {
        tracker->AddResults(search_result.neighbors);
        tracker->AddTotalCount(search_result.total_count);
      }
      VMSDK_LOG_EVERY_N_SEC(DEBUG, nullptr, 1)
          << "Error during local handling of the search operation";
    }
    // Stash `self` (the LocalResponderSearch) in the tracker so that its
    // SearchParameters fields outlive the Neighbor entries moved into
    // `results` above. Those entries' RecordsMaps contain string_view keys
    // that reference data owned by this SearchParameters object.
    //
    // We must break the circular reference first: this LocalResponderSearch
    // holds a shared_ptr to the tracker, so storing `self` in the tracker
    // would create a cycle (tracker -> self -> tracker). Copy the shared_ptr
    // to a local, clear the member, then stash.
    auto tracker_copy = tracker;
    tracker.reset();
    absl::MutexLock lock(&tracker_copy->mutex);
    tracker_copy->parameters->local_responder_ = std::move(self);
  }
};

void PerformRemoteSearchRequest(
    std::unique_ptr<coordinator::SearchIndexPartitionRequest> request,
    const std::string &address,
    coordinator::ClientPool *coordinator_client_pool,
    std::shared_ptr<SearchPartitionResultsTracker> tracker,
    cancel::Token cancellation_token) {
  const uint64_t remaining_timeout_ms =
      cancellation_token->RemainingTimeoutMs();
  if (remaining_timeout_ms == 0) {
    coordinator::SearchIndexPartitionResponse response;
    tracker->HandleResponse(
        response, address,
        grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                     "Search request deadline exceeded before dispatch"));
    return;
  }
  request->set_timeout_ms(remaining_timeout_ms);
  auto client = coordinator_client_pool->GetClient(address);

  client->SearchIndexPartition(
      std::move(request),
      [tracker, address = std::string(address)](
          grpc::Status status,
          coordinator::SearchIndexPartitionResponse &response) mutable {
        tracker->HandleResponse(response, address, status);
      });
}

void PerformRemoteSearchRequestAsync(
    std::unique_ptr<coordinator::SearchIndexPartitionRequest> request,
    const std::string &address,
    coordinator::ClientPool *coordinator_client_pool,
    std::shared_ptr<SearchPartitionResultsTracker> tracker,
    vmsdk::ThreadPool *thread_pool, cancel::Token cancellation_token) {
  thread_pool->Schedule(
      [coordinator_client_pool, address = std::string(address),
       request = std::move(request), tracker,
       cancellation_token = std::move(cancellation_token)]() mutable {
        PerformRemoteSearchRequest(std::move(request), address,
                                   coordinator_client_pool, tracker,
                                   std::move(cancellation_token));
      },
      vmsdk::ThreadPool::Priority::kHigh);
}

namespace {

absl::Status PerformSearchFanoutAsyncImpl(
    ValkeyModuleCtx *ctx,
    std::vector<vmsdk::cluster_map::NodeInfo> &search_targets,
    coordinator::ClientPool *coordinator_client_pool,
    std::unique_ptr<SearchParameters> parameters,
    vmsdk::ThreadPool *thread_pool, bool force_full_content,
    std::optional<std::vector<std::optional<uint64_t>>> slot_fingerprints) {
  // Queue depth admission check is in commands.cc (before BlockedClient
  // creation) so that returning an error doesn't crash the blocked client.
  auto request = coordinator::ParametersToGRPCSearchRequest(*parameters);
  uint64_t candidate_limit;
  if (parameters->IsNonVectorQuery()) {
    candidate_limit = parameters->limit.first_index + parameters->limit.number;
  } else {
    candidate_limit = parameters->k;
  }
  // Preserve correctness by always fetching K keys/scores. SORTBY is excluded
  // because ranking requires the sort field for every candidate.
  request->mutable_limit()->set_first_index(0);
  request->mutable_limit()->set_number(candidate_limit);

  auto mode = static_cast<options::FanoutContentFetchMode>(
      options::GetFanoutContentFetchMode().GetValue());
  bool optimize_content = !force_full_content && !parameters->no_content &&
                          !parameters->RequiresCompleteResults() &&
                          candidate_limit > 0 &&
                          mode != options::FanoutContentFetchMode::kDisabled;
  if (optimize_content) {
    request->set_content_limit(
        EstimateContentLimit(candidate_limit, search_targets.size(), mode));
  } else if (force_full_content) {
    // Fetch content for every candidate returned by the shard. The shard may
    // retain a buffer beyond candidate_limit, and those candidates can become
    // part of the final result if earlier content resolution fails.
    request->clear_content_limit();
  } else {
    request->clear_content_limit();
  }

  if (!slot_fingerprints.has_value()) {
    slot_fingerprints.emplace();
    slot_fingerprints->reserve(search_targets.size());
    for (const auto &node : search_targets) {
      if (node.shard != nullptr) {
        slot_fingerprints->emplace_back(node.shard->slots_fingerprint);
      } else {
        slot_fingerprints->emplace_back(std::nullopt);
      }
    }
  }

  std::function<void(std::unique_ptr<SearchParameters>)> content_retry;
  if (optimize_content && request->content_limit() < candidate_limit) {
    auto retry_targets = search_targets;
    content_retry =
        [retry_targets = std::move(retry_targets), coordinator_client_pool,
         thread_pool, slot_fingerprints = *slot_fingerprints](
            std::unique_ptr<SearchParameters> retry_parameters) mutable {
          const uint64_t remaining_timeout_ms =
              retry_parameters->cancellation_token->RemainingTimeoutMs();
          if (remaining_timeout_ms == 0 ||
              retry_parameters->cancellation_token->IsCancelled()) {
            retry_parameters->search_result.status =
                absl::DeadlineExceededError(kTimeoutMsg);
            if (vmsdk::IsMainThread()) {
              retry_parameters->QueryCompleteMainThread(
                  std::move(retry_parameters));
            } else {
              retry_parameters->QueryCompleteBackground(
                  std::move(retry_parameters));
            }
            return;
          }
          retry_parameters->timeout_ms = remaining_timeout_ms;
          auto status = PerformSearchFanoutAsyncImpl(
              nullptr, retry_targets, coordinator_client_pool,
              std::move(retry_parameters), thread_pool,
              /*force_full_content=*/true, std::move(slot_fingerprints));
          if (!status.ok()) {
            VMSDK_LOG(WARNING, nullptr)
                << "Failed to retry FT.SEARCH with full content: " << status;
          }
        };
  }
  auto cancellation_token = parameters->cancellation_token;
  auto tracker = std::make_shared<SearchPartitionResultsTracker>(
      search_targets.size(), parameters->k, std::move(parameters),
      std::move(content_retry));
  bool has_local_target = false;
  for (size_t target_index = 0; target_index < search_targets.size();
       ++target_index) {
    auto &node = search_targets[target_index];
    if (node.is_local) {
      // Defer the local target enqueue, since it will own the parameters from
      // then on.
      has_local_target = true;
      continue;
    }
    auto request_copy =
        std::make_unique<coordinator::SearchIndexPartitionRequest>();
    request_copy->CopyFrom(*request);

    if (ForceInvalidSlotFingerprint.GetValue()) {
      // test only: set an invalid slot fingerprint and force failure
      request_copy->set_slot_fingerprint(0);
    } else if ((*slot_fingerprints)[target_index].has_value()) {
      request_copy->set_slot_fingerprint(*(*slot_fingerprints)[target_index]);
    }

    // At 30 requests, it takes ~600 micros to enqueue all the requests.
    // Putting this into the background thread pool will save us time on
    // machines with multiple cores.
    std::string target_address =
        absl::StrCat(node.socket_address.primary_endpoint, ":",
                     coordinator::GetCoordinatorPort(node.socket_address.port));
    if (search_targets.size() >=
            valkey_search::options::GetAsyncFanoutThreshold().GetValue() &&
        thread_pool->Size() > 1) {
      PerformRemoteSearchRequestAsync(std::move(request_copy), target_address,
                                      coordinator_client_pool, tracker,
                                      thread_pool, cancellation_token);
    } else {
      PerformRemoteSearchRequest(std::move(request_copy), target_address,
                                 coordinator_client_pool, tracker,
                                 cancellation_token);
    }
  }
  if (has_local_target) {
    auto local_parameters = std::make_unique<LocalResponderSearch>();
    VMSDK_RETURN_IF_ERROR(coordinator::GRPCSearchRequestToParameters(
        *request, nullptr, local_parameters.get()));
    local_parameters->tracker = tracker;
    VMSDK_RETURN_IF_ERROR(query::SearchAsync(std::move(local_parameters),
                                             thread_pool, SearchMode::kLocal))
        << "Failed to handle FT.SEARCH locally during fan-out";
  }
  return absl::OkStatus();
}

}  // namespace

absl::Status PerformSearchFanoutAsync(
    ValkeyModuleCtx *ctx,
    std::vector<vmsdk::cluster_map::NodeInfo> &search_targets,
    coordinator::ClientPool *coordinator_client_pool,
    std::unique_ptr<SearchParameters> parameters,
    vmsdk::ThreadPool *thread_pool) {
  return PerformSearchFanoutAsyncImpl(
      ctx, search_targets, coordinator_client_pool, std::move(parameters),
      thread_pool, /*force_full_content=*/false);
}

bool IsSystemUnderLowUtilization() {
  // Get the configured threshold (queue wait time in milliseconds)
  double threshold = static_cast<double>(
      valkey_search::options::GetLocalFanoutQueueWaitThreshold().GetValue());

  auto &valkey_search_instance = ValkeySearch::Instance();
  auto reader_pool = valkey_search_instance.GetReaderThreadPool();

  if (!reader_pool) {
    return false;
  }

  // Get recent queue wait time (not global average)
  auto queue_wait_result = reader_pool->GetRecentQueueWaitTime();
  if (!queue_wait_result.ok()) {
    // If we can't get queue wait time, assume high utilization for safety
    return false;
  }

  double queue_wait_time = queue_wait_result.value();
  // System is under low utilization if queue wait time is below threshold
  return queue_wait_time < threshold;
}

}  // namespace valkey_search::query::fanout
