// Split a read request so that each cache entry is exactly one Parquet column
// chunk, instead of a fixed-size block.
//
// This is the addition; the fixed-block path in InMemoryCacheReader is
// untouched and still handles every file with no registered layout. The two
// produce the same kind of output -- a list of CacheReadChunk, each naming a
// cache entry and the slice of it to copy -- so the reader below is identical
// either way and only the partitioning differs.

#pragma once

#include "cache_read_chunk.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

struct ColumnChunkPlanRequest {
	// Destination for the first requested byte.
	char *buffer = nullptr;
	idx_t requested_start_offset = 0;
	idx_t requested_bytes_to_read = 0;
	idx_t file_size = 0;
	// Cap on the size of a synthesised entry for a region no extent covers
	// (the footer, mainly). Extents themselves are never split.
	idx_t max_gap_entry_size = 0;
};

// Requires a layout registered for [path]; the caller checks that first and
// uses the fixed-block path otherwise.
vector<CacheReadChunk> PlanColumnChunks(const ColumnChunkPlanRequest &request, const string &path);

} // namespace duckdb
