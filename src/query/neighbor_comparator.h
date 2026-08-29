/*
 * Copyright (c) 2026, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 */

#ifndef VALKEYSEARCH_SRC_QUERY_NEIGHBOR_COMPARATOR_H_
#define VALKEYSEARCH_SRC_QUERY_NEIGHBOR_COMPARATOR_H_

#include "src/indexes/vector_base.h"

namespace valkey_search::query {

// Returns true when a should precede b in the merged result order. The
// priority_queue consequently keeps the worst candidate at top(), while
// partial_sort puts the candidates most likely to survive the merge first.
struct NeighborComparator {
  bool operator()(const indexes::Neighbor &a,
                  const indexes::Neighbor &b) const {
    if (a.distance != b.distance) {
      return a.distance < b.distance;
    }
    return a.external_id->Str() > b.external_id->Str();
  }
};

}  // namespace valkey_search::query

#endif  // VALKEYSEARCH_SRC_QUERY_NEIGHBOR_COMPARATOR_H_
