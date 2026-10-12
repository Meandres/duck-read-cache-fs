// Split a read request so that each cache entry is exactly one Parquet column chunk, instead of a fixed-size block.
//
// The output is the same `CacheReadChunk` list the fixed-block path builds, so the reader executes either one
// identically; only the partitioning differs.
//
// - A column chunk is one entry, never split: a later read of another slice of it is then a hit.
// - Bytes outside any chunk (the footer, mostly) are cut on the fixed block grid, clipped to the gap. Entries in a gap
//   therefore depend only on the offset, not on where a request happened to start, so two requests into the same gap
//   always map to the same entry instead of to two overlapping ones.

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
	// Grid used to cut bytes that no column chunk covers.
	idx_t gap_block_size = 0;
};

// Returns chunks covering [requested_start_offset, requested_start_offset + requested_bytes_to_read) exactly, in order
// and without gaps; or an empty vector when the request cannot be planned (no layout registered for [path], or the
// request reaches past the end of the file), in which case the caller must use the fixed-block path. Never returns a
// plan covering only part of the request: executing one would leave the rest of the caller's buffer unfilled.
vector<CacheReadChunk> PlanColumnChunks(const ColumnChunkPlanRequest &request, const string &path);

} // namespace duckdb
