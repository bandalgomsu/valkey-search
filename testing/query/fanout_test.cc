/*
 * Copyright (c) 2026, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 */

#include "src/query/fanout.h"

#include <cstdint>
#include <limits>

#include "gtest/gtest.h"
#include "src/valkey_search_options.h"

namespace valkey_search::query::fanout {
namespace {

using options::FanoutContentFetchMode;

TEST(FanoutContentLimitEstimatorTest, DisabledAlwaysFetchesAllContent) {
  EXPECT_EQ(EstimateContentLimit(100, 16, FanoutContentFetchMode::kDisabled),
            100);
}

TEST(FanoutContentLimitEstimatorTest, ConservativeRetainsMostContent) {
  EXPECT_EQ(EstimateContentLimit(100, 4, FanoutContentFetchMode::kConservative),
            82);
  EXPECT_EQ(EstimateContentLimit(3, 2, FanoutContentFetchMode::kConservative),
            3);
}

TEST(FanoutContentLimitEstimatorTest, AggressiveUsesCeilingFairShare) {
  EXPECT_EQ(EstimateContentLimit(100, 16, FanoutContentFetchMode::kAggressive),
            7);
  EXPECT_EQ(EstimateContentLimit(3, 8, FanoutContentFetchMode::kAggressive), 1);
}

TEST(FanoutContentLimitEstimatorTest, HandlesDegenerateAndLargeInputs) {
  EXPECT_EQ(EstimateContentLimit(0, 4, FanoutContentFetchMode::kAggressive), 0);
  EXPECT_EQ(EstimateContentLimit(10, 0, FanoutContentFetchMode::kAggressive),
            10);
  EXPECT_EQ(EstimateContentLimit(std::numeric_limits<uint64_t>::max(), 2,
                                 FanoutContentFetchMode::kAggressive),
            uint64_t{1} << 63);
}

}  // namespace
}  // namespace valkey_search::query::fanout
