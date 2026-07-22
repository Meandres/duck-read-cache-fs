// `cache_httpfs_policy_stats()` -- per-group counters for the byte-bounded
// 'policy' in-memory cache.
//
// The stock `cache_httpfs_cache_access_info_query()` reports one aggregate
// hit/miss pair, which cannot answer either of the questions this fork is for:
// whether entries sized to column chunks beat a fixed grid, and whether a
// per-file policy beats a global one. Both need per-group counters plus the
// entry-size distribution, since the whole point of variable-sized entries is
// that size varies.

#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

TableFunction GetPolicyStatsQueryFunc();

} // namespace duckdb
